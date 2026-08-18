#!/usr/bin/env python3
"""sensor_probe.py — S1e: can R2 feel being touched? (issue #29)

Answers one question with two independent instruments, because neither is
trustworthy alone:

  AC5  the dome as a crude force sensor — needs NO new daemon code, so it runs
       first and may answer the whole question by itself.
  AC1-4 the sensor stream (DID 0x18) — defined in our constants since the
       start and never once switched on.

Run one phase per invocation. The operator is at the robot and we are at the
queue, so "go" arrives in chat; a driver that blocks on stdin waits in a window
nobody is watching.

    ./r2 daemon --allow read      # AC1-AC4 (read-only)
    ./r2 daemon --allow dome      # AC5 as well

    python3 sensor_probe.py selftest        # no hardware, no daemon
    python3 sensor_probe.py ac1-enable
    python3 sensor_probe.py ac2-characterise
    python3 sensor_probe.py ac3-baseline --seconds 60
    python3 sensor_probe.py ac4-touch --site dome --trials 5
    python3 sensor_probe.py ac5-resistance
    python3 sensor_probe.py verdict

> AC5's design changed on 2026-08-17, AFTER #29 was written.
> The issue says "keep moves small — use a much smaller delta" than the 45°
> cap. That instruction is now unsafe in the other direction: commanded dome
> travel below ~10.5° is SILENTLY IGNORED and still reports ok (D-013). A 5°
> resistance probe would move nothing, detect nothing, and read as "total
> resistance". The probe therefore uses 15°, the smallest travel that reliably
> moves, and treats a zero-travel result as UNRESOLVED rather than as a
> detection.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_probe as P
from r2_behavior import MIN_DOME_TRAVEL_DEG, FileBridge, Step

STATE = Path(__file__).parent / ".bridge" / "s1e-sensor-results.json"

# A resistance probe must clear the dome's silent-ignore floor (D-013).
RESIST_TRAVEL_DEG = 15.0
# #10 measured unobstructed undershoot at 2.4° (+170°) to 5.7° (−145°). An
# unresisted 15° command therefore lands ~9–13° away. Resistance has to beat
# that spread to mean anything, so require the move to fall SHORT by more than
# the worst-case free undershoot plus margin.
FREE_UNDERSHOOT_WORST_DEG = 5.7
RESIST_MARGIN_DEG = 4.0
TRIALS_TO_CONFIRM = 4          # of 5
# A trial must disturb this many channels at once to count.
CHANNELS_TO_CORROBORATE = 2


# ---------------------------------------------------------------------------
# state
# ---------------------------------------------------------------------------

def load() -> dict:
    if STATE.exists():
        return json.loads(STATE.read_text())
    return {}


def save(state: dict) -> None:
    """Persist BEFORE printing. A drain that formats results and then discards
    the raw payload has thrown away the only copy of evidence that cost the
    operator's attention to collect."""
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(state, indent=2))


# ---------------------------------------------------------------------------
# capture
# ---------------------------------------------------------------------------

GROUPS = ["accelerometer", "attitude"]
EXT_GROUPS = ["r2_head_angle", "gyroscope"]


def masks() -> tuple[int, int]:
    return P.sensor_mask(GROUPS), P.ext_sensor_mask(EXT_GROUPS)


def collect(bridge, seconds: float, mask: int, ext: int) -> list[dict]:
    """Drain sensor_stream events for `seconds`, keeping raw hex beside every
    decode so a published number can be re-derived without re-running
    hardware."""
    # FLUSH FIRST. The ring keeps accumulating between invocations, so the
    # first drain after any gap sweeps in minutes of history -- a 5 s window
    # came back with 220 samples at a 7.3 Hz stream, carrying disturbances
    # from earlier phases, and a no-touch control "fired" on them. A window
    # must measure what happened DURING the window.
    bridge.send_batch([Step("events", {})], timeout=15)
    samples: list[dict] = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        r = bridge.send_batch([Step("events", {})], timeout=15)[0]
        for ev in (r.get("data") or {}).get("events", []):
            if ev.get("name") != "sensor_stream":
                continue
            raw = bytes.fromhex(ev["data"])
            try:
                decoded = P.decode_sensor_stream(raw, mask, ext)
                err = None
            except ValueError as e:
                decoded, err = None, str(e)
            samples.append({"t": ev["t"], "raw": ev["data"],
                            "decoded": decoded, "decode_error": err})
        time.sleep(0.25)
    return samples


