#!/usr/bin/env python3
"""r2_reactive.py — touch in, behaviour out. The first closed loop.

S1e proved R2 can feel a hand (#29). S1c/S1d gave him something to say
(`express_curious`, #43). Neither half is worth anything alone: a sensor
nobody listens to, and an expression nobody triggers. This joins them.

    hand on the dome -> disturbance clears what rest produces
                     -> WAIT for him to stop rocking
                     -> express_curious()
                     -> wait for quiet again, re-arm

Run it as one continuous session. Unlike the S1e survey there is nothing to
fire on "go": the operator pets him whenever they like and the loop is
already listening. It is bounded in wall-clock and in reaction count anyway,
because CLAUDE.md forbids leaving loops running unattended.

    ./r2 daemon --allow dome          # express_curious is a dome-tier beat
    python3 r2_reactive.py selftest   # no hardware, no daemon
    python3 r2_reactive.py run

THE THREE THINGS THAT MAKE THIS HARD, none of which are the detection:

1. HE TRIGGERS HIMSELF. The beat turns the dome, the dome disturbs every
   channel the detector watches, and a naive loop re-fires on its own
   aftermath forever. Recovery is therefore not a fixed sleep -- it waits
   for measured quiet, on the same statistic that armed it.

2. HE KEEPS MOVING AFTER THE HAND LEAVES. Detection is the easy edge; the
   trailing edge is not an edge at all. Reacting the instant the threshold
   trips means the beat's opening dome move fights a body still rocking, and
   the response reads as a twitch rather than an answer. So detection starts
   a SETTLE, not a beat.

3. A DETECTOR STUCK ON LOOKS EXACTLY LIKE A GOOD ONE. This is the rule that
   cost the S1e survey ten wasted pets: a clean sweep is a prompt to check
   the null case, not a result. So the negative control is not optional here
   and not a flag -- it runs automatically before the loop arms, and a
   detector that fires on nothing REFUSES to arm at all.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_probe as P
import r2_behavior as B
from r2_behavior import FileBridge, Step
from sensor_probe import (GROUPS, EXT_GROUPS, masks, channels,
                          empirical_thresholds, trial_fires, window_stat)

LOG_DIR = Path(__file__).parent / ".bridge"


def log_path(stamp: float) -> Path:
    """One file per run. A single fixed filename silently destroyed the run
    it was most useful to compare against: the desk session's raw samples
    were overwritten by the floor session that was trying to explain them."""
    return LOG_DIR / f"s2b-reactive-{int(stamp)}.json"

# The stream runs at 4.00 Hz (S1e, measured). That is the latency floor for
# everything below and it is hardware, not code: a corroborated detection
# needs several samples, and several samples take about a second. Anything
# in this file that looks slow is usually 4 Hz wearing a different hat.
STREAM_HZ = 4.0

# ~6 samples. Must equal the window the thresholds were derived over --
# like-for-like or nothing. S1e validated the rubric at 3.0 s; this halves it
# for responsiveness, which is a real departure and is why --window exists.
# A shorter window means fewer draws in both the null and the trial, so
# false positives get likelier -- but the negative control adjudicates that
# empirically before anything arms, rather than us guessing here.
DETECT_WINDOW_S = 1.5
S1E_VALIDATED_WINDOW_S = 3.0
BASELINE_S = 30.0
CONTROL_S = 20.0           # hands-off proof the detector can stay silent
POLL_S = 0.25

# After a detection, wait for this much continuous quiet before performing.
# He rocks on for a while after the hand leaves; reacting into that produces
# a beat fighting a moving body.
SETTLE_QUIET_S = 1.5
SETTLE_MAX_S = 6.0         # a hand that never leaves still gets an answer

# After the beat, the dome he just turned is the loudest thing in the room.
RECOVER_QUIET_S = 2.0
RECOVER_MAX_S = 15.0

MAX_RUNTIME_S = 180.0
MAX_REACTIONS = 5


# ---------------------------------------------------------------------------
# the feed
# ---------------------------------------------------------------------------

class Feed:
    """Trailing samples off the sensor stream, decoded, newest last.

    Owns the flush discipline that S1e got wrong four different ways: the
    daemon's event ring keeps filling whether anyone is reading or not, so a
    window drained after any gap sweeps in history from before the gap. Every
    phase boundary here flushes first.
    """

    def __init__(self, bridge, mask: int, ext: int, keep: int = 400):
        self.bridge, self.mask, self.ext, self.keep = bridge, mask, ext, keep
        self.samples: list[dict] = []
        self.dropped = 0
        self.decode_errors = 0

    def flush(self) -> None:
        """Discard whatever accumulated, and forget what we already held.

        Used at every phase boundary, and especially after a beat: the samples
        our own dome move produced are not evidence about a hand."""
        self.bridge.send_batch([Step("events", {})], timeout=15)
        self.samples.clear()

    def drain(self) -> int:
        """Pull whatever arrived since the last call. Returns how many."""
        r = self.bridge.send_batch([Step("events", {})], timeout=15)[0]
        data = r.get("data") or {}
        self.dropped += data.get("dropped", 0) or 0
        n = 0
        for ev in data.get("events", []):
            if ev.get("name") != "sensor_stream":
                continue
            try:
                decoded = P.decode_sensor_stream(
                    bytes.fromhex(ev["data"]), self.mask, self.ext)
            except (ValueError, KeyError):
                # A length mismatch means our idea of the mask disagrees with
                # the robot's. Count it; never guess at a partial decode.
                self.decode_errors += 1
                continue
            self.samples.append({"t": ev["t"], "raw": ev["data"],
                                 "decoded": decoded})
            n += 1
        if len(self.samples) > self.keep:
            self.samples = self.samples[-self.keep:]
        return n

    def window(self, n: int) -> list[dict]:
        """The trailing `n` samples, or nothing if we do not have `n` yet.

        Returning a SHORT window would silently weaken the test: peak-to-peak
        over 2 samples is smaller than over 6 for the same disturbance, so a
        partial window reads as calm and the detector goes deaf exactly when
        it has least data."""
        return self.samples[-n:] if len(self.samples) >= n else []


# ---------------------------------------------------------------------------
# the rubric, reused verbatim from the survey that validated it
# ---------------------------------------------------------------------------

def window_n_for(rate_hz: float, seconds: float) -> int:
    """Samples per detection window. Floored at 4: below that, peak-to-peak
    is two or three draws and the null distribution is meaningless."""
    return max(4, int(round(rate_hz * seconds)))


def fires(window: list[dict], thresholds) -> tuple[bool, str | None]:
    if not window:
        return False, None
    return trial_fires(channels(window), thresholds)


def ratios(window: list[dict], thresholds) -> dict[str, float]:
    """How close each channel came to its limit, as a fraction of it.

    `fires` collapses this to a boolean, and a boolean is what made two live
    runs undiagnosable: "0 reactions" cannot distinguish "nobody touched him"
    from "he felt it and the margin was too high". A ratio of 0.05 and a
    ratio of 0.9 are completely different findings and both report as False.
    """
    ch = channels(window)
    out = {}
    for name, (_, limit) in thresholds.items():
        vals = ch.get(name)
        if vals and limit > 0:
            out[name] = window_stat(vals) / limit
    return out


# ---------------------------------------------------------------------------
# the loop
# ---------------------------------------------------------------------------

class Reactive:
    """The closed loop. `now`/`sleep` are injected so the whole state machine
    runs in a unit test at no wall-clock cost -- which is the only reason the
    settle and recovery paths have ever been exercised, since neither fires
    on a robot that is behaving."""

    def __init__(self, bridge, feed, thresholds, window_n, *,
                 ceiling: str = "dome", beat_factory=B.express_curious,
                 now=time.monotonic, sleep=time.sleep):
        self.bridge, self.feed = bridge, feed
        self.thresholds, self.window_n = thresholds, window_n
        self.ceiling, self.beat_factory = ceiling, beat_factory
        self.now, self.sleep = now, sleep
        self.reactions: list[dict] = []

    # -- phases ------------------------------------------------------------

    def _watch_until(self, deadline: float, want_quiet_s: float | None):
        """Poll until `deadline`. If `want_quiet_s` is set, return as soon as
        that much continuous quiet has passed; otherwise return on the first
        firing. Returns (outcome, channel) where outcome is one of
        'fired' | 'quiet' | 'timeout'."""
        quiet_since = None
        last_chan = None
        while self.now() < deadline:
            self.feed.drain()
            win = self.feed.window(self.window_n)
            hot, chan = fires(win, self.thresholds)
            if hot:
                last_chan = chan
                quiet_since = None
                if want_quiet_s is None:
                    return "fired", chan
            elif win:
                # Only a FULL window counts toward quiet. A partial window is
                # not evidence of calm, it is absence of data -- and treating
                # it as calm is how a recovery wait ends early, straight after
                # the flush that emptied the ring.
                if quiet_since is None:
                    quiet_since = self.now()
                elif want_quiet_s is not None and \
                        self.now() - quiet_since >= want_quiet_s:
                    return "quiet", last_chan
            self.sleep(POLL_S)
        return "timeout", last_chan

    def control(self, seconds: float) -> dict:
        """The negative control. Hands off; the detector must stay silent.

        Runs BEFORE the loop arms and gates it. This is rule one in CLAUDE.md
        and it is here in code rather than in a checklist because the failure
        it catches is invisible from the inside: four separate bugs in S1e
        each produced a confident 10/10, and a positive control could not
        have exposed any of them.
        """
        self.feed.flush()
        end = self.now() + seconds
        outcome, chan = self._watch_until(end, want_quiet_s=None)
        ok = outcome != "fired"
        return {"ok": ok, "seconds": seconds, "outcome": outcome,
                "channel": chan,
                "why": "detector stayed silent with nothing touching him" if ok
                       else f"detector fired on {chan} with nobody touching "
                            f"him -- it is stuck on, and every reaction it "
                            f"produces would be void"}

    def react(self) -> dict:
        """Settle, perform, recover. Returns the record for one reaction."""
        t0 = self.now()
        settle, _ = self._watch_until(self.now() + SETTLE_MAX_S,
                                      want_quiet_s=SETTLE_QUIET_S)
        settled_s = round(self.now() - t0, 2)

        beat = self.beat_factory()
        result = B.perform(beat, self.bridge,
                           ceiling=self.ceiling, sleep=self.sleep)
        beat_ended = self.now()

        # Everything in the ring now is the beat's own noise, not a hand.
        self.feed.flush()
        recover, _ = self._watch_until(self.now() + RECOVER_MAX_S,
                                       want_quiet_s=RECOVER_QUIET_S)

        # HONOUR THE BEAT'S OWN COOLDOWN. `Beat.cooldown_s` is not decoration
        # -- it is one of the four fields the Behavior Library requires
        # precisely so R2 can be scheduled without becoming twitchy, and a
        # loop that ignores it re-fires the same expression as fast as the
        # sensor allows. Recovery-quiet and cooldown are different gates:
        # quiet is instrument hygiene (is the signal usable again), cooldown
        # is character pacing (should he say this again yet). Both apply.
        held = 0.0
        if beat.cooldown_s:
            held = max(0.0, beat.cooldown_s - (self.now() - beat_ended))
            if held:
                self.sleep(held)
        return {"settle": settle, "settle_s": settled_s,
                "cooldown_held_s": round(held, 2),
                "beat": result.get("beat"), "beat_ok": result.get("ok"),
                "refused": result.get("refused"),
                "beat_error": result.get("error"),
                "elapsed_s": result.get("elapsed_s"),
                "residual_deg": result.get("residual_deg"),
                "recover": recover}

    def run(self, max_s: float, max_reactions: int) -> dict:
        """Arm and stay armed until one of the two bounds is hit."""
        # Blue steady: "on / waiting / neutral" (D-012). The operator needs to
        # know from across the room that he is listening -- a state that lived
        # only in my console would be invisible to the person whose hand is
        # the input device.
        #
        # And it must be a TRANSITION, not a colour. The first live run armed
        # him blue while he was ALREADY blue -- the previous session's own
        # teardown had left him there -- so the one signal telling the
        # operator "now" was indistinguishable from the state before it. They
        # waited for a cue that had already happened. `dark()` below is what
        # makes this edge visible; neither half works alone.
        self.bridge.send_batch([Step("leds", {"channels":
                                              B.front(B.BASE_NEUTRAL)})])
        end = self.now() + max_s
        while self.now() < end and len(self.reactions) < max_reactions:
            # FLUSH ON EVERY ARM, not once before the loop. Recovery can end
            # on its cap while the disturbance is still going -- and re-arming
            # then reads a window that is five parts aftermath to one part
            # present, fires immediately, and R2 answers himself. Measured:
            # one pet produced two reactions. Arming on samples collected
            # before this moment is never right, so the invariant belongs
            # here rather than in a longer recovery timeout.
            self.feed.flush()
            outcome, chan = self._watch_until(end, want_quiet_s=None)
            if outcome != "fired":
                break
            rec = self.react()
            rec["trigger_channel"] = chan
            rec["at_s"] = round(max_s - (end - self.now()), 2)
            self.reactions.append(rec)
        return {"reactions": self.reactions,
                "count": len(self.reactions),
                "stopped": "max_reactions" if len(self.reactions)
                           >= max_reactions else "time",
                "dropped_events": self.feed.dropped,
                "decode_errors": self.feed.decode_errors}


# ---------------------------------------------------------------------------
# session
# ---------------------------------------------------------------------------

def calibrate(bridge, feed, seconds: float, window_n: int,
              *, now=time.monotonic, sleep=time.sleep) -> dict:
    """Record rest and derive what rest itself produces over one window.

    `now`/`sleep` are injected for the same reason they are on Reactive: a
    baseline that can only be exercised in real time is a baseline nobody
    tests, and the branch that matters here (too few channels to corroborate)
    would then never run outside a hardware session."""
    feed.flush()
    end = now() + seconds
    while now() < end:
        feed.drain()
        sleep(POLL_S)
    series = channels(feed.samples)
    thresholds = empirical_thresholds(series, window_n)
    rate = len(feed.samples) / seconds if seconds else 0.0
    return {"samples": len(feed.samples), "rate_hz": round(rate, 2),
            "channels": len(series), "thresholds": len(thresholds),
            "series": series, "limits": {k: round(v[1], 5)
                                         for k, v in thresholds.items()},
            "_thresholds": thresholds}


def monitor_session(args) -> int:
    """Record how close he comes to firing, and decide nothing.

    Exists because two live runs returned "0 reactions" and that number
    answers no question worth asking: it is equally consistent with an
    untouched robot and with a detector whose margin is too high to ever
    trip. This drops the verdict and keeps the evidence -- every window's
    ratio against every channel's limit, plus the raw hex -- so the threshold
    can be set from data instead of from an assumption, offline, without
    spending anyone's hands on another blind trial.
    """
    bridge = FileBridge()
    held = bridge.daemon()
    if held is None:
        print("no daemon holds the bridge. Start it from Terminal:\n"
              "  cd ~/Projects/R2Z2/mac-prototype && ./r2 daemon --allow read")
        return 2

    mask, ext = masks()
    feed = Feed(bridge, mask, ext, keep=10_000)
    started = time.time()
    window_n = window_n_for(STREAM_HZ, args.window)

    bridge.send_batch([Step("leds", {"channels": B.front((0, 0, 0))})],
                      timeout=15)
    r = bridge.send_batch([Step("sensors", {"enable": True, "groups": GROUPS,
                                            "ext_groups": EXT_GROUPS})],
                          timeout=15)[0]
    if not r.get("ok"):
        print(f"could not enable the sensor stream: {r}")
        return 3

    log = {"started": started, "mode": "monitor",
           "config": {"window_s": args.window, "baseline_s": args.baseline,
                      "monitor_s": args.max_s}}
    try:
        print(f"HANDS OFF. Baseline for {args.baseline:.0f}s...")
        cal = calibrate(bridge, feed, args.baseline, window_n)
        th = cal["_thresholds"]
        log["calibration"] = {k: v for k, v in cal.items()
                              if k != "_thresholds"}
        print(f"  {cal['samples']} samples at {cal['rate_hz']} Hz, "
              f"{cal['thresholds']} channels have limits")

        bridge.send_batch([Step("leds", {"channels":
                                         B.front(B.BASE_NEUTRAL)})], timeout=15)
        print(f"\nBLUE NOW — pet him whenever for the next "
              f"{args.max_s:.0f}s. Nothing will move; I am only watching.\n")
        feed.flush()
        track: list[dict] = []
        end = time.monotonic() + args.max_s
        while time.monotonic() < end:
            feed.drain()
            win = feed.window(window_n)
            if win:
                rs = ratios(win, th)
                top = sorted(rs.items(), key=lambda kv: -kv[1])[:3]
                track.append({
                    "t": round(args.max_s - (end - time.monotonic()), 2),
                    "over": sum(1 for v in rs.values() if v >= 1.0),
                    "top": [(k, round(v, 3)) for k, v in top]})
            time.sleep(POLL_S)

        log["track"] = track
        log["raw"] = [s["raw"] for s in feed.samples]
        peak = max(track, key=lambda r: r["top"][0][1] if r["top"] else 0.0,
                   default=None)
        # Every moment that got anywhere near the limit, so a real touch is
        # visible even when nothing crossed.
        hits = [r for r in track if r["top"] and r["top"][0][1] >= 0.30]
        print(f"{len(track)} windows scored.")
        if peak:
            print(f"peak: t={peak['t']}s  {peak['top']}  "
                  f"({peak['over']} channels over their limit)")
        print(f"windows reaching 30% of a limit: {len(hits)}")
        for r in hits[:25]:
            print(f"   t={r['t']:6.2f}s  over={r['over']}  {r['top']}")
        return 0
    finally:
        log["ended"] = time.time()
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        out = LOG_DIR / f"s2b-monitor-{int(started)}.json"
        out.write_text(json.dumps(log, indent=2))
        print(f"log: {out.name}")
        try:
            bridge.send_batch([
                Step("sensors", {"enable": False}),
                Step("leds", {"channels": B.front(B.BASE_NEUTRAL)}),
            ], timeout=15)
        except Exception as e:
            print(f"!! could not tidy up: {type(e).__name__}: {e}")


def run_session(args, *, now=time.monotonic, sleep=time.sleep) -> int:
    """`now`/`sleep` are injected for the same reason they are everywhere else
    in this file: every bug in this feature that reached live hardware lived
    in how the PHASES were wired together, not in the pieces -- and a session
    that can only be exercised in real time is a session nobody tests."""
    bridge = FileBridge()
    held = bridge.daemon()
    if held is None:
        print("no daemon holds the bridge. Start it from Terminal:\n"
              "  cd ~/Projects/R2Z2/mac-prototype && ./r2 daemon --allow dome")
        return 2

    # ASK THE DAEMON what it will permit rather than believing --ceiling. The
    # flag is the operator's memory of how they launched it; the lock is what
    # they actually launched. Getting this wrong means `perform` clears its
    # own pre-flight check and R2 is then refused mid-gesture by the daemon,
    # which is the one state the pre-flight exists to prevent.
    ceiling = held.get("ceiling") or args.ceiling
    if ceiling != args.ceiling:
        print(f"note: daemon is running at --allow {ceiling!r}, not "
              f"{args.ceiling!r}; using the daemon's.")

    # A fresh session inherits a dome parked wherever yesterday left it, so
    # the process-wide drift anchor must not carry over.
    B.reset_default_home()

    mask, ext = masks()
    feed = Feed(bridge, mask, ext)
    started = time.time()
    log = {"started": started, "config": {
        "detect_window_s": args.window, "baseline_s": args.baseline,
        "control_s": args.control, "settle_quiet_s": SETTLE_QUIET_S,
        "recover_quiet_s": RECOVER_QUIET_S, "max_s": args.max_s,
        "max_reactions": args.max_reactions, "ceiling": ceiling}}

    # Dark through calibration and the control, so that going blue at arm
    # time is a change the operator can SEE rather than a colour they have to
    # have been told to expect.
    bridge.send_batch([Step("leds", {"channels": B.front((0, 0, 0))})],
                      timeout=15)

    r = bridge.send_batch([Step("sensors", {"enable": True, "groups": GROUPS,
                                            "ext_groups": EXT_GROUPS})],
                          timeout=15)[0]
    log["stream_enabled"] = r.get("ok")
    if not r.get("ok"):
        print(f"could not enable the sensor stream: {r}")
        return 3

    try:
        window_n = window_n_for(STREAM_HZ, args.window)
        print(f"HANDS OFF. Calibrating rest for {args.baseline:.0f}s "
              f"(window {window_n} samples)...")
        cal = calibrate(bridge, feed, args.baseline, window_n,
                        now=now, sleep=sleep)
        log["calibration"] = {k: v for k, v in cal.items()
                              if k not in ("_thresholds", "series")}
        log["calibration"]["series"] = cal["series"]
        print(f"  {cal['samples']} samples at {cal['rate_hz']} Hz, "
              f"{cal['thresholds']} channels have limits")
        if cal["thresholds"] < 2:
            print("REFUSING to arm: fewer than two channels produced a usable "
                  "limit, so the two-channel corroboration rule can never be "
                  "met and nothing would ever fire.")
            return 4

        loop = Reactive(bridge, feed, cal["_thresholds"], window_n,
                        ceiling=ceiling, now=now, sleep=sleep)

        print(f"STILL HANDS OFF. Negative control for {args.control:.0f}s — "
              f"proving it can stay quiet...")
        ctl = loop.control(args.control)
        log["control"] = ctl
        print(f"  {ctl['outcome']}: {ctl['why']}")
        if not ctl["ok"]:
            print("REFUSING to arm. Every reaction this produced would be "
                  "void, and a stuck detector looks exactly like a working "
                  "one from the loud side.")
            if args.window < S1E_VALIDATED_WINDOW_S:
                print(f"  This ran at a {args.window}s window. S1e validated "
                      f"the rubric at {S1E_VALIDATED_WINDOW_S}s — a longer "
                      f"window is a harder test to trip. Retry:\n"
                      f"    python3 r2_reactive.py run --window "
                      f"{S1E_VALIDATED_WINDOW_S}")
            return 5

        print(f"\nARMED for {args.max_s:.0f}s (blue = listening). "
              f"Pet him whenever — up to {args.max_reactions} reactions.\n")
        out = loop.run(args.max_s, args.max_reactions)
        log["run"] = out
        for i, rec in enumerate(out["reactions"], 1):
            print(f"  reaction {i} at {rec['at_s']:.0f}s: "
                  f"triggered on {rec['trigger_channel']}, "
                  f"settle={rec['settle']} ({rec['settle_s']}s), "
                  f"beat {'ok' if rec['beat_ok'] else 'FAILED'} "
                  f"in {rec['elapsed_s']}s, recover={rec['recover']}")
        print(f"\n{out['count']} reactions, stopped on {out['stopped']}."
              f"  dropped={out['dropped_events']} "
              f"decode_errors={out['decode_errors']}")
        return 0
    finally:
        # Turn off what we turned on, and leave him in a defined colour.
        # Whatever a session leaves lit is what the household sees until
        # something changes it.
        log["ended"] = time.time()
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        out = log_path(started)
        out.write_text(json.dumps(log, indent=2))
        print(f"log: {out.name}")
        try:
            bridge.send_batch([
                Step("sensors", {"enable": False}),
                Step("leds", {"channels": B.front(B.BASE_NEUTRAL)}),
            ], timeout=15)
        except Exception as e:
            print(f"!! could not tidy up: {type(e).__name__}: {e}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("phase", choices=["run", "monitor", "selftest"])
    ap.add_argument("--baseline", type=float, default=BASELINE_S)
    ap.add_argument("--window", type=float, default=DETECT_WINDOW_S,
                    help=f"detection window in seconds (S1e validated "
                         f"{S1E_VALIDATED_WINDOW_S})")
    ap.add_argument("--control", type=float, default=CONTROL_S)
    ap.add_argument("--max-s", type=float, default=MAX_RUNTIME_S)
    ap.add_argument("--max-reactions", type=int, default=MAX_REACTIONS)
    ap.add_argument("--ceiling", default="dome",
                    help="the --allow level the daemon was started with")
    args = ap.parse_args()
    if args.phase == "selftest":
        import test_r2_reactive
        return test_r2_reactive.run()
    if args.phase == "monitor":
        return monitor_session(args)
    return run_session(args)


if __name__ == "__main__":
    raise SystemExit(main())
