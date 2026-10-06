#!/usr/bin/env python3
"""Play many local matches between two bots and summarise who wins.

Each map is played twice per seed, once from each side, so neither bot keeps
the advantage of moving first. Matches run in parallel on the toolkit's own
engine, so results are what `unswbc run` would give.

    python tools/arena.py bots/dragonbot baselines/greedy
    python tools/arena.py bots/dragonbot baselines/starter --maps arena default --seeds 4
    python tools/arena.py A B --maps default --seed 7 --record rec   # one game, every dragon's turns

A bot is a project directory (built with its bot.toml), an executable, or a
main.py. Needs the toolkit: `uv tool install unswbc` or `pip install unswbc`.
"""

from __future__ import annotations

import argparse
import collections
import multiprocessing
import pathlib
import random
import shutil
import sys
import tempfile
import time

try:
    import unswbc
    from unswbc.bot import Bot, Pool
    from unswbc.engine import EngineModule
    from unswbc.run import _resolve
except ImportError:
    sys.exit("arena.py needs the unswbc toolkit: pip install unswbc")

ROOT = pathlib.Path(__file__).resolve().parent.parent
REASONS = {"W": "wall", "S": "self", "O": "body", "H": "head-on", "A": "no action"}


def find_maps(names: list[str] | None) -> list[pathlib.Path]:
    folders = [ROOT / "maps", pathlib.Path(unswbc.__file__).parent / "templates" / "maps"]
    found: dict[str, pathlib.Path] = {}
    for folder in folders:
        if folder.is_dir():
            for path in sorted(folder.glob("*.map")):
                found.setdefault(path.stem, path)
    if not names:
        return list(found.values())
    out = []
    for name in names:
        path = pathlib.Path(name)
        if path.is_file():
            out.append(path)
        elif name in found:
            out.append(found[name])
        else:
            sys.exit(f"no map called {name}")
    return out


