"""Bounded input search for authoring replay witnesses, not a reachability proof.

Beam pruning may miss a valid route. A found route is replayed independently
through game_tick before it is accepted. Recording puzzles can supply explicit
setup inputs and search the remaining traversal from that exact state.
"""
from replay import LEFT, RIGHT, DOWN, JUMP, ATTACK, GE_DEATH, GE_HAZARD


def compact(actions):
    """Coalesce adjacent identical held inputs without changing press edges."""
    result = []
    for count, held in actions:
        if result and result[-1][1] == held:
            result[-1][0] += count
        else:
            result.append([count, held])
    return result


def search(sim, goal, score, *, width=96, depth=100, quantum=6, combat=True):
    """Return replayable inputs to a predicate, or fail without claiming impossibility."""
    moves = [0, LEFT, RIGHT, JUMP, LEFT | JUMP, RIGHT | JUMP, DOWN | JUMP]
    if combat:
        moves += [ATTACK, LEFT | ATTACK, RIGHT | ATTACK,
                  LEFT | JUMP | ATTACK, RIGHT | JUMP | ATTACK, DOWN | ATTACK]
    # A pure distance heuristic often cuts a jump early to approach the goal's
    # height, then lacks airtime to reach its x coordinate. Keep full holds as
    # explicit candidates; short candidates still permit variable-height jumps.
    actions = [(quantum, held) for held in moves]
    actions += [(24, held) for held in (JUMP, LEFT | JUMP, RIGHT | JUMP) if quantum < 24]
    start = sim.snapshot()
    initial = sim.state()
    # Nodes carry exact snapshots; coarse keys only select which candidates survive.
    frontier = [(start, [], initial)]
    visited = {}
    best = float("inf")
    for layer in range(depth):
        candidates = {}
        for snapshot, path, _ in frontier:
            for duration, held in actions:
                sim.restore(snapshot)
                invalid = False
                for tick in range(duration):
                    state = sim.tick(held)
                    if state["events"] & (GE_HAZARD | GE_DEATH):
                        invalid = True
                        break
                    if goal(state):
                        result = compact(path + [[tick + 1, held]])
                        sim.restore(start)
                        replayed = sim.actions(result)
                        if not goal(replayed):
                            raise AssertionError("search snapshot diverged from independent input replay")
                        return result
                if invalid:
                    continue
                rank = score(state)
                if rank == float("inf"):
                    continue
                # State partitioning retains ascent/descent, key release and wall-lock alternatives.
                key = (state["room"], state["x"] // 512, state["y"] // 512,
                       state["vx"] // 128, state["vy"] // 128, state["ground"],
                       state["wall"], held, state["health"], bool(state["hold"]),
                       bool(state["lock"]), state["wire"], state["boss_hp"], sim.frames % 48)
                priority = rank + layer * 0.025
                if visited.get(key, float("inf")) <= priority:
                    continue
                visited[key] = priority
                old = candidates.get(key)
                if old is None or priority < old[0]:
                    candidates[key] = (priority, sim.snapshot(), path + [[duration, held]], state)
        if not candidates:
            break
        chosen = sorted(candidates.values(), key=lambda item: item[0])[:width]
        frontier = [(snapshot, path, state) for _, snapshot, path, state in chosen]
        if chosen[0][0] < best - 16:
            best = chosen[0][0]
            s = chosen[0][3]
            print(f"search {layer}: score {best:.1f}, room {s['room']}, "
                  f"xy {s['x']/256:.1f},{s['y']/256:.1f}, health {s['health']}", flush=True)
    sim.restore(start)
    raise RuntimeError(f"bounded search found no route ({width} states x {depth} layers)")


def waypoint(sim, room, rect, **options):
    """Search for a rectangle in one room, penalizing health loss along the route."""
    target = sim.names[room]
    start = sim.state()
    original = start["room"]
    x0, y0, x1, y1 = rect

    def goal(s):
        """The hero top-left must reach the authored checkpoint rectangle."""
        return s["room"] == target and x0 * 256 <= s["x"] <= x1 * 256 and y0 * 256 <= s["y"] <= y1 * 256

    def score(s):
        """Compare world positions so the heuristic also guides a neighboring-room exit."""
        if s["room"] not in (original, target):
            return float("inf")
        _, r = sim.rooms[s["room"]]
        _, t = sim.rooms[target]
        x = s["x"] / 256 + (r[1] - t[1]) * 8
        y = s["y"] / 256 + (r[2] - t[2]) * 8
        dx = max(x0 - x, 0, x - x1)
        dy = max(y0 - y, 0, y - y1)
        return dx + dy * 1.4 + max(0, start["health"] - s["health"]) * 200

    return search(sim, goal, score, **options)
