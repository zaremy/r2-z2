#!/usr/bin/env python3
"""Segment a touch capture into presses, and check them against the panel's
own verdicts in the same log.

COMMITTED BECAUSE THE FIRST VERSION WAS WRONG AND UNCOMMITTED. Its regex was
`\\((\\d+),(\\d+)\\)`, and the firmware pads coordinates to three characters --
so every point with x <= 99 ("(  1,326)") was silently dropped. That is the
LEFT THIRD OF THE PANEL, which is where a rightward swipe begins: right swipes
lost their opening samples, collapsed into what looked like single-sample
flicks, and the write-up then reported "no right swipe ever registered" as a
finding while `panel: swipe right` appeared three times in the same file.

Two rules follow, and they are why this file exists:

  1. Parse with the padding the producer actually emits, and PRINT THE
     COVERAGE -- points parsed against the highest point number the firmware
     logged. A parser that silently drops 39% of its input reads exactly like
     a panel that dropped 39% of its samples.
  2. The panel logs its OWN verdict (`panel: swipe left`, `panel: tap ->`).
     That is ground truth and it was in the log all along. Reconstruct only to
     explain a verdict, never instead of reading it.

Usage: seg_touch.py <capture.txt>
"""
import re
import sys

# `point   57  (  1,326)` -- note the padding inside the parentheses.
PT = re.compile(r"I \((\d+)\) touch: point\s+(\d+)\s+\(\s*(\d+),\s*(\d+)\)")
# The panel's own verdict for a gesture, and the release verdict if present.
VERDICT = re.compile(r"I \((\d+)\) panel: (swipe \w+|tap ->.*|tap on a LOCKED.*)")
PRESS = re.compile(r"I \((\d+)\) touch: press: n=(\d+).*-> (.+?)\s*$")

LIFT_MS = 150          # > 3 polls at 40 ms, and > 10 at the post-fix rate


def main(path):
    pts, verdicts, presses = [], [], []
    lowest, highest = None, 0
    for raw in open(path, errors="replace"):
        ln = re.sub(r"\x1b\[[0-9;]*m", "", raw).rstrip()
        m = PT.search(ln)
        if m:
            t, n, x, y = (int(g) for g in m.groups())
            pts.append((t, x, y))
            highest = max(highest, n)
            lowest = n if lowest is None else min(lowest, n)
            continue
        m = VERDICT.search(ln)
        if m:
            verdicts.append((int(m.group(1)), m.group(2)))
            continue
        m = PRESS.search(ln)
        if m:
            presses.append((int(m.group(1)), int(m.group(2)), m.group(3)))

    # COVERAGE FIRST. Without this the next mis-parse is invisible too.
    # The point counter is cumulative SINCE BOOT, and a capture starts
    # mid-stream, so the expected count runs from the first number seen -- not
    # from 1. Getting this wrong cries wolf on every capture and trains you to
    # ignore the one warning that matters.
    span = (highest - lowest + 1) if lowest is not None else 0
    print(f"parsed {len(pts)} points; firmware numbered {lowest}..{highest} "
          f"({span} expected)")
    if span and len(pts) < span:
        print(f"  *** MISSING {span - len(pts)} POINTS -- do not trust the "
              f"segmentation below until the parser is fixed ***")
    print(f"panel verdicts in log: {len(verdicts)}")
    if presses:
        print(f"firmware press lines:  {len(presses)}  (post-fix build)")
    print()

    if not pts:
        return 1

    groups, cur = [], [pts[0]]
    for p in pts[1:]:
        (groups.append(cur), cur := [p]) if p[0] - cur[-1][0] > LIFT_MS else cur.append(p)
    groups.append(cur)

    used = set()
    print(f"{'#':>3} {'n':>3} {'ms':>5} {'from':>9} {'to':>9} "
          f"{'dx':>5} {'dy':>5}   panel said")
    print("-" * 74)
    for i, g in enumerate(groups, 1):
        t0, x0, y0 = g[0]
        t1, x1, y1 = g[-1]
        # The panel's verdict for this press is the next one it logged.
        hit = next((k for k, (t, _) in enumerate(verdicts)
                    if k not in used and t0 <= t <= t1 + 500), None)
        said = verdicts[hit][1] if hit is not None else "(nothing)"
        if hit is not None:
            used.add(hit)
        print(f"{i:>3} {len(g):>3} {t1-t0:>5} ({x0:>3},{y0:>3}) ({x1:>3},{y1:>3}) "
              f"{x1-x0:>5} {y1-y0:>5}   {said}")

    print()
    for label in ("swipe left", "swipe right", "tap ->", "tap on a LOCKED"):
        n = sum(1 for _, v in verdicts if v.startswith(label))
        print(f"  {label:<12} {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
