#!/usr/bin/env python3
"""E2E v0 step 3.4a -- the chirp survey. The operator is the instrument.

Ten candidate sound ids, two per mood, each rated by ear:
    y = reads as that mood,  n = doesn't,  m = missed it.
Gate (plan 3.4a): every mood keeps at least one id rated `y`. A mood with none
is DROPPED from the board's set rather than given an unheard sound.

One phase per invocation, because "go" arrives in chat:
    chirp_survey.py batch A      # items 1-5, ~8 s apart
    chirp_survey.py batch B      # items 6-10
    chirp_survey.py replay 7     # one item again
    chirp_survey.py record "1y 2n 3m ..."
    chirp_survey.py verdict      # the table the board would ship
    chirp_survey.py selftest     # verdict logic on synthetic ratings, no robot

Every send's raw daemon response is appended to the results file BEFORE
anything is printed, and stderr is never suppressed: a silent send failure
reads exactly like a quiet robot.

Candidates are chosen from S1b (Sound Survey): CHATTY_11 ("inquisitive") and
CHATTY_15 ('"huh?"') are the only ids already HEARD as curious, so item 1 is a
known positive -- if the operator cannot hear it, nothing after it counts.
POSITIVE, NEGATIVE, SAD and ALARM have no S1b reading at all.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent          # mac-prototype/
sys.path.insert(0, str(HERE))
from r2_assets import R2_SOUNDS                        # noqa: E402

RESULTS = HERE / "results" / "chirp_survey_3.4a.jsonl"
VOLUME = 200          # S1b: 80 was too quiet to evaluate
SPACING_S = 8.0       # S1b's spacing; long enough to count along
MOODS = ("curious", "happy", "annoyed", "sad", "alert")

# (item, mood, sound name). Moods interleaved so no two neighbours share one.
ITEMS = [
    (1,  "curious", "R2_CHATTY_11"),   # KNOWN POSITIVE (S1b: "inquisitive")
    (2,  "sad",     "R2_SAD_1"),
    (3,  "happy",   "R2_POSITIVE_1"),
    (4,  "alert",   "R2_ALARM_1"),
    (5,  "annoyed", "R2_ANNOYED"),
    (6,  "curious", "R2_CHATTY_15"),   # S1b: '"huh?"'
    (7,  "annoyed", "R2_NEGATIVE_1"),
    (8,  "alert",   "R2_HEY_1"),
    (9,  "sad",     "R2_SAD_5"),       # S1b: length only ("medium")
    (10, "happy",   "R2_POSITIVE_5"),
]
BATCHES = {"A": [1, 2, 3, 4, 5], "B": [6, 7, 8, 9, 10]}


def _append(rec: dict) -> None:
    RESULTS.parent.mkdir(parents=True, exist_ok=True)
    rec["t"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    with RESULTS.open("a") as f:
        f.write(json.dumps(rec) + "\n")


def _item(n: int):
    for it in ITEMS:
        if it[0] == n:
            return it
    raise SystemExit(f"no item {n}")


def play(n: int, volume: bool) -> bool:
    _, mood, name = _item(n)
    params = {"id": R2_SOUNDS[name]}
    if volume:
        params["volume"] = VOLUME
    p = subprocess.run([str(HERE / "r2"), "send", "sound", "--params",
                        json.dumps(params), "--wait", "6"],
                       cwd=HERE, capture_output=True, text=True)
    _append({"kind": "send", "item": n, "mood": mood, "sound": name,
             "params": params, "rc": p.returncode, "stdout": p.stdout,
             "stderr": p.stderr})
    ok = p.returncode == 0
    print(f"  #{n:<2} {'sent' if ok else 'FAILED'}  ({name})", flush=True)
    if not ok:
        print(p.stdout.strip(), p.stderr.strip(), sep="\n", file=sys.stderr)
    return ok


def cmd_batch(which: str) -> int:
    items = BATCHES[which.upper()]
    for i, n in enumerate(items):
        if i:
            time.sleep(SPACING_S)
        if not play(n, volume=(i == 0)):
            print(f"stopped at #{n}: a failed send is not a silent chirp",
                  file=sys.stderr)
            return 1
    return 0


def parse(s: str) -> dict:
    out = {}
    for tok in s.replace(",", " ").split():
        n, r = int(tok[:-1]), tok[-1].lower()
        if r not in "ynm" or not any(it[0] == n for it in ITEMS):
            raise SystemExit(f"bad rating {tok!r}: want <item><y|n|m>")
        out[n] = r
    return out


def latest_ratings(rows) -> dict:
    ratings = {}
    for r in rows:
        if r.get("kind") == "rating":
            ratings.update({int(k): v for k, v in r["ratings"].items()})
    return ratings


def verdict(ratings: dict) -> dict:
    """mood -> {"keep": [names rated y], "status": kept|dropped|incomplete}.
    `incomplete` = no y yet but an item is unrated or missed, so a re-run
    could still keep it. Only a mood whose items are ALL rated n is dropped."""
    table = {}
    for mood in MOODS:
        its = [it for it in ITEMS if it[1] == mood]
        keep = [name for n, _, name in its if ratings.get(n) == "y"]
        pending = [n for n, _, _ in its if ratings.get(n) in (None, "m")]
        status = "kept" if keep else ("incomplete" if pending else "dropped")
        table[mood] = {"keep": keep, "status": status, "pending": pending}
    return table


def cmd_verdict() -> int:
    rows = [json.loads(l) for l in RESULTS.read_text().splitlines()] if RESULTS.exists() else []
    ratings = latest_ratings(rows)
    if ratings.get(1) != "y":
        print("WARNING: item 1 is the known positive and is not rated y "
              f"({ratings.get(1)!r}) -- check the volume and the link before "
              "trusting any n.")
    for mood, v in verdict(ratings).items():
        print(f"  {mood:8} {v['status']:10} keep={v['keep']} pending={v['pending']}")
    return 0


def selftest() -> int:
    bad = 0
    def expect(r, mood, status):
        nonlocal bad
        got = verdict(r)[mood]["status"]
        if got != status:
            bad += 1
            print(f"FAIL {r} {mood}: {got} != {status}")
    all_y = {n: "y" for n, _, _ in ITEMS}
    for m in MOODS:
        expect(all_y, m, "kept")
    expect({2: "n", 9: "n"}, "sad", "dropped")
    expect({2: "n", 9: "m"}, "sad", "incomplete")
    expect({2: "n"}, "sad", "incomplete")
    expect({2: "n", 9: "y"}, "sad", "kept")
    expect({}, "alert", "incomplete")
    if verdict({2: "y", 9: "y"})["sad"]["keep"] != ["R2_SAD_1", "R2_SAD_5"]:
        bad += 1; print("FAIL keep list")
    assert parse("1y 2n,3m") == {1: "y", 2: "n", 3: "m"}
    for badtok in ("11y", "1x", "y"):
        try:
            parse(badtok); bad += 1; print(f"FAIL parse accepted {badtok}")
        except (SystemExit, ValueError):
            pass
    assert len(ITEMS) == 10 and all(sum(i[1] == m for i in ITEMS) == 2 for m in MOODS)
    assert all(name in R2_SOUNDS for _, _, name in ITEMS)
    print("selftest", "PASS" if not bad else f"FAIL ({bad})")
    return 1 if bad else 0


def main(argv) -> int:
    if not argv:
        print(__doc__); return 2
    cmd = argv[0]
    if cmd == "batch":
        return cmd_batch(argv[1])
    if cmd == "replay":
        return 0 if play(int(argv[1]), volume=True) else 1
    if cmd == "record":
        r = parse(" ".join(argv[1:]))
        _append({"kind": "rating", "ratings": r})
        return cmd_verdict()
    if cmd == "verdict":
        return cmd_verdict()
    if cmd == "selftest":
        return selftest()
    print(__doc__); return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
