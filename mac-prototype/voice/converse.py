#!/usr/bin/env python3
"""voice/converse.py — the whole loop, on the Mac, with R2 powered off.

    wake -> capture -> transcribe -> reason -> speak
     V1       V1          V4          V5       V6

The first time these five stages run as one thing. R2's own half of the
reply -- the chirp and the dome turn -- is CHOSEN here and printed, but not
sent: that needs the BLE daemon and belongs to V2/V3 (#43, #44). What you can
actually hear tonight is Threepio.

BOUNDED BY DEFAULT, DELIBERATELY
    `--turns` defaults to 3 and the process exits when they are spent. An
    unbounded listen-and-speak loop is a thing that talks in someone's house
    until they find a way to kill it, and this session already produced one of
    those by accident with a 15-file `afplay` loop that had no stop. A
    conversational agent with a microphone and a speaker deserves a lower
    ceiling than a shell loop, not a higher one.

QUIET HOURS MUST BE OVERRIDDEN EXPLICITLY
    `speak()` refuses between 22:00 and 08:00. This script does not weaken
    that default; it makes you pass `--audition` to get past it, and says so
    on every run.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import capture as C
import reason as RSN
import speak as SPK
import transcribe as TR
import wake as WK
import r2_behavior as B

DEFAULT_MODEL = str(Path(__file__).resolve().parent / "models" / "r2d2.onnx")

# MEASURED in #12 (S1d), all 56 authored ids: 36 emit WADDLE and can fell him,
# 14 show no leg activity at all. THIS LIST IS THE SAFETY BOUNDARY and nothing
# outside it may be sent from here.
#
# Note what the list is keyed on. NOT the end stance -- EMOTE_YES ends in
# bipod and R2 stood in bipod indefinitely afterwards, unharmed. It is WADDLE
# *during* the animation that topples him. A classifier keying on final stance
# would pass a waddle-in-the-middle animation as safe.
#
# And note what deploying the tripod first does NOT do: the animation drives
# the legs itself and retracts the stabiliser, so a pre-set stance is not a
# mitigation. `get_leg_action` cannot sense stance either -- it reports tracked
# state a reconnect wipes. The measured list is the only real guard.
SAFE_ANIMATIONS = (0, 1, 6, 16, 17, 23, 25, 26, 27, 34, 44, 47, 52, 55)

# `no_leg_activity` does not mean motionless: the DOME still moves, which is
# the visible motion, without the failure mode.
# What whisper actually renders "R2-D2" as, OBSERVED across live turns:
#   "2D2."  "2d2."  "to do too."  "To detour"
# The wake word lands in the transcript because detection fires AFTER the
# phrase completes, so a pre-roll buffer holds the phrase itself. The primary
# fix is to stop feeding it to whisper (see --preroll-ms); this is the safety
# net for what still leaks, and it only ever strips at the START.
#
# Note "to do too" and "to detour": the mangling is not close to the spelling,
# so matching on "r2d2" alone would have caught two of four cases and looked
# like it worked.
WAKE_VARIANTS = re.compile(
    r"""^[\s\W]*(
          r?\s*2\s*[-,. ]*\s*d\s*2
        | r\s*two\s*d\s*two
        | artoo\s*[- ]?\s*detoo
        | to\s+do\s+too
        | to\s+detour
        | are\s*two\s*dee\s*two
    )[\s\W]*""", re.I | re.X)


def strip_wake(text: str) -> tuple[str, str]:
    """Remove a leading wake-phrase rendering. Returns (cleaned, removed)."""
    m = WAKE_VARIANTS.match(text)
    if not m:
        return text, ""
    return text[m.end():].strip(), text[:m.end()].strip()


WAKE_ANIM = 0        # ~1.5-1.9 s, light flourish, no leg motion (3 fires)
REPLY_ANIM = 1       # 5.1 s, makes sound, interruptible (AC6 measured)


def send_animation(bridge, anim_id: int, ceiling: str) -> str:
    """Play one authored animation. Refuses anything off the measured list.

    Deliberately NOT routed through `Beat`. `r2_behavior.FORBIDDEN_OPS` makes
    `animation` unrepresentable there, and that guarantee is worth keeping
    exactly as it is — the choreography layer should stay unable to fell him.
    This is a separate, narrower channel with its own allow-list, used only by
    this supervised test harness.
    """
    if anim_id not in SAFE_ANIMATIONS:
        return (f"REFUSED animation {anim_id}: not in the measured "
                f"no_leg_activity set {SAFE_ANIMATIONS}")
    if B.tier_rank(ceiling) < B.tier_rank("stance"):
        return (f"held back: animations need the 'stance' ceiling, "
                f"daemon permits {ceiling!r}")
    # Liveness only where the bridge can answer it. FakeBridge cannot, and a
    # test double should not be the reason a safety path raises.
    if hasattr(bridge, "daemon") and bridge.daemon() is None:
        return "DAEMON GONE — relaunch start-daemon-stance.command"
    try:
        out = bridge._send((B.Step("animation", {"id": anim_id}),), 30.0)
        ok = all(r.get("ok") for r in out)
        return f"animation {anim_id} {'played' if ok else 'FAILED'}: {out}"
    except Exception as e:
        return f"animation {anim_id} failed: {type(e).__name__}: {e}"


def resolve_device(substr: str | None):
    """By NAME. A device appearing mid-session shifts indices, and that is how
    a survey run got pointed at a microphone that returned digital silence."""
    import sounddevice as sd
    if not substr:
        return None, sd.query_devices(kind="input")["name"]
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0 and substr.lower() in d["name"].lower():
            return i, d["name"]
    raise SystemExit(f"no input device matching {substr!r}")


def prove_mic_live_and_calibrate(mic) -> float:
    """Prove the channel carries samples, and measure the ROOM'S OWN SILENCE.

    Two jobs on purpose, because they need the same recording: the half second
    before anyone has spoken. A held-open microphone returns digital silence,
    indistinguishable from nobody speaking; and the speech threshold has to be
    derived from a real noise floor, since the floor moves by 20 dB between
    microphones (MEASURED: MacBook mic -68.6 dBFS, BRIO -47.6 dBFS).

    The first version of this loop calibrated from the wake-word PRE-ROLL
    instead -- i.e. from a buffer full of the operator's voice. That set the
    threshold above their speech, so every following frame read as silence and
    every utterance was cut off after the hangover window. Three turns in a
    row produced 1.7 s of audio and a plausible reply, which is exactly how a
    broken thing passes for a working one.
    """
    # Sample 2 s in 100 ms windows and take a LOW PERCENTILE, not the mean of
    # one window. OBSERVED: a single 500 ms sample taken while R2 was still
    # finishing an animation measured the floor at -39 dBFS instead of ~-65,
    # putting the speech threshold at -27 — on top of the operator's own voice,
    # which would have clipped every utterance for the whole session. One
    # transient must not define the room.
    windows, probe = [], bytearray()
    win = C.FORMAT.ms_to_bytes(100)
    for b in mic.blocks():
        probe.extend(b)
        while len(probe) >= win:
            windows.append(C.rms(bytes(probe[:win])))
            del probe[:win]
        if len(windows) >= 20:
            break
    if not any(w > 0 for w in windows):
        raise SystemExit(
            "ABORT: the microphone returned 0 non-zero samples in 2 s.\n"
            "       The channel is dead, not quiet — something else holds it.")
    quiet = sorted(windows)
    floor = quiet[max(0, int(len(quiet) * 0.2))]      # 20th percentile
    threshold = C.Segmenter.threshold_for(floor)
    print(f"  mic live — floor {C.dbfs(floor):.1f} dBFS "
          f"(p20 of 20 windows; loudest {C.dbfs(quiet[-1]):.1f}), "
          f"speech threshold {C.dbfs(threshold):.1f} dBFS")
    if threshold > 0.02:      # ~-34 dBFS
        print(f"  [warn] that threshold is high — the room was noisy while "
              f"calibrating. If utterances get clipped, re-run somewhere "
              f"quieter or pass --silence-ms higher.")
    return threshold


def open_bridge(requested: str, bridge_dir: Path | None = None):
    """Return (bridge, ceiling) for sending R2's half, or (None, reason).

    The ceiling comes from the DAEMON'S OWN LOCK, not from our flag. The
    daemon records what it will actually permit (r2_probe.py:921), and a
    command-line flag is a statement of intent, not of fact — asking for
    `dome` against a daemon launched at `audio` would fail one op at a time,
    mid-conversation. Clamp up front and say so.

    Also: the queue directories SURVIVE the daemon that made them, so their
    existence is not liveness. `daemon()` checks the recorded pid.
    """
    if requested == "none":
        return None, "not sending (default)"
    # The bridge dir is EXPLICIT. FileBridge defaults it relative to its own
    # __file__, which is correct in a normal checkout and silently wrong when
    # this runs from a git worktree: the daemon writes to the main tree's
    # .bridge and the worktree copy looks at an empty one, reporting "no
    # daemon" while a daemon is plainly running. Cost one live run.
    bridge = B.FileBridge(bridge_dir) if bridge_dir else B.FileBridge()
    held = bridge.daemon()
    if held is None:
        return None, ("no daemon holds the bridge — launch "
                      "mac-prototype/start-daemon.command from Finder "
                      "(the agent cannot: macOS gives Bluetooth to the "
                      "responsible process)")
    allowed = held.get("ceiling", "read")
    if B.tier_rank(requested) > B.tier_rank(allowed):
        return bridge, (allowed, f"asked for {requested!r}, daemon permits "
                                 f"{allowed!r} — using {allowed!r}")
    return bridge, (requested, f"sending at {requested!r} "
                               f"(daemon ceiling {allowed!r})")


def send_r2(behaviour, bridge, ceiling) -> str:
    """R2's half. Never raises into the conversation loop.

    A beat above the ceiling is DEGRADED to what the ceiling allows, not
    dropped. `reason()` nearly always returns a dome angle, so rejecting the
    whole beat would make `--send audio` silent in practice — and a bring-up
    step that does nothing teaches nothing. Dropping the dome step is safe in
    the direction that matters: it removes motion, never adds it.
    """
    try:
        beat = behaviour.to_beat()
        if (B.tier_rank(beat.required_tier()) > B.tier_rank(ceiling)
                and behaviour.dome_deg):
            import dataclasses
            behaviour = dataclasses.replace(behaviour, dome_deg=0.0)
            beat = behaviour.to_beat()
        if B.tier_rank(beat.required_tier()) > B.tier_rank(ceiling):
            return (f"held back: beat needs {beat.required_tier()!r}, "
                    f"ceiling is {ceiling!r}")
        # Same guard as send_animation: liveness only where the bridge can
        # answer it. Guarding one send path and not the other is how a test
        # double crashes production code.
        if hasattr(bridge, "daemon") and bridge.daemon() is None:
            return ("DAEMON GONE — the link dropped since this run started; "
                    "relaunch start-daemon.command")
        rec = B.perform(beat, bridge, ceiling=ceiling)
        bad = [r for r in rec.get("responses", []) if not r.get("ok")]
        return f"sent {beat.name} ({len(rec.get('responses', []))} ops)" + \
               (f", {len(bad)} FAILED" if bad else "")
    except Exception as e:
        return f"send failed: {type(e).__name__}: {e}"


def one_turn(mic, engine, chunker, ring, transcriber, speaker, args, n,
             threshold, bridge, ceiling) -> bool:
    """Wait for the wake word, then run the loop once. False to stop."""
    seg = None
    print(f"\n[{n}] listening for the wake word…", flush=True)
    for block in mic.blocks():
        for frame in chunker.feed(block):
            if seg is not None and seg.active:
                utt = seg.feed(frame)
                if utt is None:
                    continue
                return handle(utt, transcriber, speaker, args, bridge, ceiling)
            ring.write(frame)
            if engine.process(frame) is not None:
                print(f"    wake  ({time.strftime('%H:%M:%S')}) — listening…",
                      flush=True)
                if bridge is not None and args.animate:
                    import threading
                    threading.Thread(
                        target=lambda: print(
                            f"    wake  {send_animation(bridge, args.wake_anim, ceiling)}",
                            flush=True),
                        daemon=True).start()
                # Threshold comes from the ROOM measured before anyone
                # spoke, never from the pre-roll — see
                # prove_mic_live_and_calibrate().
                seg = C.Segmenter(threshold=threshold,
                                  silence_ms=args.silence_ms,
                                  max_ms=15_000)
                # Keep only the LAST few hundred ms of pre-roll. The full
                # buffer holds the wake phrase, because detection fires after
                # the phrase finishes — so a generous pre-roll guarantees the
                # wake word reaches the transcriber. A short one still catches
                # the onset of a word spoken immediately afterwards.
                keep = C.FORMAT.ms_to_bytes(args.preroll_ms)
                seg.begin(preroll=ring.read()[-keep:] if keep else b"")
                ring.clear()
    return False


def handle(utt, transcriber, speaker, args, bridge, ceiling) -> bool:
    t0 = time.perf_counter()
    flag = "  <-- SUSPICIOUSLY SHORT" if utt.duration_s < 2.0 else ""
    print(f"    heard {utt.duration_s:.1f}s (ended: {utt.reason})"
          f"  peak {C.dbfs(utt.peak_level):.0f} dBFS{flag}", flush=True)

    tx = transcriber.transcribe(utt.pcm)
    print(f'    text  "{tx.text}"   [{tx.elapsed_s:.2f}s]', flush=True)
    # whisper.cpp returns bracketed markers for non-speech — [BLANK_AUDIO],
    # [SILENCE], [ Silence ]. They are not words. Two turns tonight fed
    # "[BLANK_AUDIO]" into the reasoning layer, and Threepio dutifully
    # composed a reply ABOUT the marker, which reads as a working
    # conversation right up until you notice what he is discussing.
    cleaned = re.sub(r"\[[^\]]*\]", "", tx.text).strip()
    cleaned, removed = strip_wake(cleaned)
    if removed:
        print(f"    strip \"{removed}\" (wake phrase, not speech)", flush=True)
    if not cleaned:
        print(f"    (nothing intelligible{' — ' + tx.text.strip() if tx.text.strip() else ''}"
              f", peak {C.dbfs(utt.peak_level):.0f} dBFS — skipping)")
        return True
    tx = TR.Transcript(cleaned, tx.engine, tx.elapsed_s, tx.audio_s)

    # The two model calls take the same input and neither reads the other's
    # output, so serialising them just added their latencies together. R2 is
    # lit and waiting for the whole of it.
    from concurrent.futures import ThreadPoolExecutor
    pool = ThreadPoolExecutor(max_workers=3)
    r2_future = None
    line_future = pool.submit(SPK.compose_line, tx.text)

    try:
        behaviour = RSN.reason(tx.text, timeout=args.timeout)
        print(f"    R2    {behaviour.mood}: sound "
              f"{behaviour.sound_id} ({RSN._name_of(behaviour.sound_id)}), "
              f"dome {behaviour.dome_deg:+.0f}°   [{behaviour.latency_s:.2f}s]"
              f"{'' if bridge is not None else '   (not sent)'}", flush=True)
        for r in behaviour.rejections:
            print(f"    !     {r}")
        # R2 chirps and turns WHILE Threepio talks. They are two characters in
        # one room, not a queue — and serialising them would add the dome's
        # ~2.2 s fixed move time to every exchange.
        if bridge is None:
            r2_future = None
        elif args.animate:
            # "animation OR hand-composed dome+sound+light, not both at once"
            # (r2-capabilities.md). In animate mode the authored animation
            # REPLACES the composed beat rather than stacking on it.
            r2_future = pool.submit(send_animation, bridge, args.reply_anim,
                                    ceiling)
        else:
            r2_future = pool.submit(send_r2, behaviour, bridge, ceiling)
    except RSN.ReasonError as e:
        print(f"    R2    reasoning failed: {e}")
        print(f"          -> {RSN.error_beat(str(e)).name}, back to neutral")
        line_future.cancel()
        pool.shutdown(wait=False)
        return True

    try:
        line = line_future.result(timeout=args.timeout * 2)
        print(f'    C3PO  "{line}"', flush=True)
        gate = SPK.QuietHours(enabled=not args.audition)
        SPK.speak(line, speaker=speaker, quiet_hours=gate, play_audio=True)
    except SPK.QuietHoursError as e:
        print(f"    C3PO  (silent) {e}")
    except SPK.SpeakError as e:
        print(f"    C3PO  speech failed: {e}")
        print(f"          -> {SPK.error_beat(str(e)).name}, back to neutral")
    finally:
        # ALWAYS report what happened to R2's half. This print used to sit
        # inside `except SpeakError`, so it only fired when speech failed —
        # every successful turn ran the send, captured its result string, and
        # threw it away. Four turns reported nothing and looked fine.
        if r2_future is not None:
            try:
                print(f"    R2    {r2_future.result(timeout=30)}", flush=True)
            except Exception as e:
                print(f"    R2    send result unavailable: "
                      f"{type(e).__name__}: {e}", flush=True)
        pool.shutdown(wait=False)
    print(f"    total {time.perf_counter() - t0:.1f}s "
          f"(includes playing his reply)", flush=True)
    return True


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python3 voice/converse.py",
                                description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--turns", type=int, default=3,
                   help="how many exchanges before exiting (default 3). "
                        "Bounded on purpose.")
    p.add_argument("--device", default="BRIO", help="input device name substring")
    p.add_argument("--keyword", default=DEFAULT_MODEL)
    p.add_argument("--sensitivity", type=float, default=0.5)
    p.add_argument("--timeout", type=float, default=RSN.DEFAULT_TIMEOUT_S)
    p.add_argument("--animate", action="store_true",
                   help="play authored animations instead of composed beats. "
                        "Needs the 'stance' ceiling. Only ids measured as "
                        "no_leg_activity in #12 are ever sent.")
    p.add_argument("--wake-anim", type=int, dest="wake_anim", default=WAKE_ANIM)
    p.add_argument("--reply-anim", type=int, dest="reply_anim", default=REPLY_ANIM)
    p.add_argument("--bridge-dir", default=None, type=Path,
                   help="the daemon's .bridge directory. Needed when this runs "
                        "from a worktree, where the default resolves to the "
                        "wrong tree.")
    p.add_argument("--send",
                   choices=["none", "leds", "audio", "dome", "stance"],
                   default="none",
                   help="actually drive R2. Default none. Each actuator is "
                        "opt-in and never bundled — bring-up order is "
                        "read -> leds -> audio -> dome (CLAUDE.md).")
    p.add_argument("--preroll-ms", type=int, dest="preroll_ms", default=200,
                   help="pre-roll kept ahead of the wake instant. 700 fed the "
                        "wake phrase itself to the transcriber every turn.")
    p.add_argument("--silence-ms", type=int, dest="silence_ms", default=1400,
                   help="hangover before an utterance is considered finished. "
                        "900 cut people off at a thinking pause; 1400 does not.")
    p.add_argument("--audition", action="store_true",
                   help="speak even during quiet hours (22:00-08:00)")
    a = p.parse_args(argv)

    idx, name = resolve_device(a.device)
    engine = WK.create_engine("openwakeword", keywords=[a.keyword],
                              sensitivity=a.sensitivity)
    transcriber = TR.create_transcriber("whisper.cpp", warm=True)
    speaker = SPK.create_speaker()

    gate = SPK.QuietHours()
    print(f"device     : {name}")
    print(f"wake model : {Path(a.keyword).name}")
    print(f"transcribe : {transcriber.backend_report()}")
    print(f"voice      : {getattr(speaker, 'voice', '?')}")
    print(f"turns      : {a.turns}  (exits after these — ctrl-C also works)")
    if gate.active() and not a.audition:
        print(f"QUIET HOURS active ({gate.start:%H:%M}-{gate.end:%H:%M}): "
              f"Threepio will stay SILENT. Pass --audition to override.")
    bridge, info = open_bridge(a.send, a.bridge_dir)
    if bridge is None:
        print(f"R2 body    : {info}")
        ceiling = None
    else:
        ceiling, msg = info
        print(f"R2 body    : {msg}")
    with C.MicSource(device=idx) as mic:
        threshold = prove_mic_live_and_calibrate(mic)
        chunker = C.Chunker(engine.frame_bytes)
        ring = C.RingBuffer(C.FORMAT.ms_to_bytes(700))
        for n in range(1, a.turns + 1):
            if not one_turn(mic, engine, chunker, ring, transcriber,
                            speaker, a, n, threshold, bridge, ceiling):
                break
    print(f"\ndone — {a.turns} turn(s) spent, exiting.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nstopped")
