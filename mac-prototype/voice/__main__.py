#!/usr/bin/env python3
"""voice/__main__.py — the terminal harness for #42. No robot, no BLE.

    cd mac-prototype
    python3 -m voice --list-devices
    python3 -m voice listen  --engine openwakeword
    python3 -m voice ambient --engine openwakeword --minutes 30

`listen` is AC1: speak the phrase, watch it fire, read the timestamp, RMS and
buffer duration. `ambient` is AC3: leave it running in the room and it reports
false accepts per hour at a given sensitivity, so the sensitivity is SET from a
measurement instead of chosen in the abstract.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from capture import (  # noqa: E402
    FORMAT,
    Chunker,
    MicSource,
    RingBuffer,
    Segmenter,
    dbfs,
    rms,
    write_wav,
)
from wake import DEFAULT_SENSITIVITY, create_engine  # noqa: E402


def list_devices() -> int:
    try:
        import sounddevice
    except ImportError:
        print("sounddevice is not installed:  .venv/bin/pip install sounddevice")
        return 1
    print(sounddevice.query_devices())
    return 0


def cmd_listen(args: argparse.Namespace) -> int:
    engine = create_engine(
        args.engine, keywords=args.keyword or None, sensitivity=args.sensitivity
    )
    seg = Segmenter(threshold=args.threshold, silence_ms=args.silence_ms)
    ring = RingBuffer(FORMAT.ms_to_bytes(args.preroll_ms))
    chunk = Chunker(engine.frame_bytes)

    print(
        f"engine={engine.name} frame={engine.frame_samples} samples "
        f"({FORMAT.bytes_to_ms(engine.frame_bytes):.0f} ms) "
        f"sensitivity={args.sensitivity} threshold={args.threshold}"
    )
    print(f"format={FORMAT.rate_hz} Hz / {FORMAT.width_bytes * 8}-bit / "
          f"{FORMAT.channels}ch   (ctrl-C to stop)")

    detections = 0
    with engine, MicSource(device=args.device) as mic:
        for block in mic.blocks():
            for frame in chunk.feed(block):
                if seg.active:
                    utt = seg.feed(frame)
                    if utt is None:
                        continue
                    detections += 1
                    print(
                        f"  utterance  {utt.duration_s:6.2f} s  "
                        f"peak {dbfs(utt.peak_level):6.1f} dBFS  "
                        f"end={utt.reason}"
                    )
                    if args.save:
                        out = write_wav(
                            Path(args.save) / f"utterance-{detections:03d}.wav", utt.pcm
                        )
                        print(f"             -> {out}")
                    continue

                ring.write(frame)
                event = engine.process(frame)
                if event is not None:
                    stamp = time.strftime("%H:%M:%S", time.localtime(event.at))
                    print(
                        f"[{stamp}] WAKE  {event.keyword!r} score={event.score:.3f}  "
                        f"level={dbfs(rms(frame)):6.1f} dBFS  "
                        f"preroll={FORMAT.bytes_to_ms(len(ring)):.0f} ms"
                    )
                    seg.begin(preroll=ring.read(), now=event.at)
                    ring.clear()
    return 0


def cmd_ambient(args: argparse.Namespace) -> int:
    """AC3. Nobody says the phrase; every fire is a false accept."""
    engine = create_engine(
        args.engine, keywords=args.keyword or None, sensitivity=args.sensitivity
    )
    chunk = Chunker(engine.frame_bytes)
    deadline = time.time() + args.minutes * 60
    fires: list[float] = []
    peak = 0.0
    started = time.time()

    print(
        f"ambient sample: {args.minutes} min, engine={engine.name}, "
        f"sensitivity={args.sensitivity}. Do NOT say the phrase."
    )
    try:
        with engine, MicSource(device=args.device) as mic:
            for block in mic.blocks():
                for frame in chunk.feed(block):
                    peak = max(peak, rms(frame))
                    event = engine.process(frame)
                    if event is not None:
                        fires.append(event.at)
                        print(
                            f"  false accept #{len(fires)} at "
                            f"{time.strftime('%H:%M:%S')} score={event.score:.3f}"
                        )
                if time.time() >= deadline:
                    break
    except KeyboardInterrupt:
        print("\n  interrupted — reporting on the partial sample")

    elapsed_h = (time.time() - started) / 3600.0
    print(
        f"\nRESULT  engine={engine.name} sensitivity={args.sensitivity}\n"
        f"  duration      {elapsed_h * 60:.1f} min\n"
        f"  false accepts {len(fires)}  ({len(fires) / max(elapsed_h, 1e-9):.2f} / hour)\n"
        f"  ambient peak  {dbfs(peak):.1f} dBFS\n"
        f"Record this in #42 AC3 together with the sensitivity above."
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python3 -m voice", description=__doc__)
    p.add_argument("--list-devices", action="store_true")
    sub = p.add_subparsers(dest="cmd")

    def common(sp):
        # Defaults come from .env.example's WAKE_ENGINE / WAKE_KEYWORD so the
        # engine choice lives in config rather than in argv.
        sp.add_argument("--engine",
                        default=os.environ.get("WAKE_ENGINE", "openwakeword"),
                        choices=["openwakeword", "porcupine", "scripted"])
        sp.add_argument("--keyword", action="append",
                        default=[k for k in [os.environ.get("WAKE_KEYWORD")] if k])
        sp.add_argument("--sensitivity", type=float, default=DEFAULT_SENSITIVITY)
        sp.add_argument("--device", default=None)
        return sp

    lis = common(sub.add_parser("listen", help="AC1 — detect and segment"))
    lis.add_argument("--threshold", type=float, default=Segmenter.DEFAULT_THRESHOLD)
    lis.add_argument("--silence-ms", type=int, dest="silence_ms",
                     default=Segmenter.DEFAULT_SILENCE_MS)
    lis.add_argument("--preroll-ms", type=int, dest="preroll_ms",
                     default=Segmenter.DEFAULT_PREROLL_MS)
    lis.add_argument("--save", default=None, metavar="DIR",
                     help="write each utterance to a .wav so it can be listened to")
    lis.set_defaults(func=cmd_listen)

    amb = common(sub.add_parser("ambient", help="AC3 — false accepts over N minutes"))
    amb.add_argument("--minutes", type=float, default=30.0)
    amb.set_defaults(func=cmd_ambient)

    args = p.parse_args(argv)
    if args.list_devices:
        return list_devices()
    if not getattr(args, "func", None):
        p.print_help()
        return 2
    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("\nstopped")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
