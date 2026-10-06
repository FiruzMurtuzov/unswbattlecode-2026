// What one dragon knows: the protocol parser, the board geometry (wrapping,
// kelp and portals), everything it has seen so far, and its own body.
//
// Every dragon is its own process with a 7x7 window, so all of this is
// rebuilt from scratch by a dragon born from a split.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bot {

constexpr int DX[4] = {0, 1, 0, -1};
constexpr int DY[4] = {-1, 0, 1, 0};
constexpr char DIR_CHAR[4] = {'N', 'E', 'S', 'W'};

inline int opposite(int d) { return d ^ 2; }

inline int dirIndex(char c) {
    switch (c) {
    case 'N': return 0;
    case 'E': return 1;
    case 'S': return 2;
    default: return 3;
    }
}

constexpr int VISION_R = 3;
constexpr int VISION = 7;
constexpr int MAX_ROUNDS = 500;
constexpr int INF = 1 << 28;

// Edge codes kept per edge; a code >= 0 is a portal id.
constexpr int EDGE_UNKNOWN = -3;
constexpr int EDGE_KELP = -2;
constexpr int EDGE_OPEN = -1;

// What step() returns instead of a tile.
constexpr int STEP_WALL = -1;
constexpr int STEP_BLIND = -2; // through a portal whose far end we have not found

// ---------------------------------------------------------------- input

class LineReader {
  public:
    // Reads the next non-blank line. False at end of input.
    bool next() {
        while (std::fgets(buf_, sizeof buf_, stdin)) {
            size_t n = std::strlen(buf_);
            bool whole = n > 0 && buf_[n - 1] == '\n';
            while (n > 0 && (buf_[n - 1] == '\n' || buf_[n - 1] == '\r')) buf_[--n] = 0;
            if (!whole && n == sizeof buf_ - 1) {
                // An overlong line: keep the start, drop the rest.
                int c;
                while ((c = std::fgetc(stdin)) != EOF && c != '\n') {}
            }
            pos_ = 0;
            skip();
            if (buf_[pos_]) return true;
        }
        return false;
    }

    std::string_view token() {
        skip();
        size_t start = pos_;
        while (buf_[pos_] && buf_[pos_] != ' ' && buf_[pos_] != '\t') pos_++;
        return std::string_view(buf_ + start, pos_ - start);
    }

    long long integer() {
        std::string_view t = token();
        long long v = 0;
        bool neg = false;
        size_t i = 0;
        if (i < t.size() && t[i] == '-') { neg = true; i++; }
        for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; i++) v = v * 10 + (t[i] - '0');
        return neg ? -v : v;
    }

    uint64_t unsigned64() {
        std::string_view t = token();
        uint64_t v = 0;
        for (char c : t) {
            if (c < '0' || c > '9') break;
            v = v * 10 + static_cast<uint64_t>(c - '0');
        }
        return v;
    }

    bool startsWith(const char* word) const { return std::strncmp(buf_ + pos_, word, std::strlen(word)) == 0; }

  private:
    void skip() {
        while (buf_[pos_] == ' ' || buf_[pos_] == '\t') pos_++;
    }
    char buf_[512];
    size_t pos_ = 0;
};

// ---------------------------------------------------------------- the view

// A dragon seen this turn, ours or theirs.
struct Seen {
    int id = -1;
    char team = 'A';
    int head = -1;           // tile of its head, -1 if the head is out of view
    int facing = 0;
    std::vector<int> chain;  // tiles from the head towards the tail, while they can be followed
    int visible = 0;         // segments in view
    bool whole = false;      // its tail is in view too, so length is exact
    int length = 0;          // exact if whole, else a guess
};

class World {
  public:
    // Fixed for the game.
    int W = 0, H = 0, N = 0;
    int myId = 0, unitLimit = 64;
    char myTeam = 'A', enemyTeam = 'B';
    int myQueen = 0, enemyQueen = 1;
    bool isQueen = false;
    int home = -1;           // where this dragon first saw itself

    // This turn.
    int round = 0, facing = 0, length = 0, unitCount = 1, head = 0;
    int turns = 0;           // turns this dragon has played, this one included
    std::vector<uint64_t> messages;
    std::array<int, 5> echoes{};
    std::array<int, VISION * VISION> vision{};  // tiles row by row from the top left
    std::vector<Seen> dragons;

