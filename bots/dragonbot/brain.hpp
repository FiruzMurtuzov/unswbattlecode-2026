// Decides one turn: which steps to take, or whether to split.
//
// Every legal move of up to a few steps is simulated exactly (own body,
// pearls, paid steps) and scored on
//   * pearls eaten on the way, and a value field for what is left: pearls,
//     beds about to spawn, unexplored tiles, the last enemy queen sighting;
//   * room left to live in, from a flood fill that knows when body tiles free up;
//   * enemy heads that could ram ours next turn (a queen weighs this heavily).
// A separate search looks for a path straight into the enemy queen's head:
// a head-on collision kills both dragons, and a team whose queen is dead
// loses the round-500 tiebreak, so any worker trades itself for that.
#pragma once

#include "config.hpp"
#include "world.hpp"

#include <cmath>
#include <queue>

namespace bot {

class Brain {
  public:
    explicit Brain(World& world) : w(world) {}

    std::string decide() {
        setup();
        out_.clear();

        bestScore_ = -1e18;
        bestSteps_.clear();
        bestBlind_ = false;
        bestRoom_ = 0;
        attacking_ = false;

        searchMoves();
        considerAttacks();

        int split = chooseSplit();
        if (split > 0) {
            w.applySplit(split);
            out_ += "SPLIT " + std::to_string(split) + "\n";
            label("split " + std::to_string(split));
            return out_;
        }

        if (bestSteps_.empty()) {
            bestSteps_.push_back(lastResort());
            label("doomed");
        }
        if (bestBlind_) {  // blind steps are only ever a single step
            w.blindKey = w.edgeKey(w.head, bestSteps_[0]);
            w.blindDir = bestSteps_[0];
        }
        w.applyMove(bestSteps_);
        out_ += "MOVE ";
        for (int d : bestSteps_) out_ += DIR_CHAR[d];
        out_ += "\n";
        return out_;
    }

  private:
    World& w;
    std::string out_;
    std::string label_;

    struct Enemy {
        int id, head, length, freeSteps, reach;
        bool queen;
        std::vector<int> dist;
    };
    std::vector<Enemy> enemies_;
    std::vector<int> distMe_;
    std::vector<int> otherFree_;  // steps until a tile held by another dragon frees up
    std::vector<int> queue_;
    int myQueenHead_ = -1;        // our queen's head, if in view this turn

    // Value field: best few (value, source) pairs per tile from distinct targets.
    static constexpr int K = 3;
    std::vector<std::array<float, K>> fieldVal_;
    std::vector<std::array<int, K>> fieldSrc_;
    std::vector<uint8_t> fieldCount_;

    // Search state.
    std::deque<int> sb_;
    int sMissing_ = 0, sLen_ = 0, sPearls_ = 0, sPaid_ = 0, freeSteps_ = 1, maxDepth_ = 1, nodes_ = 0;
    std::vector<uint8_t> mine_;
    std::vector<int> bodyTurn_;  // == w.stamp on tiles our body held at the start of the turn
    std::vector<uint8_t> eatenNow_;
    std::vector<int> eaten_;
    std::array<int, 32> steps_{};
    double bestScore_ = -1e18;
    std::vector<int> bestSteps_;
    bool bestBlind_ = false;
    int lastRoom_ = 0, bestRoom_ = 0;  // flood-fill room after the move
    int lastKnown_ = 0;
    int nearRoom_ = 0;                 // tiles within NEAR_DEPTH steps, from the last fill
    static constexpr int NEAR_DEPTH = 3;
    bool attacking_ = false;

    // Flood fill scratch.
    std::vector<int> ffStamp_, ffDist_, bodyStamp_, bodyFree_;
    int ffMark_ = 0;

    // ------------------------------------------------------------ setup