def play(job: tuple) -> dict:
    map_path, bots, seed, record, swapped = job
    (argv_a, dir_a), (argv_b, dir_b) = bots
    pools = {"A": Pool(argv_a, cwd=str(dir_a), size=1), "B": Pool(argv_b, cwd=str(dir_b), size=1)}
    live: dict[int, Bot] = {}
    teams: dict[int, str] = {}
    deaths = collections.Counter()
    errors: list[str] = []
    transcript: dict[int, list[str]] = collections.defaultdict(list)
    died: list[str] = []

    def spawn(dragon_id: int, init: bytes) -> None:
        team = next((l.split()[1] for l in init.decode().splitlines() if l.startswith("TEAM")), "A")
        teams[dragon_id] = team
        live[dragon_id] = Bot(pools[team], init=init, name=str(dragon_id))
        if record:
            transcript[dragon_id].append(init.decode())

    def reply(dragon_id: int, block: bytes) -> bytes:
        bot = live[dragon_id]
        out = bot.ask(block)
        if bot.error is not None and len(errors) < 5:
            errors.append(f"dragon {dragon_id} ({teams.get(dragon_id)}): {bot.error}")
        if record:
            transcript[dragon_id].append(block.decode())
            transcript[dragon_id].append("#REPLY\n" + out.decode(errors="replace") + "#END\n")
        return out

    def death(dragon_id: int, round_num: int, reason: str) -> None:
        deaths[(teams.get(dragon_id, "?"), REASONS.get(reason, reason))] += 1
        if record:
            transcript[dragon_id].append(f"#DIED round {round_num}: {REASONS.get(reason, reason)}\n")
            died.append(f"round {round_num}: dragon {dragon_id} ({teams.get(dragon_id)}) {REASONS.get(reason, reason)}")
        bot = live.pop(dragon_id, None)
        if bot is not None:
            bot.stop()

    try:
        result = EngineModule().run(map_path.read_bytes(), reply, death, spawn, lambda line: None, 0, seed)
    finally:
        for bot in live.values():
            bot.stop()
        for pool in pools.values():
            pool.close()
    return {"map": map_path.stem, "seed": seed, "swapped": swapped, "result": result, "deaths": deaths,
            "errors": errors, "died": died,
            "transcripts": {k: "".join(v) for k, v in transcript.items()}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bot_a")
    parser.add_argument("bot_b")
    parser.add_argument("--maps", nargs="*", help="map names or paths (default: every map)")
    parser.add_argument("--seeds", type=int, default=2, help="seeds per map; each is played from both sides")
    parser.add_argument("--seed", type=lambda s: int(s, 0), help="base seed (default: random)")
    parser.add_argument("--jobs", type=int, default=max(1, multiprocessing.cpu_count() - 1))
    parser.add_argument("--record", metavar="DIR", help="play one game and write every dragon's input and replies into DIR")
    parser.add_argument("--quiet", action="store_true", help="only print the summary")
    args = parser.parse_args()

    # Snapshot compiled bots, so rebuilding one mid-run cannot change the match.
    snapshots = pathlib.Path(tempfile.mkdtemp(prefix="arena-"))
    bots = []
    for index, name in enumerate((args.bot_a, args.bot_b)):
        argv, folder, kind = _resolve(name)
        if kind == "c/c++":
            copy = snapshots / f"bot{index}"
            shutil.copy2(argv[0], copy)
            argv = [str(copy)] + argv[1:]
        bots.append((argv, folder))

    base = args.seed if args.seed is not None else random.getrandbits(32)
    jobs = []
    for map_path in find_maps(args.maps):
        for i in range(args.seeds):
            seed = (base * 1_000_003 + i * 7919 + hash(map_path.stem) % 10007) & ((1 << 63) - 1)
            jobs.append((map_path, (bots[0], bots[1]), seed, args.record, False))
            jobs.append((map_path, (bots[1], bots[0]), seed, args.record, True))
    if args.record is not None:
        jobs = jobs[:1]

    started = time.monotonic()
    per_map = collections.defaultdict(lambda: [0, 0, 0])
    total = [0, 0, 0]
    queens = [[], []]
    deaths = collections.Counter()
    with multiprocessing.Pool(args.jobs) as workers:
        for outcome in workers.imap_unordered(play, jobs):
            swapped = outcome["swapped"]
            r = outcome["result"]
            first = r.winner == ("B" if swapped else "A")
            second = r.winner == ("A" if swapped else "B")
            slot = 0 if first else 1 if second else 2
            per_map[outcome["map"]][slot] += 1
            total[slot] += 1
            qa, qb = (r.b_queen, r.a_queen) if swapped else (r.a_queen, r.b_queen)
            queens[0].append(qa)
            queens[1].append(qb)
            for (team, reason), n in outcome["deaths"].items():
                ours = (team == "A") != swapped
                deaths[("A-bot" if ours else "B-bot", reason)] += n
            if not args.quiet:
                verdict = ["bot A wins", "bot B wins", "draw"][slot]
                print(f"{outcome['map']:<22} seed {outcome['seed'] % 100000:>5} {'(swapped)' if swapped else '         '} "
                      f"{verdict:<10} rounds {r.rounds + 1:>3}  queens {qa:>3} v {qb:<3} "
                      f"dragons {r.a_dragons if not swapped else r.b_dragons:>2} v "
                      f"{r.b_dragons if not swapped else r.a_dragons:<2}  died "
                      f"{sum(n for (t, _), n in outcome['deaths'].items() if (t == 'A') != swapped):>3} v "
                      f"{sum(n for (t, _), n in outcome['deaths'].items() if (t == 'A') == swapped):<3}", flush=True)
            for e in outcome["errors"]:
                print("   !", e)
            if args.record:
                folder = pathlib.Path(args.record)
                folder.mkdir(parents=True, exist_ok=True)
                for dragon_id, text in outcome["transcripts"].items():
                    (folder / f"dragon{dragon_id}.txt").write_text(text)
                print("\n".join(outcome["died"]))
                print(f"wrote {len(outcome['transcripts'])} transcripts into {folder}")
    shutil.rmtree(snapshots, ignore_errors=True)

    print()
    print(f"{'map':<22} {'A wins':>7} {'B wins':>7} {'draws':>6}")
    for name, (a, b, d) in sorted(per_map.items()):
        print(f"{name:<22} {a:>7} {b:>7} {d:>6}")
    games = sum(total)
    print(f"{'TOTAL':<22} {total[0]:>7} {total[1]:>7} {total[2]:>6}   "
          f"A score {(total[0] + 0.5 * total[2]) / max(1, games):.3f} over {games} games "
          f"in {time.monotonic() - started:.0f}s")
    if queens[0]:
        print(f"mean queen length at the end: A {sum(queens[0]) / len(queens[0]):.1f}  "
              f"B {sum(queens[1]) / len(queens[1]):.1f}")
    print("deaths:", ", ".join(f"{who} {why} {n}" for (who, why), n in sorted(deaths.items())))


if __name__ == "__main__":
    main()
