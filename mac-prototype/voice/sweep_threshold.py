"""AC3 threshold sweep — does a usable sensitivity range exist? (#42)

Runs against a recorded score trace, so it needs no room time and no hardware.
AC3 requires this before treating a false-accept rate as a runtime tunable.

    python3 voice/sweep_threshold.py <trace.jsonl> [...]

Contiguous stable interval around the operating point,
and every transition, computed honestly. Detection count is NOT monotonic in
threshold -- a lower threshold merges adjacent bursts via the refractory
window -- so min/max over the set of thresholds giving N is meaningless."""
import json, sys

if len(sys.argv) < 2:
    sys.exit(__doc__)
TRACES = [(p, p, None) for p in sys.argv[1:]]

def frames(p): return [json.loads(l) for l in open(p)][1:]

def dets(fr, thr, refr=1.5):
    n, last = 0, -1e9
    for f in fr:
        if f["score"] >= thr and f["t"] - last > refr: n += 1; last = f["t"]
        elif f["score"] >= thr: last = f["t"]
    return n

GRID = [i/1000 for i in range(1, 1000)]          # 0.001 .. 0.999
for label, path, said in TRACES:
    fr = frames(path)
    vals = [(t, dets(fr, t)) for t in GRID]
    base = dets(fr, 0.5)
    # largest CONTIGUOUS interval containing 0.5 with the same count
    i = next(k for k, (t, _) in enumerate(vals) if t >= 0.5)
    lo = hi = i
    while lo > 0 and vals[lo-1][1] == base: lo -= 1
    while hi < len(vals)-1 and vals[hi+1][1] == base: hi += 1
    trans = [(vals[k][0], vals[k-1][1], vals[k][1])
             for k in range(1, len(vals)) if vals[k][1] != vals[k-1][1]]
    print(f"{label}")
    print(f"  count at 0.5 = {base}")
    print(f"  stable contiguous interval containing 0.5: "
          f"[{vals[lo][0]:.3f}, {vals[hi][0]:.3f}]  = {vals[hi][0]-vals[lo][0]:.3f} wide "
          f"({100*(hi-lo+1)/len(vals):.0f}% of the 0-1 range)")
    if trans:
        print(f"  transitions across the grid: " +
              ", ".join(f"{t:.3f}:{a}->{b}" for t, a, b in trans))
    else:
        print("  transitions: none anywhere in 0.001-0.999")
    print()

print("=" * 68)
print("WHAT THE TRANSITIONS ACTUALLY ARE")
print("=" * 68)
for label, path, _ in TRACES:
    fr = frames(path)
    mid = [f for f in fr if 0.005 <= f["score"] < 0.95]
    if mid:
        print(f"{label}: {len(mid)} frame(s) in the middle of the range")
        for f in mid:
            print(f"   t+{f['t']-fr[0]['t']:6.2f}s  score={f['score']:.4f}")
    else:
        print(f"{label}: no frames between 0.005 and 0.95 — fully bimodal")