    void setup() {
        int n = w.N;
        if (static_cast<int>(otherFree_.size()) != n) {
            otherFree_.assign(n, 0);
            distMe_.assign(n, INF);
            mine_.assign(n, 0);
            bodyTurn_.assign(n, 0);
            eatenNow_.assign(n, 0);
            ffStamp_.assign(n, 0);
            ffDist_.assign(n, 0);
            bodyStamp_.assign(n, 0);
            bodyFree_.assign(n, 0);
            fieldVal_.assign(n, {});
            fieldSrc_.assign(n, {});
            fieldCount_.assign(n, 0);
            queue_.reserve(n);
        }

        std::fill(otherFree_.begin(), otherFree_.end(), 0);
        for (const Seen& s : w.dragons) {
            if (s.id == w.myId) continue;
            for (int i = 0; i < static_cast<int>(s.chain.size()); i++)
                otherFree_[s.chain[i]] = std::max(2, s.length - i + 1);
        }
        // Segments we could not place in a chain stay put as far as we know.
        for (int t : w.vision)
            if (w.occupant(t) >= 0 && w.occupant(t) != w.myId && otherFree_[t] == 0) otherFree_[t] = 40;
        // Out of view, a dragon seen there a moment ago is probably still
        // around: a corridor someone just walked into is not free.
        for (int t = 0; t < w.N; t++)
            if (!w.inView(t) && w.round - w.ghostRound[t] <= GHOST_ROUNDS) otherFree_[t] = std::max(otherFree_[t], 3);

        myQueenHead_ = -1;
        if (!w.isQueen) {
            if (Seen* q = w.seen(w.myQueen); q && q->team == w.myTeam) myQueenHead_ = q->head;
        }

        bfs(w.head, distMe_, INF, true);

        enemies_.clear();
        for (const Seen& s : w.dragons) {
            if (s.team == w.myTeam || s.head < 0) continue;
            Enemy e;
            e.id = s.id;
            e.head = s.head;
            e.length = s.length;
            e.queen = s.id == w.enemyQueen;
            e.freeSteps = (s.length + 3) / 4;
            e.reach = e.freeSteps + std::max(0, s.length - 2);
            e.dist.assign(w.N, INF);
            bfs(s.head, e.dist, std::min(e.reach + 2, 16), false);
            enemies_.push_back(std::move(e));
        }

        buildField();
    }

    // Breadth-first distances from `from`, around kelp and other dragons.
    // Heads count as reachable (a head-on collision), but nothing goes on past them.
    void bfs(int from, std::vector<int>& dist, int limit, bool passOwnBody) {
        std::fill(dist.begin(), dist.end(), INF);
        queue_.clear();
        dist[from] = 0;
        queue_.push_back(from);
        for (size_t qi = 0; qi < queue_.size(); qi++) {
            int u = queue_[qi];
            if (dist[u] >= limit) continue;
            for (int d = 0; d < 4; d++) {
                int v = w.step(u, d);
                if (v < 0 || dist[v] != INF) continue;
                int o = w.occupant(v);
                if (o >= 0 && !w.headAt[v]) {
                    if (!(passOwnBody && o == w.myId)) continue;
                }
                dist[v] = dist[u] + 1;
                if (o >= 0 && !(passOwnBody && o == w.myId)) continue;  // a head: reachable, not passable
                queue_.push_back(v);
            }
        }
    }

    // ------------------------------------------------------------ targets

    bool enemyHalf(int t) const {
        int away = w.mirror(w.home);
        return w.torusDist(t, away) < w.torusDist(t, w.home);
    }