    // Memory, one entry per tile.
    std::vector<int> hEdge, vEdge;  // code of the north and of the west edge of each tile
    std::vector<int> partner;       // edge key -> far edge key, -1 unknown (keys: N + t is the west edge of t)
    std::map<int, std::vector<int>> portalEnds;
    std::vector<int> lastSeen;      // round the tile was last in view, -1 never
    std::vector<uint8_t> pearl;     // pearl there when last seen
    std::vector<int> bedTimer;      // countdown when last seen, -1 not a bed
    std::vector<int> bedSeen;       // round bedTimer was read
    std::vector<int> ghostRound;    // round another dragon was last seen on the tile

    // Occupancy this turn (tiles in view only).
    std::vector<int> occ;           // id of the dragon on the tile, -1 none
    std::vector<int> occStamp;
    std::vector<int> segAt;         // index of the segment in that dragon's chain, -1 unknown
    std::vector<uint8_t> headAt;
    std::vector<int> viewStamp;
    int stamp = 0;

    // Our own body, head first. `missing` segments at the tail end have never been located.
    std::deque<int> body;
    int missing = 0;

    // Sightings that outlive the view.
    int enemyQueenTile = -1, enemyQueenRound = -1000, enemyQueenLength = 0;
    int myQueenTile = -1, myQueenRound = -1000;

    // A step we took through a portal with an unknown far end, to learn from next turn.
    int blindKey = -1, blindDir = 0;

    // ------------------------------------------------------------ geometry

    int X(int t) const { return t % W; }
    int Y(int t) const { return t / W; }
    int tile(int x, int y) const {
        x %= W;
        if (x < 0) x += W;
        y %= H;
        if (y < 0) y += H;
        return y * W + x;
    }
    int neighbour(int t, int d) const { return tile(X(t) + DX[d], Y(t) + DY[d]); }

    int edgeKey(int t, int d) const {
        switch (d) {
        case 0: return t;
        case 2: return tile(X(t), Y(t) + 1);
        case 3: return N + t;
        default: return N + tile(X(t) + 1, Y(t));
        }
    }
    int edgeCode(int key) const { return key < N ? hEdge[key] : vEdge[key - N]; }
    void setEdge(int key, int code) { (key < N ? hEdge[key] : vEdge[key - N]) = code; }

    // The tile entered by crossing edge `key` while heading `d`.
    int beyond(int key, int d) const {
        if (key < N) {
            if (d == 2) return key;
            if (d == 0) return tile(X(key), Y(key) - 1);
        } else {
            int t = key - N;
            if (d == 1) return t;
            if (d == 3) return tile(X(t) - 1, Y(t));
        }
        return STEP_BLIND;
    }

    // Where the head lands stepping `d` from `t`, as the engine works it out.
    int step(int t, int d) const {
        int key = edgeKey(t, d);
        int code = edgeCode(key);
        if (code == EDGE_KELP) return STEP_WALL;
        if (code >= 0) {
            int far = partner[key];
            return far < 0 ? STEP_BLIND : beyond(far, d);
        }
        return neighbour(t, d);
    }

    int torusDist(int a, int b) const {
        int dx = std::abs(X(a) - X(b)), dy = std::abs(Y(a) - Y(b));
        return std::min(dx, W - dx) + std::min(dy, H - dy);
    }
    // Chebyshev distance on the torus: <= 3 means `b` is in the window of a head at `a`.
    int cheb(int a, int b) const {
        int dx = std::abs(X(a) - X(b)), dy = std::abs(Y(a) - Y(b));
        return std::max(std::min(dx, W - dx), std::min(dy, H - dy));
    }
    int mirror(int t) const { return tile(W - 1 - X(t), H - 1 - Y(t)); }

    bool inView(int t) const { return viewStamp[t] == stamp; }
    int occupant(int t) const { return occStamp[t] == stamp ? occ[t] : -1; }
    bool pearlNow(int t) const { return inView(t) && pearl[t]; }

    // Countdown of a pearl bed as of this round, or -1 for a tile that is not one.
    int bedCountdown(int t) const {
        if (bedTimer[t] < 0) return -1;
        return bedTimer[t] - (round - bedSeen[t]);
    }

    Seen* seen(int id) {
        for (Seen& s : dragons)
            if (s.id == id) return &s;
        return nullptr;
    }
    const Seen* seen(int id) const { return const_cast<World*>(this)->seen(id); }

    bool isAlly(int id) const {
        const Seen* s = seen(id);
        return s && s->team == myTeam;
    }

    // ------------------------------------------------------------ protocol

