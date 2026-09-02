"""S1g — the three measurements the light language rests on.

Amendment A and docs/behaviour-states.md are fully specified and NONE of the
three assumptions underneath them has been measured:

  M1  holo dimming at a 120 ms STEP RATE. It dims cleanly at rest (255/64/16
      measured). The light language's only continuous channel depends on it
      still reading smooth when stepped at the command interval.
  M2  the firmware's OWN alternation rate. Ours is judged beside it -- if his
      resting alternation runs near `thinking`'s 0.9 s, that state is
      indistinguishable from him being himself.
  M3  PWM-duty linearity. Quiet hours scale value; if 25% duty looks nearly
      off, the dim frame is not a dim frame.

ORDER IS LOAD-BEARING. M2 must run BEFORE any LED write. A colour we set
survives until he sleeps (MEASURED 2026-08-18), so the first write destroys
his resting alternation for the rest of the session and M2 becomes
unmeasurable until the next sleep. This driver therefore has NO leds op in the
`alt` phase at all -- structurally, not by promise.

One phase per invocation. The operator is at the droid and cannot see this
console.

    python3 s1g_lights.py alt      # observe only. sends nothing but `status`
    python3 s1g_lights.py holo     # instrument check, then two ramps
    python3 s1g_lights.py duty     # instrument check, then a 5-step ladder
    python3 s1g_lights.py record --phase holo --answer stepped
    python3 s1g_lights.py park     # leave him in a defined state
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from r2_behavior import FileBridge, Step, front, back, holo, logic
import r2_lights as LG

RESULTS = Path(__file__).parent / ".bridge" / "s1g-results.json"
STEP_S = 0.12          # cmd_safe_interval: the rate the whole question is about
SLOW_S = 0.40          # the contrast rate, comfortably above the ceiling


# --------------------------------------------------------------------------
# transport
# --------------------------------------------------------------------------

def send(bridge, op, params, *, timeout=15.0):
    """One op. stderr is NEVER suppressed: a silent crash in a send loop reads
    exactly like a dead device, and that cost a previous session 15 minutes
    chasing a hardware fault that did not exist."""
    r = bridge.send_batch([Step(op, params)], timeout=timeout)
    print(f"    {op} {json.dumps(params)[:60]} -> "
          f"{'ok' if r and r[0].get('ok') else r}", file=sys.stderr)
    return r


def store(phase: str, **fields):
    """Persist BEFORE printing. A drain that formats and then discards has
    thrown away the only copy of evidence that cost operator attention."""
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    entry = data.setdefault(phase, {})
    entry.update(fields)
    entry["at"] = time.time()
    RESULTS.parent.mkdir(parents=True, exist_ok=True)
    RESULTS.write_text(json.dumps(data, indent=1))
    return entry


def link_ok(bridge) -> bool:
    if bridge.daemon() is None:
        print("no daemon holds the bridge. From a Terminal YOU open:\n"
              "  cd ~/Projects/R2Z2/mac-prototype && ./r2 daemon --allow leds",
              file=sys.stderr)
        return False
    # `head`, not `status`: r2_behavior.OP_TIER deliberately omits `status`,
    # because that layer names only the ops it may emit and the omission IS
    # the safety property. `head` is read-tier, already permitted, and a
    # successful reply proves R2 answered rather than merely that a daemon
    # process exists.
    r = send(bridge, "head", {})
    return bool(r and r[0].get("ok"))


# --------------------------------------------------------------------------
# verdicts — dry-tested on synthetic data before any hardware is involved
# --------------------------------------------------------------------------

HOLO_ANSWERS = ("smooth", "stepped", "flicker", "invisible", "missed")
DUTY_ANSWERS = ("even", "compressed", "bunched", "not_monotonic",
                "invisible", "missed")


def holo_verdict(instrument_ok: bool, fast: str, slow: str) -> dict:
    """M1. `instrument_limited` is a real outcome, not a tidy-up: if the
    operator could not see the holo at all, the ramp reading carries no
    information and must not be recorded as 'stepped'."""
    if not instrument_ok or fast in ("invisible", "missed"):
        return {"verdict": "instrument_limited",
                "why": "the holo was not visibly ON/OFF in the control, so "
                       "nothing the ramp did could be attributed to the ramp"}
    if fast == "smooth":
        return {"verdict": "confirmed",
                "why": f"reads smooth at the {STEP_S:.2f}s command interval; "
                       f"the holo can carry a fade at the rate we can drive"}
    if slow == "smooth":
        return {"verdict": "rate_limited",
                "why": f"smooth at {SLOW_S:.2f}s but {fast} at {STEP_S:.2f}s "
                       f"-- the fixture dims, our command rate is the limit"}
    return {"verdict": "refuted",
            "why": f"{fast} at {STEP_S:.2f}s and {slow} at {SLOW_S:.2f}s -- "
                   f"stepping does not read as a fade at any rate we can "
                   f"drive, so the holo is not the continuous channel "
                   f"Amendment A section 3 assigns it"}


def duty_verdict(instrument_ok: bool, answer: str) -> dict:
    """M3. The quiet-hours frame is only a dim frame if the ladder reads as a
    ladder."""
    if not instrument_ok or answer in ("invisible", "missed"):
        return {"verdict": "instrument_limited",
                "why": "full vs off was not visibly different in the control"}
    if answer == "even":
        return {"verdict": "confirmed",
                "why": "duty tracks perceived brightness closely enough that "
                       "a scaled corner is a legible dim frame"}
    if answer == "compressed":
        return {"verdict": "confirmed_nonlinear",
                "why": "monotonic but bunched -- quiet hours work, and the "
                       "dim VALUE needs choosing by eye rather than by "
                       "arithmetic"}
    if answer == "bunched":
        return {"verdict": "refuted",
                "why": "the low end is indistinguishable from off, so the "
                       "quiet-hours frame is not a dim frame, it is a dark "
                       "one"}
    return {"verdict": "refuted",
            "why": "not monotonic: a higher duty did not read brighter, so "
                   "scaling cannot be used to express anything"}


def alt_verdict(alternating: bool, cycles: int, seconds: float) -> dict:
    """M2. If he is showing a colour WE set rather than his own alternation,
    the measurement is impossible -- and reporting a period anyway would be
    inventing one."""
    if not alternating:
        return {"verdict": "instrument_limited",
                "why": "he was not alternating, so there was no resting "
                       "cadence to time. Something has written a colour since "
                       "he last slept."}
    if cycles < 3 or seconds <= 0:
        return {"verdict": "instrument_limited",
                "why": f"{cycles} cycles over {seconds}s is too few to time"}
    period = seconds / cycles
    collides = abs(period - 0.9) < 0.25
    return {"verdict": "measured", "period_s": round(period, 2),
            "collides_with_thinking": collides,
            "why": (f"{cycles} cycles in {seconds}s = {period:.2f}s per cycle. "
                    + ("COLLIDES with `thinking` at 0.9s -- that state would "
                       "be hard to tell from him being himself."
                       if collides else
                       "Clear of `thinking` at 0.9s."))}


# --------------------------------------------------------------------------
# phases
# --------------------------------------------------------------------------

def phase_alt(bridge) -> int:
    """M2. Sends NOTHING but `status`. No leds op appears in this function --
    the first write would end the measurement for the rest of the session."""
    if not link_ok(bridge):
        return 2
    store("alt", started=time.time(), method="passive observation, no writes")
    print("\nM2 -- his own alternation. NOTHING is being sent.\n"
          "Watch the FRONT dome lights and time TEN full colour cycles\n"
          "(red -> blue -> red counts as one). A phone stopwatch is fine.\n\n"
          "If he is holding ONE steady colour instead of alternating, say so --\n"
          "that is the answer, and it means something wrote to him since he\n"
          "last slept. Do not estimate a period in that case.\n", file=sys.stderr)
    return 0


def _instrument_check(bridge, label, on_channels, off_channels):
    """Fire the case whose answer is already known, FIRST. A channel that is
    dead produces the quiet answer, and quiet reads as a real measurement."""
    print(f"\n  instrument check: {label} -- OFF, then FULL, twice",
          file=sys.stderr)
    for _ in range(2):
        send(bridge, "leds", {"channels": off_channels}); time.sleep(0.9)
        send(bridge, "leds", {"channels": on_channels});  time.sleep(0.9)
    send(bridge, "leds", {"channels": off_channels})


def phase_holo(bridge) -> int:
    """M1. Instrument check, then the same ramp at two rates."""
    if not link_ok(bridge):
        return 2
    _instrument_check(bridge, "holo (bit 7)", holo(255), holo(0))
    ramp = [16, 32, 48, 64, 96, 128, 160, 192, 224, 255]
    for label, gap in (("FAST", STEP_S), ("SLOW", SLOW_S)):
        print(f"\n  ramp {label} ({gap:.2f}s per step, {len(ramp)} steps)",
              file=sys.stderr)
        time.sleep(1.2)
        for lvl in ramp:
            send(bridge, "leds", {"channels": holo(lvl)})
            time.sleep(gap)
        send(bridge, "leds", {"channels": holo(0)})
    store("holo", ramp=ramp, fast_gap_s=STEP_S, slow_gap_s=SLOW_S)
    return 0


def phase_duty(bridge) -> int:
    """M3. Green because it is mid-luminance (0.715) -- red at 0.213 would put
    the whole ladder near the floor and test the floor, not the ladder."""
    if not link_ok(bridge):
        return 2
    _instrument_check(bridge, "front PSI green", front(LG.BASE_SUCCESS),
                      front((0, 0, 0)))
    steps = [1.0, 0.75, 0.5, 0.25, 0.10]
    print(f"\n  ladder: {', '.join(f'{int(s*100)}%' for s in steps)} "
          f"-- 2.5s each, announced in order", file=sys.stderr)
    time.sleep(1.2)
    for s in steps:
        rgb = tuple(int(round(c * s)) for c in LG.BASE_SUCCESS)
        send(bridge, "leds", {"channels": front(rgb)})
        time.sleep(2.5)
    send(bridge, "leds", {"channels": front((0, 0, 0))})
    store("duty", steps=steps, colour="GREEN")
    return 0


def phase_park(bridge) -> int:
    """Leave him in a defined state. Whatever a session leaves him in is what
    the household sees until he sleeps."""
    if not link_ok(bridge):
        return 2
    send(bridge, "leds", {"channels": LG.assertion("idle")})
    print("\n  parked on idle blue.", file=sys.stderr)
    return 0


def cmd_record(args) -> int:
    if args.phase == "holo":
        v = holo_verdict(args.instrument_ok, args.fast, args.slow)
    elif args.phase == "duty":
        v = duty_verdict(args.instrument_ok, args.answer)
    else:
        v = alt_verdict(args.alternating, args.cycles, args.seconds)
    entry = store(args.phase, **v)
    print(json.dumps(entry, indent=1))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for p in ("alt", "holo", "duty", "park"):
        sub.add_parser(p)
    r = sub.add_parser("record")
    r.add_argument("--phase", required=True, choices=("alt", "holo", "duty"))
    r.add_argument("--instrument-ok", dest="instrument_ok",
                   action="store_true")
    r.add_argument("--fast", choices=HOLO_ANSWERS, default="missed")
    r.add_argument("--slow", choices=HOLO_ANSWERS, default="missed")
    r.add_argument("--answer", choices=DUTY_ANSWERS, default="missed")
    r.add_argument("--alternating", action="store_true")
    r.add_argument("--cycles", type=int, default=0)
    r.add_argument("--seconds", type=float, default=0.0)
    args = ap.parse_args()
    if args.cmd == "record":
        return cmd_record(args)
    bridge = FileBridge()
    return {"alt": phase_alt, "holo": phase_holo,
            "duty": phase_duty, "park": phase_park}[args.cmd](bridge)


if __name__ == "__main__":
    sys.exit(main())