    void buildField() {
        struct Item {
            float value;
            int tile, src;
            bool operator<(const Item& o) const { return value < o.value; }
        };
        std::priority_queue<Item> pq;
        double stepsPerTurn = std::max(1, (w.length + 3) / 4);

        auto contested = [&](int t, double v) {
            for (const Enemy& e : enemies_) {
                if (e.dist[t] < distMe_[t]) return v * CONTESTED_FACTOR;
            }
            return v;
        };

        for (int t = 0; t < w.N; t++) {
            fieldCount_[t] = 0;
            double v = 0;
            int seenAt = w.lastSeen[t];
            if (seenAt < 0) {
                v = (!w.isQueen && enemyHalf(t)) ? HUNT_EXPLORE_VALUE : EXPLORE_VALUE;
            } else if (w.pearl[t]) {
                int age = w.round - seenAt;
                v = PEARL_VALUE * (age == 0 ? 1.0 : std::max(0.25, 1.0 - age / 80.0));
            } else if (w.bedTimer[t] >= 0) {
                int c = w.bedCountdown(t);
                if (c <= 0) {
                    int age = w.round - seenAt;
                    v = PEARL_VALUE * 0.5 * std::max(0.3, 1.0 - age / 150.0);
                } else {
                    double arrive = distMe_[t] >= INF ? 50.0 : distMe_[t] / stepsPerTurn;
                    double wait = std::max(0.0, c - arrive);
                    v = PEARL_VALUE * 0.85 - 1.5 * wait;
                }
            }
            if (v > 0) v = contested(t, v);
            if (v > 0.5) pq.push({static_cast<float>(v), t, t});
        }

        if (!w.isQueen && w.enemyQueenTile >= 0) {
            int age = w.round - w.enemyQueenRound;
            if (age <= 40) {
                double v = QUEEN_SIGHTING_VALUE * (1.0 - age / 40.0) + 1.0;
                pq.push({static_cast<float>(v), w.enemyQueenTile, w.N + w.enemyQueenTile});
            }
        }

        while (!pq.empty()) {
            Item it = pq.top();
            pq.pop();
            int t = it.tile;
            int c = fieldCount_[t];
            if (c >= K) continue;
            bool dup = false;
            for (int i = 0; i < c; i++)
                if (fieldSrc_[t][i] == it.src) dup = true;
            if (dup) continue;
            fieldVal_[t][c] = it.value;
            fieldSrc_[t][c] = it.src;
            fieldCount_[t] = static_cast<uint8_t>(c + 1);
            if (it.value <= 1.0f) continue;
            if (t != it.src && t + w.N != it.src && otherFree_[t] > 0) continue;
            for (int d = 0; d < 4; d++) {
                int v = w.step(t, d);
                if (v < 0 || fieldCount_[v] >= K) continue;
                if (otherFree_[v] > 0 && !w.headAt[v]) continue;
                pq.push({it.value - 1.0f, v, it.src});
            }
        }
    }

    double fieldAt(int t) const {
        for (int i = 0; i < fieldCount_[t]; i++) {
            int src = fieldSrc_[t][i];
            if (src < w.N && eatenNow_[src]) continue;
            return fieldVal_[t][i];
        }
        return 0.0;
    }

    // ------------------------------------------------------------ scoring

    double threatAt(int h) const {
        double t = 0;
        for (const Enemy& e : enemies_) {
            int d = e.dist[h];
            bool sees = w.cheb(e.head, h) <= VISION_R;
            if (w.isQueen) {
                if (d <= 1) t += QUEEN_THREAT_ADJ;
                else if (sees && d <= e.freeSteps) t += QUEEN_THREAT_FREE;
                else if (sees && d <= e.reach) t += QUEEN_THREAT_DASH;
                if (sees) t += QUEEN_THREAT_NEAR;
            } else {
                if (d <= 1) t += WORKER_THREAT_ADJ;
                else if (sees && d <= e.freeSteps) t += WORKER_THREAT_FREE;
                else if (sees && d <= e.reach) t += WORKER_THREAT_DASH;
            }
        }
        return t;
    }

    // Tiles the head can still reach, counting a body tile as open once the
    // body has had time to slide off it. Stops counting at `cap`. With
    // `knownOnly`, tiles never seen count as walls.
    int floodSpace(int h, int cap, bool knownOnly = false) {
        ffMark_++;
        // Segment i leaves its tile with the (len - i)th step, and the engine
        // checks the whole body before each step, so the earliest step onto
        // that tile is number len - i + 1.
        for (int i = 0; i < static_cast<int>(sb_.size()); i++) {
            bodyStamp_[sb_[i]] = ffMark_;
            bodyFree_[sb_[i]] = sLen_ - i + 1;
        }
        queue_.clear();
        queue_.push_back(h);
        ffStamp_[h] = ffMark_;
        ffDist_[h] = 0;
        int count = 1;
        nearRoom_ = 1;
        for (size_t qi = 0; qi < queue_.size(); qi++) {
            int u = queue_[qi];
            int du = ffDist_[u];
            if (du < NEAR_DEPTH) nearRoom_ = static_cast<int>(qi) + 1;
            for (int d = 0; d < 4; d++) {
                int v = w.step(u, d);
                if (v < 0 || ffStamp_[v] == ffMark_) continue;
                if (knownOnly && w.lastSeen[v] < 0) continue;
                int block = otherFree_[v];
                if (bodyStamp_[v] == ffMark_) block = std::max(block, bodyFree_[v]);
                if (du + 1 < block) continue;
                ffStamp_[v] = ffMark_;
                ffDist_[v] = du + 1;
                if (du + 1 <= NEAR_DEPTH) nearRoom_ = count + 1;
                if (++count >= cap) return count;
                queue_.push_back(v);
            }
        }
        return count;
    }

