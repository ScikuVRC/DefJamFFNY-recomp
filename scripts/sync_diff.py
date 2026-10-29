#!/usr/bin/env python3
"""Compare the per-step fingerprints of two runs of the same fight.

Each run must have had RECOMP_TEST_SYNC=1 (with the test probes on), which writes
one [TEST-SYNC] line per fight step: a hash of each fighter record, of the pad
table and of the fight's random number generators (src/hooks/test_telemetry.c).

    python scripts/sync_diff.py RUN_A/game.log.err RUN_B/game.log.err [--match N]

Prints the first step at which the runs differ and which parts differ there, or
how many steps agreed. Exit status 0 when they agree over the steps both cover,
1 when they part, 2 when there is nothing to compare.
"""
import argparse
import json
import sys

PARTS = ["fighter 0", "fighter 1", "fighter 2", "fighter 3", "pad table",
         "match rng", "AI rngs", "utility rng", "CLogic rng",
         "play 0", "play 1", "play 2", "play 3", "spectators", "background crowd"]


def fingerprints(path, match=None):
    """{step: [hash per part]} for one match of a log (the first, unless named)."""
    steps = {}
    chosen = match
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[TEST-SYNC] " not in line:
                continue
            try:
                r = json.loads(line.split("[TEST-SYNC] ", 1)[1])
            except ValueError:
                continue
            if chosen is None:
                chosen = r.get("match")
            if r.get("match") != chosen:
                continue
            if r.get("event") != "sync":
                continue
            steps[r["step"]] = r["f"] + [r["pad"]] + r["rng"] + r.get("g", ["", "", "", ""]) + r.get("crowd", [])
    return steps


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("first")
    ap.add_argument("second")
    ap.add_argument("--match", type=int, help="match ordinal in the process (default: the first in each log)")
    a = ap.parse_args(argv)
    one, two = fingerprints(a.first, a.match), fingerprints(a.second, a.match)
    common = sorted(set(one) & set(two))
    if not common:
        print("nothing to compare: %d and %d steps with [TEST-SYNC]" % (len(one), len(two)))
        return 2
    first = {}
    for step in common:
        for i, (x, y) in enumerate(zip(one[step], two[step])):
            if x != y and PARTS[i] not in first:
                first[PARTS[i]] = step
    if first:
        print("parted over %d common steps; first step each part differs:" % len(common))
        for part, step in sorted(first.items(), key=lambda item: item[1]):
            print("  %6d (%6.1f s)  %s" % (step, step / 60.0, part))
        return 1
    print("agree over %d steps (%d to %d; %.1f s); the runs covered %d and %d steps"
          % (len(common), common[0], common[-1], common[-1] / 60.0, len(one), len(two)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
