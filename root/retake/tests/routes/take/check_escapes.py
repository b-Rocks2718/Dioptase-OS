"""Bounded searches that the take set pieces cannot be skipped (evidence,
not proof: beam pruning may miss a route). Run from root/retake after
`make -C tests build/sim.so`; exits nonzero if any search finds an escape."""
import sys
sys.path.insert(0, "tools")
from replay import Sim
from solve import waypoint

S3 = 1 | 2   # jump cut, wire


def s3_plank():
    """From the highest clap height over the water, no way back without a cut."""
    escapes = 0
    sim = Sim()
    for x in (520, 556, 590):
        for room, rect in (("s3_plank", [560, 90, 640, 132]), ("s3_plank", [440, 90, 512, 132]),
                           ("s3_wreck", [0, 90, 80, 132])):
            sim.start("s3_plank", x, 150, S3)
            s = sim.state()
            while s["y"] < 182 * 256:
                s = sim.tick(0)
            try:
                waypoint(sim, room, rect, width=192, depth=50, quantum=4)
                print(f"ESCAPE s3_plank: from x {x} to {room} {rect}")
                escapes += 1
            except RuntimeError:
                print(f"no route s3_plank: from x {x} to {room} {rect}")
    return escapes


if __name__ == "__main__":
    sys.exit(1 if s3_plank() else 0)