    // Room a dragon of this length wants in front of it: enough to follow
    // its own tail for a while, capped so a huge dragon on a small map is
    // not forever "trapped".
    static int roomNeeded(int len) { return std::min(len + 2, 30); }

    double spacePenalty(int h) {
        int need = roomNeeded(sLen_);
        int comfy = std::min(std::max(2 * sLen_ + 4, 24), 60);
        int cnt = floodSpace(h, comfy);
        lastRoom_ = cnt;
        double pen = 0;
        // Single-file passages commit a dragon for many turns, and anyone
        // can plug the far end. Prefer open ground.
        int open = std::min(nearRoom_, 12);
        pen += (w.isQueen ? QUEEN_OPEN_WEIGHT : OPEN_WEIGHT) * (12 - open);
        if (cnt < need) pen += TRAPPED_PENALTY * (1.0 + static_cast<double>(need - cnt) / need);
        if (cnt < comfy) pen += CRAMPED_WEIGHT * (comfy - cnt) * 24.0 / comfy;
        // The fill above takes unseen ground to be open. Walking a long body
        // into ground we cannot see is how dead ends get us, so also ask how
        // much room is certain.
        int known = cnt < need ? cnt : floodSpace(h, need, true);
        lastKnown_ = known;
        if (known < need) pen += KNOWN_ROOM_WEIGHT * (need - known);
        return pen;
    }

    // Room left to the other heads near us once our body has moved: boxing
    // in an enemy is good (it has to crash), boxing in a friend is not.
    double neighbourRoom(int h) {
        double s = 0;
        for (const Seen& o : w.dragons) {
            if (o.id == w.myId || o.head < 0 || w.cheb(o.head, h) > VISION_R) continue;
            int need = o.length + 2;
            int cnt = floodSpace(o.head, need);
            if (cnt >= need) continue;
            double frac = 1.0 + static_cast<double>(need - cnt) / need;
            if (o.team == w.myTeam) s -= (o.id == w.myQueen ? TRAP_MY_QUEEN : TRAP_ALLY) * frac;
            else s += (o.id == w.enemyQueen ? TRAP_ENEMY_QUEEN : TRAP_ENEMY) * frac;
        }
        return s;
    }

    double evaluate(int nsteps, bool blind) {
        int h = sb_.front();
        double s = 0;
        lastRoom_ = blind ? 1 : 0;
        s += sPearls_ * PEARL_VALUE;
        s -= sPaid_ * PEARL_VALUE;
        if (!blind) {
            s += fieldAt(h);
            s -= threatAt(h);
            s -= spacePenalty(h);
            s += neighbourRoom(h);
        } else {
            s -= 300;  // we cannot see where it comes out
        }
        if (myQueenHead_ >= 0 && w.cheb(h, myQueenHead_) <= 1) s -= 20;
        // Small, per-dragon tie breaks so allies do not mirror each other.
        s += 0.05 * (steps_[0] == w.facing);
        uint32_t x = static_cast<uint32_t>(h * 2654435761u) ^ static_cast<uint32_t>(w.myId * 40503u + w.round * 977u);
        x ^= x >> 13;
        s += (x % 1000) * 1e-5;
        (void)nsteps;
        return s;
    }

    // ------------------------------------------------------------ move search

    int popTail() {
        if (sMissing_ > 0) {
            sMissing_--;
            return -2;
        }
        int t = sb_.back();
        sb_.pop_back();
        mine_[t]--;
        return t;
    }

