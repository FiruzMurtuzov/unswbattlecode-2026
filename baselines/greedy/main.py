"""A plain greedy bot to test against: walk to the nearest pearl in view,
keep clear of other heads, ram the enemy queen when it is next to us, and
split once long enough. Roughly what a first serious submission looks like."""

from collections import deque

import helper as unswbc
from helper import Direction, EdgeType

ct: unswbc.Controller
game: unswbc.Game


def neighbours(pos):
    tile = ct.get_tile(pos)
    if tile is None:
        return
    for d in Direction.get_direction_list():
        edge = tile.get_edge(d)
        if edge.get_edge_type() != EdgeType.EMPTY:
            continue  # kelp, or a portal we do not want to reason about
        nxt = pos.add_dir(d)
        if ct.get_tile(nxt) is not None:
            yield d, nxt


def free(pos) -> bool:
    tile = ct.get_tile(pos)
    return tile is not None and tile.get_dragon() is None


def room(start) -> int:
    seen = {start}
    todo = deque([start])
    while todo:
        p = todo.popleft()
        for _, n in neighbours(p):
            if n not in seen and free(n):
                seen.add(n)
                todo.append(n)
    return len(seen)


def execute_turn() -> None:
    me = ct.get_team()
    head = ct.get_position()
    enemy_queen = 1 if me == unswbc.Team.A else 0

    danger = set()
    for tile in ct.get_tiles():
        part = tile.get_dragon()
        if part is None or not part.is_head() or part.get_id() == ct.get_id():
            continue
        if part.get_team() != me and part.get_id() == enemy_queen:
            for d, n in neighbours(head):
                if n == tile.get_position():
                    ct.make_move(d)
                    return
        for _, n in neighbours(tile.get_position()):
            danger.add(n)

    if ct.get_length() >= 8 and ct.can_split(4):
        ct.do_split(4)
        return

    first = {}
    todo = deque()
    for d, n in neighbours(head):
        if free(n) and n not in danger:
            first[n] = d
            todo.append(n)
    while todo:
        p = todo.popleft()
        if ct.get_tile(p).has_pearl() and room(p) >= 3:
            ct.make_move(first[p])
            return
        for _, n in neighbours(p):
            if n not in first and free(n):
                first[n] = first[p]
                todo.append(n)

    best, best_room = None, -1
    for d, n in neighbours(head):
        if free(n):
            r = room(n) - (10 if n in danger else 0)
            if r > best_room:
                best, best_room = d, r
    ct.make_move(best or ct.get_dir())


def main() -> None:
    global ct, game
    ct, game = unswbc.init()
    while unswbc.update(ct, game):
        execute_turn()
        unswbc.end_turn()


if __name__ == "__main__":
    main()
