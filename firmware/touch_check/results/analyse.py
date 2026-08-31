#!/usr/bin/env python3
"""Re-derive every published A3 figure from the raw capture.

Exists so the numbers in docs/research/board-capabilities.md are auditable from
the branch. The firmware only tracks a running count and min/max; distinct-value
counts and monotonic runs are post-processing, and without this script those
figures would be unreproducible assertions.

    python3 analyse.py a3-touch-2026-08-31.txt

STROKE SEGMENTATION, and why it is not optional. The firmware prints a sample
only when x or y CHANGED, and the original capture predates the `LIFT` marker,
so consecutive lines in it can belong to two different touches. An unconstrained
longest-monotonic-run over that file reports 22 -- but that run contains a 197 px
jump and spans a lift. It is not a drag. This script therefore splits strokes on
a `LIFT` line when present, and otherwise on a coordinate discontinuity, and only
reports runs WITHIN a stroke. Reported figures are stable at a 25, 40 and 50 px
threshold, so they do not depend on where that line is drawn.
"""
import re
import sys
from collections import Counter

PANEL_W, PANEL_H = 368, 448
V2_PANEL_X_GAP = 0x10
MAX_STEP_PX = 25          # conservative; see stability check in main()


def load(path):
    """Return a list of strokes, each a list of (x, y, gesture)."""
    strokes, cur = [], []
    for line in open(path, errors="replace"):
        if "LIFT" in line:
            if cur:
                strokes.append(cur)
                cur = []
            continue
        m = re.search(r"TOUCH x=\s*(\d+) y=\s*(\d+)\s+gesture=(\S+)", line)
        if m:
            cur.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    if cur:
        strokes.append(cur)
    return strokes


def split_on_jumps(strokes, max_step):
    """Split further wherever the finger could not physically have travelled."""
    out = []
    for st in strokes:
        cur = [st[0]] if st else []
        for prev, p in zip(st, st[1:]):
            if max(abs(p[0] - prev[0]), abs(p[1] - prev[1])) > max_step:
                out.append(cur)
                cur = [p]
            else:
                cur.append(p)
        if cur:
            out.append(cur)
    return out


def longest_monotonic(strokes, idx):
    """Longest strictly-monotonic run in one axis, never crossing a stroke."""
    best = 1
    for st in strokes:
        for sign in (1, -1):
            cur = 1
            for prev, p in zip(st, st[1:]):
                d = p[idx] - prev[idx]
                cur = cur + 1 if d != 0 and (d > 0) == (sign > 0) else 1
                best = max(best, cur)
    return best


def main(path):
    raw = load(path)
    pts = [p for st in raw for p in st]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    g = Counter(p[2] for p in pts)
    below = sum(1 for v in xs if v < V2_PANEL_X_GAP)
    strokes = split_on_jumps(raw, MAX_STEP_PX)

    if not pts:
        # The likeliest capture to hand this script is a FAILED one -- nobody
        # touched the panel, or the listener attached to a board sitting in the
        # ROM downloader. Say so plainly instead of dying in min().
        print(f"no touch samples in {path!r}.")
        print("That is a capture with no data, NOT a measurement that touch is")
        print("absent. Check the app was running (heartbeat lines) before reading")
        print("anything into it.")
        return

    print(f"points logged        : {len(pts)}")
    print(f"strokes (segmented)  : {len(strokes)}  (from {len(raw)} LIFT-delimited)")
    print(f"distinct x / y       : {len(set(xs))} / {len(set(ys))}")
    print(f"x range              : {min(xs)}..{max(xs)}   (panel 0..{PANEL_W - 1})")
    print(f"y range              : {min(ys)}..{max(ys)}   (panel 0..{PANEL_H - 1})")
    print(f"samples with x < {V2_PANEL_X_GAP:<3}: {below}"
          f"   <- nonzero REFUTES the display's 16 px offset applying to touch")
    print(f"longest in-stroke monotonic run: "
          f"x {longest_monotonic(strokes, 0)}  y {longest_monotonic(strokes, 1)}")
    print(f"gesture register     : {dict(g)}")

    print("\nthreshold stability (the run figures must not depend on MAX_STEP_PX):")
    for step in (25, 40, 50):
        s2 = split_on_jumps(raw, step)
        print(f"  max step {step:>3} px -> x {longest_monotonic(s2, 0)}"
              f"  y {longest_monotonic(s2, 1)}")

    print("\nCAVEAT: the firmware logs only when x or y changed, so these counts are")
    print("per-logged-point, NOT per-poll. No gesture RATE can be derived, and a")
    print("tap -- which holds coordinates still -- is structurally invisible here.")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "a3-touch-2026-08-31.txt")