    void unpop(int t) {
        if (t == -2) {
            sMissing_++;
            return;
        }
        sb_.push_back(t);
        mine_[t]++;
    }

    void consider(int nsteps, bool blind) {
        double s = evaluate(nsteps, blind);
#ifdef TRACE_ROUND
        if (w.round == TRACE_ROUND) {
            std::fprintf(stderr, "cand ");
            for (int i = 0; i < nsteps; i++) std::fputc(DIR_CHAR[steps_[i]], stderr);
            std::fprintf(stderr, " score %.1f room %d known %d field %.1f threat %.1f pearls %d paid %d\n", s, lastRoom_, lastKnown_,
                         blind ? 0.0 : fieldAt(sb_.front()), blind ? 0.0 : threatAt(sb_.front()), sPearls_, sPaid_);
        }
#endif
        if (s > bestScore_) {
            bestScore_ = s;
            bestSteps_.assign(steps_.begin(), steps_.begin() + nsteps);
            bestBlind_ = blind;
            bestRoom_ = lastRoom_;
        }
    }

    void dfs(int depth) {
        if (++nodes_ > SEARCH_NODE_CAP) return;
        int h = sb_.front();
        for (int d = 0; d < 4; d++) {
            int nt = w.step(h, d);
            if (nt == STEP_WALL) continue;
            bool pay = depth >= freeSteps_;
            if (pay && sLen_ <= 2) continue;
            if (nt == STEP_BLIND) {
                if (depth == 0) {
                    steps_[0] = d;
                    consider(1, true);
                }
                continue;
            }
            if (!w.inView(nt) || mine_[nt]) continue;
            // Another dragon: a body kills us, a head kills both. Our own
            // tail tiles count as free once the simulated body has left them.
            int o = w.occupant(nt);
            if (o >= 0 && !(o == w.myId && bodyTurn_[nt] == w.stamp)) continue;

            sb_.push_front(nt);
            mine_[nt]++;
            bool ate = w.pearlNow(nt) && !eatenNow_[nt];
            int popped = -3, paidPop = -3;
            if (ate) {
                eatenNow_[nt] = 1;
                eaten_.push_back(nt);
                sLen_++;
                sPearls_++;
            } else {
                popped = popTail();
            }
            if (pay) {
                paidPop = popTail();
                sLen_--;
                sPaid_++;
            }
            steps_[depth] = d;

            consider(depth + 1, false);
            if (depth + 1 < maxDepth_) dfs(depth + 1);

            if (pay) {
                unpop(paidPop);
                sLen_++;
                sPaid_--;
            }
            if (ate) {
                eatenNow_[nt] = 0;
                eaten_.pop_back();
                sLen_--;
                sPearls_--;
            } else {
                unpop(popped);
            }
            mine_[nt]--;
            sb_.pop_front();
        }
    }

    void loadBody() {
        for (int t : sb_) mine_[t] = 0;
        sb_ = w.body;
        for (int t : sb_) {
            mine_[t] = 1;
            bodyTurn_[t] = w.stamp;
        }
        sMissing_ = w.missing;
        sLen_ = w.length;
        sPearls_ = 0;
        sPaid_ = 0;
        nodes_ = 0;
    }

    void searchMoves() {
        loadBody();
        freeSteps_ = (w.length + 3) / 4;
        maxDepth_ = std::min(freeSteps_, MAX_SEARCH_STEPS);
        dfs(0);
        if (bestSteps_.empty() || bestRoom_ < roomNeeded(w.length)) {
            // Everything free is bad: see whether paying a segment or two escapes.
            int deeper = std::min(freeSteps_ + std::max(0, w.length - 2), MAX_SEARCH_STEPS + 2);
            if (deeper > maxDepth_) {
                maxDepth_ = deeper;
                nodes_ = 0;
                dfs(0);
            }
        }
        for (int t : sb_) mine_[t] = 0;
        sb_.clear();
        label("move " + std::to_string(static_cast<int>(bestScore_)));
    }

    // ------------------------------------------------------------ attacks

