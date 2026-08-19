#!/usr/bin/env python3
"""r2_reactive.py — touch in, behaviour out. The first closed loop.

S1e proved R2 can feel a hand (#29). S1c/S1d gave him something to say
(`express_curious`, #43). Neither half is worth anything alone: a sensor
nobody listens to, and an expression nobody triggers. This joins them.

    hand on the dome -> disturbance clears what rest produces
                     -> WAIT for him to stop rocking
                     -> express_curious()
                     -> wait for quiet again, hold the beat's cooldown, re-arm

A control runs at BOTH ends. The opening one refuses to arm a detector that
fires on nothing; the closing one marks the whole run suspect if specificity
drifted somewhere inside it. One control proves less than half of what two do,
and the thresholds are frozen from a baseline taken in a pose the dome does
not stay in.

Run it as one continuous session. Unlike the S1e survey there is nothing to
fire on "go": the operator pets him whenever they like and the loop is
already listening. It is bounded in wall-clock and in reaction count anyway,
because CLAUDE.md forbids leaving loops running unattended.

    ./r2 daemon --allow dome          # express_curious is a dome-tier beat
    python3 r2_reactive.py selftest   # no hardware, no daemon
    python3 r2_reactive.py run

    ./r2 daemon --allow read          # monitor moves NOTHING, so `read` is
    python3 r2_reactive.py monitor    # the correct ceiling for it

`monitor` records how close every window came to firing and decides nothing.
Reach for it whenever a `run` returns a bare "0 reactions": that number is
equally consistent with an untouched robot and with a threshold too high to
ever trip, and telling those apart by re-running `run` costs the operator's
hands for no information.

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
import r2_lights as LG
from r2_behavior import FileBridge, Step
from r2_status import StatusLayer, StatusStore
from sensor_probe import (GROUPS, EXT_GROUPS, masks, channels,
                          empirical_thresholds, trial_fires, window_stat)

LOG_DIR = Path(__file__).parent / ".bridge"

# Patchable for the same reason LOG_DIR is. Left as None the StatusLayer uses
# its own default, which is the REAL store -- and the test suite promptly wrote
# {"state": "idle"} into it. A test run must not be able to tell the next live
# session what R2 is.
STATUS_STORE: Path | None = None


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
        self.flushes = 0
        # Consecutive drains that returned nothing. A DEAD stream is the one
        # failure this whole design cannot see by itself: the buffer keeps its
        # last full window, every poll re-scores those same samples, and
        # "quiet" comes back forever. A settle completes, the beat performs,
        # and a 180 s armed session reports 0 reactions -- identical to an
        # untouched robot. CLAUDE.md: prove the channel live before trusting
        # silence.
        self.stale_polls = 0

    def flush(self) -> None:
        """Discard whatever accumulated, and forget what we already held.

        Used at every phase boundary, and especially after a beat: the samples
        our own dome move produced are not evidence about a hand."""
        self.bridge.send_batch([Step("events", {})], timeout=15)
        self.samples.clear()
        self.stale_polls = 0
        self.flushes += 1

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
        self.stale_polls = 0 if n else self.stale_polls + 1
        return n

    def stalled(self, polls: int) -> bool:
        """Has the stream produced NOTHING for `polls` consecutive drains?

        Separate from `dropped`, which counts ring evictions -- the opposite
        problem. Nothing in the record distinguished a deaf session from a
        calm one before this."""
        return self.stale_polls >= polls

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
                 status: StatusLayer | None = None,
                 now=time.monotonic, sleep=time.sleep):
        self.bridge, self.feed = bridge, feed
        self.thresholds, self.window_n = thresholds, window_n
        self.ceiling, self.beat_factory = ceiling, beat_factory
        # Constructed rather than left None when absent. An optional status
        # layer is not a status layer: every call site would accept the
        # default and the wiring would be inert, which is the exact shape
        # that left r2_lights unimported for three PRs.
        self.status = status if status is not None else StatusLayer(bridge)
        self.now, self.sleep = now, sleep
        self.reactions: list[dict] = []
        self.stalled = False

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
            elif win and not self.feed.stalled(self.STALE_POLLS):
                # Only a FULL window counts toward quiet, AND only while
                # samples are still arriving. A partial window is not evidence
                # of calm, it is absence of data -- and neither is a full one
                # that stopped being refreshed. On a dead stream the same six
                # samples are re-scored every poll and "quiet" comes back
                # forever, so the settle completes, the beat performs, and a
                # whole session reports 0 reactions indistinguishably from an
                # untouched robot.
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

    # 1.5 s of COMPLETE silence at a 0.25 s poll. At 4 Hz that is six
    # consecutive missed samples, which is not jitter. It also has to be
    # SHORTER than the shortest settle can complete in (SETTLE_QUIET_S / POLL_S
    # = 6 polls) -- set at 12, the guard could never fire during a settle,
    # because quiet was declared first and `react` had already moved on.
    STALE_POLLS = 6

    def react(self) -> dict:
        """Settle, perform, recover. Returns the record for one reaction."""
        t0 = self.now()
        settle, _ = self._watch_until(self.now() + SETTLE_MAX_S,
                                      want_quiet_s=SETTLE_QUIET_S)
        settled_s = round(self.now() - t0, 2)

        # DO NOT PERFORM INTO A DEAD STREAM. `settle` returning "quiet" on a
        # stalled feed is not a measurement -- it is the same six samples
        # re-scored every poll. Moving the robot on the strength of that is
        # acting on data that stopped arriving.
        if self.feed.stalled(self.STALE_POLLS):
            return {"settle": settle, "settle_s": settled_s,
                    "cooldown_held_s": 0.0, "beat": None, "beat_ok": False,
                    "refused": True, "stalled": True,
                    "beat_error": "sensor stream produced nothing for "
                                  f"{self.STALE_POLLS} consecutive polls; "
                                  "refusing to perform on stale data",
                    "elapsed_s": 0.0, "residual_deg": None,
                    "recover": "stalled"}

        # THROUGH THE STATUS LAYER, not straight to perform(). The beat's
        # rest colour comes from whatever status R2 is in, and the status is
        # re-asserted afterwards. Called directly, a beat run while status was
        # `attention` painted blue over a pending issue and the issue stopped
        # being pending.
        #
        # Curiosity is NOT a status and this does not set one: it is an
        # expression, it is not among the ten states, and
        # docs/behaviour-states.md is explicit that expression passes through
        # and hands the status back.
        result = self.status.express(self.beat_factory,
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
        cooldown_s = result.get("cooldown_s") or 0.0
        if cooldown_s:
            held = max(0.0, cooldown_s - (self.now() - beat_ended))
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
        """Arm and stay armed until one of the two bounds is hit.

        `max_s` bounds when the loop stops LISTENING, not when it returns. A
        reaction that starts at 179.9 s still runs to completion, so the wall
        clock can overrun by one full cycle (settle + beat + recover +
        cooldown, worst case ~54 s). That is deliberate: cutting a beat off
        mid-gesture would leave R2 mid-expression, which is a worse state
        than a session that runs long. The overrun is bounded, and the
        cooldown is the last thing in it, so nothing is moving during it.
        """
        # CYAN, front and back: the `listen` row of docs/behaviour-states.md.
        # This armed with BASE_NEUTRAL blue until that table landed and made
        # blue steady mean *idle, nothing engaged* -- the exact opposite of
        # what an armed loop is doing, and unreadable against it from across
        # the room. The operator's hand is the input device; they have to be
        # able to tell "listening" from "not" without walking over.
        #
        # It must also be a TRANSITION, not a colour (D-014). The first live
        # run armed him blue while he was ALREADY blue -- the previous
        # session's teardown had left him there -- so the one signal saying
        # "now" was indistinguishable from the state before it, and the
        # operator waited out a whole window for a cue that had already
        # happened. Dark through the hands-off phases is what makes this edge
        # visible; neither half works alone.
        self.bridge.send_batch([Step("leds", {"channels": {
            **B.front(B.BASE_ENGAGED), **B.back(B.BASE_ENGAGED)}})])
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
            if self.feed.stalled(self.STALE_POLLS):
                self.stalled = True
                break
            if outcome != "fired":
                break
            rec = self.react()
            rec["trigger_channel"] = chan
            rec["at_s"] = round(max_s - (end - self.now()), 2)
            self.reactions.append(rec)
        # DISARM IS AN EDGE TOO -- and now it is one for free. `_tidy` ends on
        # BASE_NEUTRAL, so the exit reads cyan -> blue: `listen` -> `idle`,
        # two different hues and two rows of the same table. This used to
        # paint an explicit dark frame here because armed and session-over
        # were both blue and nothing else separated them; that frame is now a
        # flicker between two states that already differ, so it is gone.
        return {"reactions": self.reactions,
                "count": len(self.reactions),
                "stalled": self.stalled,
                "stopped": "stalled" if self.stalled else
                           ("max_reactions" if len(self.reactions)
                            >= max_reactions else "time"),
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


def _tidy(bridge, status=None) -> None:
    """Put R2 back: stream off, a defined colour on. Never raises.

    Shared by both sessions because it is the one thing that must happen on
    every exit path, and two copies of it is two chances to fix only one.
    The disable is attempted even when the enable was never confirmed -- a
    lost reply is not evidence the packet missed.

    THE COLOUR COMES FROM THE STATUS LAYER when there is one. This painted
    BASE_NEUTRAL unconditionally, so a session that raised `attention` wiped
    it on the way out -- the pending issue stopped being pending the moment
    the session ended, and the next connect restored idle because that is what
    the teardown had written. "Leave him in a defined state" was satisfied and
    the state was the wrong one.

    Falls back to blue when there is no status layer: a defined colour beats
    an inherited one, and the monitor path has no status of its own.
    """
    try:
        bridge.send_batch([
            # STOP FIRST. "Default to STOP. Disconnect or failure must result
            # in stop, not last-command." If a session dies between a dome
            # step and its settle, nothing else here halts him -- turning the
            # sensor stream off and setting a colour are not stopping. `stop`
            # is permitted at every tier precisely so this is always available.
            Step("stop", {}),
            Step("sensors", {"enable": False}),
            Step("leds", {"channels":
                          status.assertion_channels() if status is not None
                          else B.front(B.BASE_NEUTRAL)}),
        ], timeout=15)
    except BaseException as e:
        # BaseException, not Exception: a Ctrl-C landing inside the teardown
        # is a plausible operator reflex on a session that runs long, and it
        # must not escape a `finally` and skip the log write below it.
        # r2_probe.py:1697 makes the same choice for the same reason.
        print(f"!! could not tidy up: {type(e).__name__}: {e}")


def monitor_session(args, *, now=time.monotonic, sleep=time.sleep) -> int:
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

    log = {"started": started, "mode": "monitor",
           "config": {"window_s": args.window, "baseline_s": args.baseline,
                      "monitor_s": args.max_s}}
    try:
        # Inside the try for the same reason as run_session: an early return
        # must not skip the teardown that turns the stream off and puts a
        # defined colour back.
        bridge.send_batch([Step("leds", {"channels": B.front((0, 0, 0))})],
                          timeout=15)
        r = bridge.send_batch([Step("sensors", {"enable": True,
                                                "groups": GROUPS,
                                                "ext_groups": EXT_GROUPS})],
                              timeout=15)[0]
        log["stream_enabled"] = r.get("ok")
        if not r.get("ok"):
            print(f"could not enable the sensor stream: {r}")
            return 3

        print(f"HANDS OFF. Baseline for {args.baseline:.0f}s...")
        cal = calibrate(bridge, feed, args.baseline, window_n,
                        now=now, sleep=sleep)
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
        end = now() + args.max_s
        while now() < end:
            feed.drain()
            win = feed.window(window_n)
            if win:
                rs = ratios(win, th)
                top = sorted(rs.items(), key=lambda kv: -kv[1])[:3]
                track.append({
                    "t": round(args.max_s - (end - now()), 2),
                    "over": sum(1 for v in rs.values() if v >= 1.0),
                    "top": [(k, round(v, 3)) for k, v in top]})
            sleep(POLL_S)

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
        _tidy(bridge)
        log["ended"] = time.time()
        try:
            LOG_DIR.mkdir(parents=True, exist_ok=True)
            log_file = LOG_DIR / f"s2b-monitor-{int(started)}.json"
            log_file.write_text(json.dumps(log, indent=2))
            print(f"log: {log_file.name}")
        except (OSError, TypeError, ValueError) as e:
            print(f"!! could not write the run log: {type(e).__name__}: {e}")


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

    # EVERYTHING that changes the robot lives inside the try, so the teardown
    # in `finally` covers it. Both of these used to sit above it, and both
    # leaked on the one path that skipped the teardown:
    #
    #   - the dark LEDs are a TRANSIENT, not a state (D-014 says dark is the
    #     absence of a colour, used to make the next one legible). Returning
    #     early left R2 parked in it, and whatever a session leaves lit is
    #     what the household sees until something changes it.
    #   - `ok: False` includes "no response", and a lost reply is NOT evidence
    #     the packet missed. An unconfirmed enable may well have enabled the
    #     stream, so the disable has to be attempted regardless. This is the
    #     same attempted-enable-sets rule already fixed one layer down in
    #     r2_probe.py, reintroduced here by putting the call in the wrong
    #     block.
    # CHECK THE CEILING NOW, not 50 s from now. `perform` refuses on tier and
    # says so clearly -- but only after calibration and the control have both
    # run, so a daemon started at the wrong --allow costs the operator the
    # full hands-off stretch before telling them. Worse, at `read` the LED
    # writes are refused too, so D-014's arming cue silently does not happen:
    # the exact failure D-014 exists to eliminate.
    need = B.express_curious().required_tier()
    if B.tier_rank(need) > B.tier_rank(ceiling):
        print(f"daemon ceiling is {ceiling!r} but the behaviour needs "
              f"{need!r}. Relaunch:\n"
              f"  cd ~/Projects/R2Z2/mac-prototype && ./r2 daemon "
              f"--allow {need}")
        return 6

    # AFTER the ceiling refusal, deliberately. Asserting a status writes LEDs
    # and the chirp announces "I am back" -- doing that for a session about to
    # abort spends the operator's droid on an event that never happens, and it
    # broke the invariant that a refused session moves nothing at all.
    #
    # MEASURED 2026-08-18: a colour we set survives a link drop and a fresh
    # connect but NOT a sleep cycle, so a session that skips this inherits
    # whatever the firmware reverted to -- R2's own red/blue alternation,
    # every morning, whatever the light language says.
    #
    # `dropped` is a claim the store held that could not be restored (a stale
    # danger, an interaction state whose conversation is long over). Printed
    # rather than swallowed: silently clearing a pending claim and silently
    # re-asserting a stale one are both wrong, and only the operator can tell
    # which happened.
    # Constructed OUTSIDE the try so the `finally` can always reach it, but
    # CONNECTED inside it. connect() writes LEDs and sends a chirp, and
    # FileBridge.send_batch raises on failure -- outside the try that raise
    # escaped run_session with no teardown and no run log. TestTeardownAlwaysRuns
    # exists because "the setup used to sit OUTSIDE the try, so the one path
    # that returned early skipped the teardown entirely"; this was that shape
    # again.
    status = StatusLayer(
        bridge, StatusStore(STATUS_STORE) if STATUS_STORE else None)

    loop = None
    try:
        asserted, dropped = status.connect()
        log["status"] = {"asserted": asserted, "dropped": dropped}
        print(f"status: asserted {asserted!r}" +
              (f"; DROPPED {dropped!r} (not restorable across a session — "
               f"re-derive it if it still holds)" if dropped else ""))
        bridge.send_batch([Step("leds", {"channels": B.front((0, 0, 0))})],
                          timeout=15)
        r = bridge.send_batch([Step("sensors", {"enable": True,
                                                "groups": GROUPS,
                                                "ext_groups": EXT_GROUPS})],
                              timeout=15)[0]
        log["stream_enabled"] = r.get("ok")
        if not r.get("ok"):
            print(f"could not enable the sensor stream: {r}")
            return 3

        window_n = window_n_for(STREAM_HZ, args.window)
        print(f"HANDS OFF. Calibrating rest for {args.baseline:.0f}s "
              f"(window {window_n} samples)...")
        cal = calibrate(bridge, feed, args.baseline, window_n,
                        now=now, sleep=sleep)

        # RE-DERIVE FROM WHAT THE STREAM ACTUALLY DID. `STREAM_HZ` is a
        # measured constant, not a guarantee: sensor_probe.py records a
        # session that ran at 7.3 Hz. If the rate differs, `--window 1.5` is
        # not 1.5 seconds, and the control-failure hint telling the operator
        # to "retry at --window 3.0" is advice in units the code ignores.
        measured_n = window_n_for(cal["rate_hz"], args.window)
        thresholds = cal["_thresholds"]
        if measured_n != window_n:
            print(f"  note: stream measured {cal['rate_hz']} Hz, not "
                  f"{STREAM_HZ}; window is {measured_n} samples, not "
                  f"{window_n}. Re-deriving thresholds at the real rate.")
            window_n = measured_n
            thresholds = empirical_thresholds(cal["series"], window_n)
        log["calibration"] = {k: v for k, v in cal.items()
                              if k not in ("_thresholds", "series")}
        log["calibration"]["series"] = cal["series"]
        log["calibration"]["window_n"] = window_n
        print(f"  {cal['samples']} samples at {cal['rate_hz']} Hz, "
              f"{len(thresholds)} channels have limits")
        if len(thresholds) < 2:
            print("REFUSING to arm: fewer than two channels produced a usable "
                  "limit, so the two-channel corroboration rule can never be "
                  "met and nothing would ever fire.")
            return 4

        loop = Reactive(bridge, feed, thresholds, window_n,
                        ceiling=ceiling, status=status, now=now, sleep=sleep)

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

        print(f"\nARMED for {args.max_s:.0f}s (cyan = listening). "
              f"Pet him whenever — up to {args.max_reactions} reactions.\n")
        out = loop.run(args.max_s, args.max_reactions)
        log["run"] = out
        for i, rec in enumerate(out["reactions"], 1):
            # `elapsed_s` is absent from perform()'s refusal dict, so a
            # refused beat printed "in Nones".
            took = rec.get("elapsed_s")
            took = f"in {took}s" if took is not None else "(never ran)"
            print(f"  reaction {i} at {rec['at_s']:.0f}s: "
                  f"triggered on {rec['trigger_channel']}, "
                  f"settle={rec['settle']} ({rec['settle_s']}s), "
                  f"beat {'ok' if rec['beat_ok'] else 'FAILED'} "
                  f"{took}, recover={rec['recover']}")
        print(f"\n{out['count']} reactions, stopped on {out['stopped']}."
              f"  dropped={out['dropped_events']} "
              f"decode_errors={out['decode_errors']}")

        # CLOSING CONTROL. "Run the case that must NOT fire, BEFORE any real
        # trial -- and again at the end." The opening control proves the
        # detector was honest when it armed; only this one proves it was
        # still honest when it stopped. Thresholds are frozen from a baseline
        # taken in a pose the dome has since walked away from, so specificity
        # drift is the expected failure, not a hypothetical -- and without
        # this every reaction in the run stays unadjudicated.
        print(f"\nHANDS OFF again — closing control ({args.control:.0f}s).")
        closing = loop.control(args.control)
        log["closing_control"] = closing
        print(f"  {closing['outcome']}: {closing['why']}")
        if not closing["ok"]:
            print("!! The detector fires on nothing NOW, though it was quiet "
                  "before the run. Every reaction above is suspect: "
                  "specificity drifted somewhere inside the session.")
            return 7
        return 0
    finally:
        # Reactions already performed are evidence, and they used to be
        # discarded whenever anything raised -- the daemon going away
        # mid-session (its 900 s idle timeout, Ctrl-C in its Terminal, R2
        # carried out of range) is the ordinary case, not the exotic one, and
        # `FileBridge._send` raises RuntimeError on all of them. `loop` holds
        # them on the object, so recover them here rather than on the one
        # path that returns cleanly.
        if loop is not None and "run" not in log:
            log["run"] = {"reactions": loop.reactions,
                          "count": len(loop.reactions),
                          "incomplete": True,
                          "dropped_events": feed.dropped,
                          "decode_errors": feed.decode_errors}
        # ROBOT FIRST, disk second. The log write used to come first and was
        # unguarded inside a `finally`: a full disk or a bad permission would
        # raise there, mask whatever exception was already propagating, AND
        # skip the teardown entirely -- trading R2's physical state for a
        # logging convenience. The write is the part that is allowed to fail.
        _tidy(bridge, status)
        log["ended"] = time.time()
        try:
            LOG_DIR.mkdir(parents=True, exist_ok=True)
            log_file = log_path(started)
            log_file.write_text(json.dumps(log, indent=2))
            print(f"log: {log_file.name}")
        except (OSError, TypeError, ValueError) as e:
            print(f"!! could not write the run log: {type(e).__name__}: {e}")


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