    bool readInit() {
        if (!in_.next()) return false;
        in_.token();
        myId = static_cast<int>(in_.integer());
        in_.next();
        in_.token();
        myTeam = in_.token()[0];
        enemyTeam = myTeam == 'A' ? 'B' : 'A';
        in_.next();
        in_.token();
        W = static_cast<int>(in_.integer());
        H = static_cast<int>(in_.integer());
        in_.next();
        in_.token();
        unitLimit = static_cast<int>(in_.integer());

        N = W * H;
        // Every bundled map lists team A's dragon first, so dragon 0 is A's queen.
        myQueen = myTeam == 'A' ? 0 : 1;
        enemyQueen = 1 - myQueen;
        isQueen = myId == myQueen;

        hEdge.assign(N, EDGE_UNKNOWN);
        vEdge.assign(N, EDGE_UNKNOWN);
        partner.assign(2 * N, -1);
        lastSeen.assign(N, -1);
        pearl.assign(N, 0);
        bedTimer.assign(N, -1);
        bedSeen.assign(N, 0);
        ghostRound.assign(N, -1000);
        occ.assign(N, -1);
        occStamp.assign(N, 0);
        segAt.assign(N, -1);
        headAt.assign(N, 0);
        viewStamp.assign(N, 0);
        predOf_.assign(N, -1);
        predStamp_.assign(N, 0);
        return true;
    }

    bool readTurn() {
        if (!in_.next()) return false;
        if (in_.startsWith("ENDGAME")) return false;
        in_.token();
        round = static_cast<int>(in_.integer());
        stamp++;
        turns++;

        in_.next();
        in_.token();
        facing = dirIndex(in_.token()[0]);
        in_.next();
        in_.token();
        length = static_cast<int>(in_.integer());
        in_.next();
        in_.token();
        unitCount = static_cast<int>(in_.integer());
        in_.next();
        in_.token();
        int count = static_cast<int>(in_.integer());
        messages.clear();
        for (int i = 0; i < count; i++) {
            in_.next();
            messages.push_back(in_.unsigned64());
        }

        in_.next();
        echoes.fill(0);
        if (in_.startsWith("ECHOES")) {
            in_.token();
            for (int& e : echoes) e = static_cast<int>(in_.integer());
            in_.next();
        }

        for (int i = 0; i < VISION * VISION; i++) {
            if (i > 0) in_.next();
            int x = static_cast<int>(in_.integer());
            int y = static_cast<int>(in_.integer());
            int has = static_cast<int>(in_.integer());
            int timer = static_cast<int>(in_.integer());
            int t = tile(x, y);
            vision[i] = t;
            viewStamp[t] = stamp;
            lastSeen[t] = round;
            pearl[t] = has != 0;
            bedTimer[t] = timer;
            bedSeen[t] = round;
        }
        head = vision[VISION * VISION / 2];
        if (home < 0) home = head;

        std::vector<Part> parts;
        in_.next();
        in_.token();
        count = static_cast<int>(in_.integer());
        parts.reserve(count);
        for (int i = 0; i < count; i++) {
            in_.next();
            Part p;
            p.team = in_.token()[0];
            p.id = static_cast<int>(in_.integer());
            int x = static_cast<int>(in_.integer());
            int y = static_cast<int>(in_.integer());
            p.tile = tile(x, y);
            p.facing = dirIndex(in_.token()[0]);
            p.head = in_.integer() == 1;
            parts.push_back(p);
        }

        for (int row = 0; row <= VISION; row++) {
            in_.next();
            for (int col = 0; col < VISION; col++) {
                int code = edgeToken(in_.token());
                int t = row < VISION ? vision[row * VISION + col]
                                     : neighbour(vision[(VISION - 1) * VISION + col], 2);
                learnEdge(t, code);
            }
        }
        for (int row = 0; row < VISION; row++) {
            in_.next();
            for (int col = 0; col <= VISION; col++) {
                int code = edgeToken(in_.token());
                int t = col < VISION ? vision[row * VISION + col]
                                     : neighbour(vision[row * VISION + VISION - 1], 1);
                learnEdge(N + t, code);
            }
        }

        learnBlindPortal();
        buildDragons(parts);
        syncBody();
        return true;
    }

    // ------------------------------------------------------------ own body

    // Replays our own move the way the engine will, so next turn we know the
    // whole body even where it has left the window.
    void applyMove(const std::vector<int>& dirs) {
        int len = length;
        int freeSteps = (len + 3) / 4;
        for (size_t i = 0; i < dirs.size(); i++) {
            int next = step(body.front(), dirs[i]);
            if (next < 0) {  // through a blind portal: rebuilt from the view next turn
                body.clear();
                return;
            }
            body.push_front(next);
            if (pearl[next] && inView(next)) {
                pearl[next] = 0;
                len++;
            } else {
                popTail();
            }
            if (static_cast<int>(i) >= freeSteps) {
                popTail();
                len--;
            }
        }
    }