    // Shortest legal path that ends with our head on `target`, within `limit` steps.
    bool pathInto(int target, int limit, std::vector<int>& path) {
        std::vector<int> dist(w.N, INF);
        queue_.clear();
        dist[target] = 0;
        queue_.push_back(target);
        for (size_t qi = 0; qi < queue_.size(); qi++) {
            int u = queue_[qi];
            if (dist[u] >= limit) continue;
            for (int d = 0; d < 4; d++) {
                int v = w.step(u, d);
                if (v < 0 || dist[v] != INF || !w.inView(v)) continue;  // only ground we can see
                int o = w.occupant(v);
                if (o >= 0 && o != w.myId) continue;
                dist[v] = dist[u] + 1;
                queue_.push_back(v);
            }
        }
        if (dist[w.head] > limit) return false;

        loadBody();
        path.clear();
        bool found = attackDfs(target, dist, 0, limit, path);
        for (int t : sb_) mine_[t] = 0;
        sb_.clear();
        return found;
    }

    bool attackDfs(int target, const std::vector<int>& dist, int depth, int limit, std::vector<int>& path) {
        if (++nodes_ > SEARCH_NODE_CAP) return false;
        int h = sb_.front();
        for (int d = 0; d < 4; d++) {
            int nt = w.step(h, d);
            if (nt < 0) continue;
            if (dist[nt] != dist[h] - 1) continue;  // shortest paths only
            bool pay = depth >= freeSteps_;
            if (pay && sLen_ <= 2) continue;
            if (nt == target) {
                path.push_back(d);
                return true;
            }
            if (mine_[nt] || w.occupant(nt) >= 0 || depth + 1 >= limit) continue;
            sb_.push_front(nt);
            mine_[nt]++;
            bool ate = w.pearlNow(nt) && !eatenNow_[nt];
            int popped = -3, paidPop = -3;
            if (ate) {
                eatenNow_[nt] = 1;
                sLen_++;
            } else {
                popped = popTail();
            }
            if (pay) {
                paidPop = popTail();
                sLen_--;
            }
            path.push_back(d);
            bool ok = attackDfs(target, dist, depth + 1, limit, path);
            if (!ok) path.pop_back();
            if (pay) {
                unpop(paidPop);
                sLen_++;
            }
            if (ate) {
                eatenNow_[nt] = 0;
                sLen_--;
            } else {
                unpop(popped);
            }
            mine_[nt]--;
            sb_.pop_front();
            if (ok) return true;
        }
        return false;
    }

    void considerAttacks() {
        int myReach = (w.length + 3) / 4 + std::max(0, w.length - 2);
        bool desperate = doomed();
        for (const Enemy& e : enemies_) {
            if (e.dist[w.head] > myReach + 1 && distMe_[e.head] > myReach) continue;
            double value = 0;
            if (e.queen && !w.isQueen) {
                value = KILL_QUEEN_VALUE;
            } else if (!w.isQueen) {
                // A trade: their dragon for ours.
                if (myQueenHead_ >= 0 && e.dist[myQueenHead_] <= e.reach + 1) value += KILL_GUARD_VALUE;
                value += (e.length - w.length) * KILL_LENGTH_VALUE * 2.0;
                value -= 40;  // a live worker is worth something too
            }
            if (desperate) value = std::max(value, 1.0 + e.length);  // dying anyway: take one along
            if (value <= 0) continue;
            double score = value;
            if (score <= bestScore_) continue;

            std::vector<int> path;
            freeSteps_ = (w.length + 3) / 4;
            if (!pathInto(e.head, myReach, path)) continue;
            int paid = std::max(0, static_cast<int>(path.size()) - freeSteps_);
            score -= paid * 0.5;
            if (score > bestScore_) {
                bestScore_ = score;
                bestSteps_ = path;
                bestBlind_ = false;
                attacking_ = true;
                label(std::string(e.queen ? "KILL QUEEN " : "ram ") + std::to_string(e.id));
            }
        }
    }

    // ------------------------------------------------------------ splitting