def channels(samples: list[dict]) -> dict[str, list[float]]:
    """Flatten decoded samples into {"group.component": [values]}."""
    out: dict[str, list[float]] = {}
    for s in samples:
        for group, comps in (s.get("decoded") or {}).items():
            for comp, value in comps.items():
                out.setdefault(f"{group}.{comp}", []).append(value)
    return out


# ---------------------------------------------------------------------------
# rubric — dry-testable, no hardware
# ---------------------------------------------------------------------------

def window_stat(vals: list[float], mu: float = 0.0) -> float:
    """Peak-to-peak WITHIN the window. `mu` is ignored, kept for call symmetry.

    Deliberately offset-invariant, and that is the whole point. The previous
    statistic was `max|v - baseline_mean|`, which fires whenever a channel
    simply SITS somewhere new: head angle rests at a stable value, and a small
    shift between the baseline and a trial puts every sample past the limit
    with nothing touching him. A no-touch control fired on
    r2_head_angle for exactly that reason.

    Touch is a disturbance, not a displacement. Peak-to-peak measures the
    disturbance and ignores where the channel happens to be parked."""
    if not vals:
        return 0.0
    return max(vals) - min(vals)


def empirical_thresholds(baseline: dict[str, list[float]], window_n: int,
                         margin: float = 1.5) -> dict[str, tuple[float, float]]:
    """Per channel: (rest mean, the excursion REST ITSELF produces).

    REPLACES a 6-sigma rule that was measurably broken. That rule compared the
    MAXIMUM deviation across a whole trial against a sigma multiple derived
    from the baseline -- and the max of 85-220 draws clears 6 sigma by chance
    alone, so every trial fired regardless of touch. A no-touch control fired
    on gyroscope.x and exposed it, after 10/10 trials had "passed".

    The honest comparison is like-for-like: chop the rest baseline into windows
    the same length as a trial, take the biggest excursion rest produces in any
    of them, and require a trial to beat that by `margin`. The null
    distribution then comes from the data instead of from an assumption about
    its shape.
    """
    out = {}
    for name, vals in baseline.items():
        if len(vals) < 2 * window_n:
            continue
        mu = statistics.fmean(vals)
        windows = [vals[i:i + window_n]
                   for i in range(0, len(vals) - window_n + 1, max(1, window_n // 2))]
        rest_p2p = max(window_stat(w) for w in windows)
        out[name] = (mu, rest_p2p * margin)
    return out


def trial_fires(trial: dict[str, list[float]],
                thresholds: dict[str, tuple[float, float]]) -> tuple[bool, str | None]:
    """Did this trial exceed what REST produces over an equal-length window?

    Returns the winning channel so a finding names its discriminating signal
    rather than asserting one. `thresholds` comes from empirical_thresholds,
    NOT from a sigma rule -- see the note there for why that distinction cost
    ten trials."""
    over = []
    for name, vals in trial.items():
        if name not in thresholds or not vals:
            continue
        _, limit = thresholds[name]
        if limit <= 0:
            continue
        ratio = window_stat(vals) / limit
        if ratio >= 1.0:
            over.append((ratio, name))
    over.sort(reverse=True)
    # CORROBORATION. A hand disturbs several channels at once -- gyro,
    # attitude and accelerometer all move together. A single channel crossing
    # its limit alone is far more likely to be a spurious encoder blip, and
    # measurably was: r2_head_angle produced a lone false positive in 1 of 4
    # no-touch controls under a single-channel rule.
    fired = len(over) >= CHANNELS_TO_CORROBORATE
    return fired, (over[0][1] if over else None)


def resistance_verdict(start: float, commanded: float, landed: float) -> dict:
    """AC5. Three outcomes, not two.

    `unresolved` exists because the dome ignores sub-threshold commands
    outright: zero travel can mean "a hand stopped it" OR "the command was
    below the floor and never issued". Those are opposite findings and they
    look identical, so a probe that only reports detects/does-not would report
    the dangerous one confidently."""
    want = commanded - start
    got = landed - start
    if abs(want) < MIN_DOME_TRAVEL_DEG:
        return {"verdict": "unresolved",
                "why": f"commanded travel {want:+.1f}° is at or below the "
                       f"silent-ignore floor; a null result proves nothing"}
    shortfall = abs(want) - abs(got)
    if abs(got) < 1.0:
        return {"verdict": "unresolved", "shortfall_deg": round(shortfall, 2),
                "why": "dome did not move at all — indistinguishable from a "
                       "dropped command; re-run unobstructed to confirm the "
                       "instrument before reading this as resistance"}
    detected = shortfall > (FREE_UNDERSHOOT_WORST_DEG + RESIST_MARGIN_DEG)
    return {"verdict": "detects_resistance" if detected else "does_not",
            "shortfall_deg": round(shortfall, 2),
            "threshold_deg": FREE_UNDERSHOOT_WORST_DEG + RESIST_MARGIN_DEG,
            "why": f"moved {got:+.1f}° of a commanded {want:+.1f}°"}


def overall_verdict(state: dict) -> dict:
    """AC6 — {touch_detectable, motion_detectable_only, neither} plus
    `instrument_limited`, the case where the measurement could not have
    detected the effect. Without that fourth outcome a stream that never
    turned on reports a confident, wrong 'neither'."""
    stream_live = bool(state.get("ac1", {}).get("accepted")) and \
        state.get("ac2", {}).get("samples", 0) > 0
    touch = state.get("ac4", {})
    dome_fired = touch.get("dome", {}).get("distinguishable")
    body_fired = touch.get("body", {}).get("distinguishable")
    resist = state.get("ac5", {}).get("verdict")

    if resist == "detects_resistance":
        return {"verdict": "touch_detectable",
                "route": "dome resistance (AC5)",
                "why": "a hand on the dome measurably shortens a commanded "
                       "move, which needs no sensor stream at all"}
    if not stream_live:
        return {"verdict": "instrument_limited",
                "why": "the sensor stream never produced samples, so silence "
                       "is not evidence of anything. AC5 did not answer it "
                       "either." if resist != "does_not" else
                       "stream produced no samples; AC5 found no resistance, "
                       "but one negative on a single instrument is not a "
                       "verdict"}
    if dome_fired or body_fired:
        return {"verdict": "touch_detectable",
                "route": f"sensor stream ({'dome' if dome_fired else 'body'})",
                "why": "petting is separable from the rest baseline"}
    gross = state.get("ac2", {}).get("responds_to_gross_motion")
    if gross:
        return {"verdict": "motion_detectable_only",
                "why": "the stream reacts to being moved but not to being "
                       "touched; reactions to petting must be scheduled"}
    return {"verdict": "neither",
            "why": "stream is live and nothing separated touch or motion "
                   "from rest"}


# ---------------------------------------------------------------------------
# phases
# ---------------------------------------------------------------------------

def ac1_enable(bridge, state, args):
    mask, ext = masks()
    r = bridge.send_batch([Step("sensors", {
        "enable": True, "groups": GROUPS, "ext_groups": EXT_GROUPS,
        "interval": args.interval})], timeout=20)[0]
    state["ac1"] = {"ok": r.get("ok"), **(r.get("data") or {}),
                    "error": r.get("error")}
    save(state)
    d = state["ac1"]
    print(json.dumps({"accepted": d.get("accepted"),
                      "mask_confirmed": d.get("mask_confirmed"),
                      "errs": d.get("errs"),
                      "readback": d.get("readback")}, indent=2))
    if not d.get("accepted"):
        print("\nAC1 = refused. That is a RESULT, not a failure — record it "
              "and stop. Do not read later silence as 'he feels nothing'.")


def ac2_characterise(bridge, state, args):
    mask, ext = masks()
    samples = collect(bridge, args.seconds, mask, ext)
    bad = [s for s in samples if s["decode_error"]]
    rate = len(samples) / args.seconds if args.seconds else 0.0
    state["ac2"] = {"samples": len(samples), "decode_errors": len(bad),
                    "rate_hz": round(rate, 2),
                    "first_error": bad[0]["decode_error"] if bad else None,
                    "channels": sorted(channels(samples)),
                    "raw_sample": samples[0]["raw"] if samples else None}
    save(state)
    print(json.dumps(state["ac2"], indent=2))
    if not samples:
        print("\nNo sensor_stream events. Either the stream is off or the "
              "event ring is not seeing it — check AC1's readback before "
              "concluding anything.")


def ac3_baseline(bridge, state, args):
    mask, ext = masks()
    print(f"Hands OFF. Recording rest baseline for {args.seconds:.0f}s...")
    samples = collect(bridge, args.seconds, mask, ext)
    ch = channels(samples)
    # Store the raw SERIES, not a summary. `empirical_thresholds` derives the
    # limits from it at scoring time, so a rubric change can be re-applied to
    # an existing baseline instead of costing another 60 s of the operator's
    # patience -- which is exactly what happened when the sigma rule was
    # replaced. A stored mean/sigma pair would also imply sigma is still the
    # threshold source, and it is not.
    state["ac3"] = {"seconds": args.seconds, "samples": len(samples),
                    "rest_mean": {k: round(statistics.fmean(v), 5)
                                  for k, v in ch.items()},
                    # EVERY raw sample, not the first 50. The rubric had to
                    # be rewritten after the fact and a truncated baseline
                    # could not be re-analysed, costing a whole re-run.
                    "raw": [s["raw"] for s in samples],
                    "series": {k: v for k, v in ch.items()}}
    save(state)
    print(json.dumps({k: v for k, v in state["ac3"].items() if k != "raw"},
                     indent=2))


def ac4_touch(bridge, state, args):
    mask, ext = masks()
    series = state.get("ac3", {}).get("series")
    if not series:
        print("Run ac3-baseline first — a touch threshold without a rest "
              "baseline is meaningless. (If ac3 predates the rubric fix it "
              "has no `series`; re-run it.)")
        return
    rate = state.get("ac3", {}).get("samples", 0) / max(
        state.get("ac3", {}).get("seconds", 1), 1)
    window_n = max(4, int(rate * args.window))
    floor = empirical_thresholds(series, window_n)
    # ONE trial per invocation, fired immediately. Looping with input()
    # prompts blocks in a process the operator cannot see; a fixed lead-in
    # lands after they have looked away. They say "go", this collects.
    site = state.setdefault("ac4", {}).setdefault(
        args.site, {"trials": 0, "fired": 0, "detail": []})
    samples = collect(bridge, args.window, mask, ext)
    fired, chan = trial_fires(channels(samples), floor)
    site["trials"] += 1
    site["fired"] += int(fired)
    site["detail"].append({"trial": site["trials"], "fired": fired,
                           "channel": chan, "samples": len(samples),
                           "raw": [s["raw"] for s in samples[:20]]})
    site["distinguishable"] = site["fired"] >= TRIALS_TO_CONFIRM
    site["signal"] = next((d["channel"] for d in site["detail"]
                           if d["fired"]), None)
    save(state)
    print(f"trial {site['trials']} on {args.site}: "
          f"{'FIRED' if fired else 'nothing'}"
          f"{' on ' + chan if chan else ''}  ({len(samples)} samples)")
    print(f"  running: {site['fired']}/{site['trials']} fired -> "
          f"{'distinguishable' if site['distinguishable'] else 'not yet'}")


def ac5_resistance(bridge, state, args):
    start = bridge.send_batch([Step("head", {})], timeout=15)[0]["data"]["degrees"]
    # NO input() here. The operator is at the robot and this process is at
    # the queue -- a blocking prompt waits in a window nobody is watching.
    # They say "go" in chat while already holding the dome, and this fires.
    print(f"Commanding {RESIST_TRAVEL_DEG:+.0f}° from {start:.1f}° NOW.")
    bridge.send_batch([Step("dome", {"delta": RESIST_TRAVEL_DEG,
                                     "settle": 0})], timeout=15)
    time.sleep(3.0)
    landed = bridge.send_batch([Step("head", {})], timeout=15)[0]["data"]["degrees"]
    v = resistance_verdict(start, start + RESIST_TRAVEL_DEG, landed)
    state["ac5"] = {"start": round(start, 2), "landed": round(landed, 2),
                    "commanded_travel": RESIST_TRAVEL_DEG, **v}
    save(state)
    print(json.dumps(state["ac5"], indent=2))


def verdict(bridge, state, args):
    v = overall_verdict(state)
    state["ac6"] = v
    save(state)
    print(json.dumps(v, indent=2))


# ---------------------------------------------------------------------------
# selftest — every branch, on fabricated data, with no robot
# ---------------------------------------------------------------------------

def selftest(*_):
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: got {got!r} want {want!r}")

    # resistance branches
    check("resist/free", resistance_verdict(0, 15, 10)["verdict"], "does_not")
    check("resist/held", resistance_verdict(0, 15, 2.5)["verdict"],
          "detects_resistance")
    check("resist/nomove", resistance_verdict(0, 15, 0)["verdict"], "unresolved")
    check("resist/subthreshold", resistance_verdict(0, 5, 0)["verdict"],
          "unresolved")

    # trial firing, against an EMPIRICAL null built from rest itself
    import random
    rng = random.Random(7)
    rest = {"g": [rng.gauss(0.0, 0.1) for _ in range(400)],
            "h": [rng.gauss(0.0, 0.1) for _ in range(400)]}
    th = empirical_thresholds(rest, window_n=40)
    check("trial/quiet", trial_fires({"g": [rng.gauss(0.0, 0.1)
                                            for _ in range(40)]}, th)[0], False)
    # A "touch" is a DISTURBANCE. A constant value -- however large -- has
    # zero peak-to-peak and must NOT fire; that is the point of the statistic,
    # and these cases originally encoded the opposite.
    disturbed = {"g": [rng.gauss(0.0, 2.0) for _ in range(40)],
                 "h": [rng.gauss(0.0, 2.0) for _ in range(40)]}
    check("trial/touched", trial_fires(disturbed, th)[0], True)
    check("trial/names-channel", trial_fires(disturbed, th)[1], "g")
    check("trial/large-but-constant-does-not-fire",
          trial_fires({"g": [3.0] * 40}, th)[0], False)
    check("trial/unknown-channel-ignored",
          trial_fires({"nope": [9999.0]}, th)[0], False)
    # THE THIRD REGRESSION. One channel alone is not enough, however large.
    check("trial/single-channel-does-not-corroborate",
          trial_fires({"g": [rng.gauss(0.0, 5.0) for _ in range(40)]}, th)[0],
          False)
    # THE REGRESSION. A quiet window as LONG as the trials that broke the old
    # rubric (220 samples) must still not fire. The 6-sigma rule failed
    # exactly here: max-of-many-draws beats any sigma multiple by chance.
    long_quiet = {"g": [rng.gauss(0.0, 0.1) for _ in range(220)]}
    check("trial/long-quiet-window-does-not-fire",
          trial_fires(long_quiet, th)[0], False)
    # THE SECOND REGRESSION. A channel that is merely PARKED somewhere new
    # must not fire: a steady offset is not a disturbance. This is what broke
    # the no-touch control on r2_head_angle.
    offset_but_steady = {"g": [50.0 + rng.gauss(0.0, 0.02) for _ in range(40)]}
    check("trial/steady-offset-does-not-fire",
          trial_fires(offset_but_steady, th)[0], False)
    # ...while a real disturbance at the same offset still does.
    noisy_at_offset = {"g": [50.0 + rng.gauss(0.0, 3.0) for _ in range(40)],
                       "h": [50.0 + rng.gauss(0.0, 3.0) for _ in range(40)]}
    check("trial/disturbance-at-an-offset-fires",
          trial_fires(noisy_at_offset, th)[0], True)
    # And a threshold must not be derived from too little rest data.
    check("threshold/needs-enough-baseline",
          "g" in empirical_thresholds({"g": [0.1] * 10}, window_n=40), False)

    # overall verdict branches
    live = {"ac1": {"accepted": True}, "ac2": {"samples": 100}}
    check("verdict/stream-dead",
          overall_verdict({"ac1": {"accepted": False}})["verdict"],
          "instrument_limited")
    check("verdict/resistance-wins",
          overall_verdict({"ac5": {"verdict": "detects_resistance"}})["verdict"],
          "touch_detectable")
    check("verdict/petting",
          overall_verdict({**live, "ac4": {"dome": {"distinguishable": True}}})["verdict"],
          "touch_detectable")
    check("verdict/gross-only",
          overall_verdict({**live, "ac2": {"samples": 5,
                                           "responds_to_gross_motion": True}})["verdict"],
          "motion_detectable_only")
    check("verdict/neither",
          overall_verdict({**live, "ac4": {"dome": {"distinguishable": False}}})["verdict"],
          "neither")

    # a stream that accepted but produced nothing must NOT read as 'neither'
    check("verdict/accepted-but-silent",
          overall_verdict({"ac1": {"accepted": True}, "ac2": {"samples": 0}})["verdict"],
          "instrument_limited")

    total = 20
    print(f"{total - len(fails)}/{total} rubric cases passed")
    for f in fails:
        print("  FAIL", f)
    return 1 if fails else 0


PHASES = {"ac1-enable": ac1_enable, "ac2-characterise": ac2_characterise,
          "ac3-baseline": ac3_baseline, "ac4-touch": ac4_touch,
          "ac5-resistance": ac5_resistance, "verdict": verdict,
          "selftest": selftest}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("phase", choices=sorted(PHASES))
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--window", type=float, default=3.0)
    ap.add_argument("--site", default="dome",
                    choices=["dome", "body", "control"],
                    help="`control` = no-touch negative control; it MUST NOT fire")
    ap.add_argument("--interval", type=int, default=250)
    args = ap.parse_args()
    if args.phase == "selftest":
        return selftest()
    bridge = FileBridge()
    if not bridge.running():
        print("bridge not running. From Terminal:\n"
              "  cd ~/Projects/R2Z2/mac-prototype && ./r2 daemon --allow read")
        return 1
    state = load()
    PHASES[args.phase](bridge, state, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