    void applySplit(int childSize) {
        for (int i = 0; i < childSize; i++) popTail();
    }

  private:
    struct Part {
        int tile, id, facing;
        char team;
        bool head;
    };

    LineReader in_;
    std::vector<int> predOf_, predStamp_;

    static int edgeToken(std::string_view t) {
        if (t.empty() || t[0] == '.') return EDGE_OPEN;
        if (t[0] == 'w') return EDGE_KELP;
        int v = 0;
        for (char c : t) {
            if (c < '0' || c > '9') break;
            v = v * 10 + (c - '0');
        }
        return v;
    }

    void learnEdge(int key, int code) {
        setEdge(key, code);
        if (code < 0 || partner[key] >= 0) return;
        // Both ends of a portal show the same id, so two ends seen make a pair.
        std::vector<int>& ends = portalEnds[code];
        if (std::find(ends.begin(), ends.end(), key) == ends.end()) ends.push_back(key);
        if (ends.size() == 2) {
            partner[ends[0]] = ends[1];
            partner[ends[1]] = ends[0];
        }
    }

    // Last turn we stepped through a portal blind: where we came out names its far end.
    void learnBlindPortal() {
        if (blindKey < 0) return;
        int far = edgeKey(head, opposite(blindDir));
        if (edgeCode(far) >= 0 && partner[blindKey] < 0) {
            partner[blindKey] = far;
            partner[far] = blindKey;
        }
        blindKey = -1;
    }

    void buildDragons(const std::vector<Part>& parts) {
        dragons.clear();
        for (const auto& p : parts) {
            Seen* s = seen(p.id);
            if (!s) {
                dragons.emplace_back();
                s = &dragons.back();
                s->id = p.id;
                s->team = p.team;
            }
            s->visible++;
            if (p.id != myId) ghostRound[p.tile] = round;
            occ[p.tile] = p.id;
            occStamp[p.tile] = stamp;
            segAt[p.tile] = -1;
            headAt[p.tile] = p.head;
            if (p.head) {
                s->head = p.tile;
                s->facing = p.facing;
            }
        }
        // Each body segment faces the segment before it, so following the
        // facings backwards from the head walks the body in order.
        for (const auto& p : parts) {
            if (p.head) continue;
            int to = step(p.tile, p.facing);
            if (to >= 0 && occupant(to) == p.id) {
                predOf_[to] = p.tile;
                predStamp_[to] = stamp;
            }
        }
        for (Seen& s : dragons) {
            if (s.head < 0) {
                s.length = s.visible + 4;
                continue;
            }
            int cur = s.head;
            s.chain.push_back(cur);
            segAt[cur] = 0;
            while (predStamp_[cur] == stamp && static_cast<int>(s.chain.size()) < s.visible) {
                cur = predOf_[cur];
                if (segAt[cur] >= 0) break;
                segAt[cur] = static_cast<int>(s.chain.size());
                s.chain.push_back(cur);
            }
            bool whole = static_cast<int>(s.chain.size()) == s.visible;
            if (whole) {
                // The tail is whatever ends the chain if nothing past it could be hidden.
                for (int d = 0; d < 4 && whole; d++) {
                    int n = step(cur, d);
                    if (n == STEP_WALL) continue;
                    if (n == STEP_BLIND || !inView(n)) whole = false;
                }
            }
            s.whole = whole;
            s.length = whole ? s.visible : std::max<int>(s.visible, static_cast<int>(s.chain.size())) + 4;
            if (s.id == enemyQueen && s.team == enemyTeam) {
                enemyQueenTile = s.head;
                enemyQueenRound = round;
                enemyQueenLength = s.length;
            }
            if (s.id == myQueen && s.team == myTeam) {
                myQueenTile = s.head;
                myQueenRound = round;
            }
        }
    }

    void popTail() {
        if (missing > 0) {
            missing--;
        } else if (!body.empty()) {
            body.pop_back();
        }
    }

    // Keep the replayed body if it still agrees with what we see, else
    // rebuild it from the window.
    void syncBody() {
        bool good = !body.empty() && body.front() == head &&
                    static_cast<int>(body.size()) + missing == length;
        if (good) {
            for (int t : body) {
                if (inView(t) && occupant(t) != myId) {
                    good = false;
                    break;
                }
            }
        }
        if (good) return;
        body.clear();
        Seen* me = seen(myId);
        if (me) {
            for (int t : me->chain) body.push_back(t);
        } else {
            body.push_back(head);
        }
        while (static_cast<int>(body.size()) > length) body.pop_back();
        missing = length - static_cast<int>(body.size());
    }
};

} // namespace bot
