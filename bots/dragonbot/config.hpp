// Tunable weights. Every value can be overridden at compile time with
// -DNAME=value, which is how tools/arena.py builds A/B variants.
#pragma once

#ifndef PEARL_VALUE
#define PEARL_VALUE 14.0      // worth of one pearl, in steps of travel
#endif
#ifndef EXPLORE_VALUE
#define EXPLORE_VALUE 3.0     // worth of looking at a tile never seen
#endif
#ifndef HUNT_EXPLORE_VALUE
#define HUNT_EXPLORE_VALUE 6.0 // the same, for a hunter in the enemy half
#endif
#ifndef QUEEN_SIGHTING_VALUE
#define QUEEN_SIGHTING_VALUE 40.0 // hunter's pull towards a fresh enemy queen sighting
#endif
#ifndef CONTESTED_FACTOR
#define CONTESTED_FACTOR 0.45 // pearl value kept when an enemy head is closer to it
#endif

// Threat penalties: an enemy head that could move onto our head next turn.
#ifndef QUEEN_THREAT_ADJ
#define QUEEN_THREAT_ADJ 900.0
#endif
#ifndef QUEEN_THREAT_FREE
#define QUEEN_THREAT_FREE 500.0
#endif
#ifndef QUEEN_THREAT_DASH
#define QUEEN_THREAT_DASH 260.0
#endif
#ifndef QUEEN_THREAT_NEAR
#define QUEEN_THREAT_NEAR 25.0  // per enemy head merely in view
#endif
#ifndef WORKER_THREAT_ADJ
#define WORKER_THREAT_ADJ 120.0
#endif
#ifndef WORKER_THREAT_FREE
#define WORKER_THREAT_FREE 35.0
#endif
#ifndef WORKER_THREAT_DASH
#define WORKER_THREAT_DASH 8.0
#endif

// Space: how much room the head has after the move.
#ifndef TRAPPED_PENALTY
#define TRAPPED_PENALTY 2500.0
#endif
#ifndef CRAMPED_WEIGHT
#define CRAMPED_WEIGHT 4.0
#endif
#ifndef OPEN_WEIGHT
#define OPEN_WEIGHT 0.8          // per tile short of 12 within 3 steps of the head
#endif
#ifndef QUEEN_OPEN_WEIGHT
#define QUEEN_OPEN_WEIGHT 2.5
#endif
#ifndef KNOWN_ROOM_WEIGHT
#define KNOWN_ROOM_WEIGHT 25.0   // per tile of room missing if unseen tiles are walls
#endif

#ifndef GHOST_ROUNDS
#define GHOST_ROUNDS 2           // remember other dragons out of view this long
#endif

// Killing blows.
#ifndef KILL_QUEEN_VALUE
#define KILL_QUEEN_VALUE 20000.0
#endif
#ifndef KILL_GUARD_VALUE
#define KILL_GUARD_VALUE 900.0   // removing an enemy head that threatens our queen
#endif
#ifndef KILL_LENGTH_VALUE
#define KILL_LENGTH_VALUE 6.0    // per segment the enemy is longer than us
#endif
#ifndef TRAP_ENEMY_QUEEN
#define TRAP_ENEMY_QUEEN 1500.0  // leaving the enemy queen with no room
#endif
#ifndef TRAP_ENEMY
#define TRAP_ENEMY 60.0
#endif
#ifndef TRAP_ALLY
#define TRAP_ALLY 400.0          // boxing in one of our own dragons
#endif
#ifndef TRAP_MY_QUEEN
#define TRAP_MY_QUEEN 4000.0     // boxing in our own queen
#endif

// Splitting.
#ifndef WORKER_SPLIT_LEN
#define WORKER_SPLIT_LEN 16      // a worker this long splits in half
#endif
#ifndef QUEEN_SPLIT_LEN
#define QUEEN_SPLIT_LEN 14       // a lone queen this long buds off a hunter
#endif
#ifndef QUEEN_SPLIT_SIZE
#define QUEEN_SPLIT_SIZE 5
#endif
#ifndef QUEEN_SPLIT_MAX_UNITS
#define QUEEN_SPLIT_MAX_UNITS 1  // only while the team has at most this many dragons
#endif
#ifndef SPLIT_LAST_ROUND
#define SPLIT_LAST_ROUND 420
#endif

// Search.
#ifndef MAX_SEARCH_STEPS
#define MAX_SEARCH_STEPS 5
#endif
#ifndef SEARCH_NODE_CAP
#define SEARCH_NODE_CAP 6000
#endif

#ifndef BOT_DEBUG
#define BOT_DEBUG 0              // 1: label each dragon with its plan in the replay
#endif