    // Every step kills us. Die without taking a friend along: a wall or an
    // enemy body costs only us, an ally's head would cost two of ours.
    int lastResort() const {
        int best = w.facing, bestRank = -100;
        for (int d = 0; d < 4; d++) {
            int t = w.step(w.head, d);
            int rank = 0;
            if (t < 0) {
                rank = 2;
            } else {
                int o = w.occupant(t);
                bool ally = o >= 0 && o != w.myId && w.isAlly(o);
                if (o == w.myId) rank = 2;
                else if (o < 0) rank = 1;  // out of view: unknown
                else if (!ally) rank = w.headAt[t] ? 3 : 2;
                else rank = w.headAt[t] ? -10 : -1;
            }
            if (rank > bestRank) {
                bestRank = rank;
                best = d;
            }
        }
        return best;
    }

    // No move leaves more than a couple of tiles to live in.
    bool doomed() const { return bestSteps_.empty() || bestRoom_ <= 3; }

    // Room around the head right now, with the body where it is.
    int roomNow(int cap) {
        loadBody();
        int cnt = floodSpace(w.head, cap);
        for (int t : sb_) mine_[t] = 0;
        sb_.clear();
        return cnt;
    }

    // Can a child of `n` tail segments move off next turn?
    bool childCanMove(int n) const {
        if (w.missing > 0 || static_cast<int>(w.body.size()) < w.length) return false;
        int L = w.length;
        int childHead = w.body[L - 1];
        for (int d = 0; d < 4; d++) {
            int t = w.step(childHead, d);
            if (t < 0 || !w.inView(t)) continue;
            if (w.occupant(t) >= 0) continue;
            bool inBody = false;
            for (int i = 0; i < L; i++)
                if (w.body[i] == t) inBody = true;
            if (!inBody) return true;
        }
        (void)n;
        return false;
    }

    // Trapped: splitting is the only action that keeps the head still, and
    // a shorter body clears out of the way sooner. Find the smallest split
    // that leaves the head enough room to wriggle out.
    int escapeSplit() {
        int L = w.length;
        int known = static_cast<int>(w.body.size());
        for (int n = 2; n <= L - 2; n++) {
            int keep = L - n;
            if (keep > known) continue;
            loadBody();
            while (static_cast<int>(sb_.size()) > keep) {
                mine_[sb_.back()] = 0;
                sb_.pop_back();
            }
            sMissing_ = 0;
            sLen_ = keep;
            int room = floodSpace(w.head, roomNeeded(keep));
            for (int t : sb_) mine_[t] = 0;
            sb_.clear();
            if (room >= roomNeeded(keep) && room > bestRoom_ && childCanMove(n)) return n;
        }
        // No split saves the head. Save the body instead: the tail end walks
        // away as a new dragon and keeps most of the length on the board.
        if ((bestSteps_.empty() || bestRoom_ <= 2) && childCanMove(L - 2)) return L - 2;
        return 0;
    }

    int chooseSplit() {
        int L = w.length;
        bool room = w.unitCount < w.unitLimit && !attacking_;
        if (!room || L < 4) return 0;

        // Nothing we can do moves us anywhere safe: splitting is the one
        // action that keeps the head still. A queen gives up as little as it can.
        if (bestSteps_.empty() || bestRoom_ <= std::max(2, roomNeeded(L) / 3)) {
            int n = escapeSplit();
            if (n > 0) return n;
        }

        if (w.round > SPLIT_LAST_ROUND) return 0;
        if (threatAt(w.head) > 0) return 0;
        for (const Enemy& e : enemies_) {
            if (w.cheb(e.head, w.head) <= VISION_R + 1) return 0;
        }

        int n = 0;
        if (!w.isQueen && L >= WORKER_SPLIT_LEN) n = L / 2;
        if (w.isQueen && w.unitCount <= QUEEN_SPLIT_MAX_UNITS && L >= QUEEN_SPLIT_LEN) n = QUEEN_SPLIT_SIZE;
        // Two dragons need the room of one long one, and the child its own way out.
        if (n > 0 && L - n >= 2 && childCanMove(n) && roomNow(2 * L + 8) >= 2 * L + 8) return n;
        return 0;
    }

    void label(const std::string& text) {
#if BOT_DEBUG
        label_ = text;
        out_ += "INDICATOR " + std::string(w.isQueen ? "Q " : "") + text + "\n";
#else
        (void)text;
#endif
    }
};

} // namespace bot
