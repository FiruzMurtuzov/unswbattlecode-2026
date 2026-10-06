# UNSW Battlecode 2026 — "Dragons"

## What the competition is
[UNSW Battlecode](https://game.battlecode.au) is a ~4-week online AI programming contest run by UNSW CPMSoc, open to Asia-Pacific students (teams of up to 4, Python / C / C++, $20k+ prizes). You submit a bot; it plays ranked matches against other teams on a ladder, then Sprint / Qualifier / Grand Final tournaments.

## The game (from the organisers' engine, github.com/unswcpmsoc/unswbc)
- Turn-based Snake-like game on a wrapping grid (up to 64×64) with **kelp** walls and **portals** on tile edges.
- Each team controls **dragons**. Every dragon is a **separate process** that only sees a **7×7 window** around its head; teammates can only talk through *sonar* rays.
- Each turn a dragon must `MOVE` (a sequence of steps: first ⌈L/4⌉ free, each extra costs a segment), `SPLIT n` (tail n segments become a new dragon), or it dies.
- Eating a **pearl** grows you by 1. Pearls spawn on beds with visible countdowns; a dead dragon drops a pearl on every other segment.
- Hitting kelp, yourself, or another body kills you. **Head-on collisions kill both.**
- Game ends when a team is eliminated, or after 500 rounds: winner has the longer **queen** (dragon 0 for A, 1 for B), then longest dragon, then total length.
- Judge limits: 100M CPU "points", 48 MB per dragon per turn — C++ gets ~60× more compute than Python.

## This repo
| path | what |
|---|---|
| `bots/dragonbot/` | the C++ submission (`world.hpp` parsing/memory/portals, `brain.hpp` decisions, `config.hpp` weights) |
| `baselines/starter`, `baselines/greedy` | opponents for testing |
| `tools/arena.py` | runs many local matches in parallel, both sides, and summarises |

### Strategy of dragonbot
- Remembers every tile/edge/pearl bed it has seen, learns portal pairs, tracks its own body exactly.
- Simulates every legal multi-step move and scores it: pearls eaten + a value field (pearls, beds about to spawn, unexplored ground), time-aware flood-fill room (pessimistic for unseen ground), openness, and threat from enemy heads that could ram it next turn (queen weighs this heavily).
- Workers **ram the enemy queen** head-on whenever a path is reachable (kills her → wins the tiebreak), guard our queen, and body-block enemies.
- Splits long workers into hunters; when trapped, splits so the tail survives.

Local result vs the greedy baseline on small maps: ~82% score (28W 5L 3D over 36 games). Known weak spot: Colosseum (pearl-dense, dragons grow huge and crash).

## Usage
```sh
uv tool install unswbc             # or: pip install unswbc
unswbc maps                        # fetch contest maps
unswbc run maps/arena.map bots/dragonbot baselines/greedy
python tools/arena.py bots/dragonbot baselines/greedy --maps arena devil --seeds 2
unswbc run maps/default.map bots/dragonbot bots/dragonbot --sandbox   # judge's CPU meter
unswbc auth set bc_... && unswbc submit bots/dragonbot
```
Tune by editing `config.hpp` (or `-D` overrides) and comparing with `tools/arena.py`.
