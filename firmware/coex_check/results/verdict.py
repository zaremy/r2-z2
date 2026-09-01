#!/usr/bin/env python3
"""Apply #103 A1's pre-declared verdict to the captured arm logs.

The thresholds were fixed in the issue BEFORE any arm ran, so the verdict is
arithmetic, not judgement:

  PASS      zero disconnects in all three arms, and no gap between successful
            keepalives over 10 s (three missed) in any arm
  MARGINAL  arms 1-2 clean; arm 3 has gaps >10 s but no disconnect
  FAIL      any disconnect in any arm, or any gap >10 s in arms 1-2

Two scoring rules that matter, both about not counting our own startup as a
result:

  1. Nothing before the first `LINK UP` counts. The firmware emits a failed
     keepalive every 3 s while the link is still coming up, and in arm 2 Wi-Fi
     init pushed the BLE connect out to 7.3 s. Those are startup, not
     coexistence.
  2. A Wi-Fi disconnect is NOT a BLE disconnect. Arms 2 and 3 reconnect Wi-Fi
     automatically; it is counted and reported separately so an AP problem can
     never be read as a radio-sharing failure.

    python3 verdict.py arm1.log arm2.log arm3.log
"""
import re
import sys

GAP_LIMIT_S = 10.0


def score(path):
    text = open(path, errors="replace").read()
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
    lines = text.splitlines()

    first_up = next((i for i, l in enumerate(lines) if "LINK UP" in l), None)
    body = lines[first_up:] if first_up is not None else []

    down_lines  = sum(1 for l in body if "LINK DOWN" in l)
    breaches    = sum(1 for l in body if "EXCEEDS THE" in l)
    wifi_drops  = sum(1 for l in body if "Wi-Fi disconnected" in l)
    cap_gaps    = sum(1 for l in lines if "monitor exited" in l)

    worst, sent, ack, fail, arm, last_t = 0.0, 0, 0, 0, "?", 0.0
    summary_disc = 0
    summaries = 0
    stamps = []          # every t+ seen, in order, to prove the run PROGRESSED
    for l in body:
        m = re.search(r"\[(\S+)\] t\+(\d+)s .*sent=(\d+) ack=(\d+) fail=(\d+)"
                      r"\s+disconnects=(\d+)\s+worst gap=([\d.]+)s", l)
        if m:
            arm = m.group(1)
            last_t = float(m.group(2))
            sent, ack, fail = int(m.group(3)), int(m.group(4)), int(m.group(5))
            # The firmware's own cumulative count. Scored ALONGSIDE the
            # instantaneous LINK DOWN line, because the two can disagree: a
            # distillation that kept summaries but dropped the event line would
            # otherwise hide a disconnect. Take the worse of the two.
            summary_disc = max(summary_disc, int(m.group(6)))
            stamps.append(float(m.group(2)))
            worst = max(worst, float(m.group(7)))
            summaries += 1

    below_1mbps = sum(1 for l in body if "BELOW 1 Mbps" in l)
    # From body, not text: load recorded before the BLE link came up says
    # nothing about BLE under load.
    loads, load_times = [], []
    for l in body:
        m = re.search(r"^I \((\d+)\).*load ([\d.]+) Mbit/s", l)
        if m:
            load_times.append(int(m.group(1)) / 1000.0)
            loads.append(float(m.group(2)))

    return {
        "file": path, "arm": arm, "minutes": last_t / 60.0,
        "sent": sent, "ack": ack, "fail_after_link_up": fail,
        "disconnects": max(down_lines, summary_disc),
        "down_lines": down_lines, "summary_disc": summary_disc,
        "summaries": summaries, "stamps": stamps,
        "complete": "capture end" in text,
        "gap_breaches": breaches,
        "worst_gap_s": worst, "wifi_drops": wifi_drops,
        "capture_gaps": cap_gaps,
        "load_samples": len(loads), "load_times": load_times,
        "load_mean_mbps": (sum(loads) / len(loads)) if loads else None,
        "load_below_target": below_1mbps,
    }


def main(paths):
    rows = [score(p) for p in paths]
    for r in rows:
        print(f"\n{r['arm']}  ({r['file']})")
        print(f"  duration            : {r['minutes']:.0f} min")
        print(f"  keepalives sent/ack : {r['sent']} / {r['ack']}")
        print(f"  BLE disconnects     : {r['disconnects']}"
              f"   (event lines {r['down_lines']}, firmware count {r['summary_disc']})")
        print(f"  worst gap           : {r['worst_gap_s']:.1f} s   (limit {GAP_LIMIT_S:.0f})")
        print(f"  gap breaches        : {r['gap_breaches']}")
        print(f"  Wi-Fi reconnects    : {r['wifi_drops']}   (not a BLE failure)")
        print(f"  capture gaps        : {r['capture_gaps']}   (holes in MY recording)")
        if r["load_samples"]:
            print(f"  arm-3 load          : {r['load_mean_mbps']:.2f} Mbit/s mean, "
                  f"{r['load_below_target']} windows under 1 Mbps")

    # VALIDATE THE INPUTS BEFORE SCORING THEM. An earlier version of this
    # script printed PASS for three copies of /dev/null: every counter defaulted
    # to zero, zero disconnects and zero gap breaches looked like a clean sweep,
    # and len(rows)>=3 was the only gate. A verdict tool that cannot fail is
    # worth nothing, and this project has already lost a whole survey to one.
    MIN_MINUTES = 55.0
    problems = []
    warnings = []
    seen = {}
    for r in rows:
        tag = r["arm"][:4]
        if r["summaries"] == 0:
            problems.append(f"{r['file']}: no summary lines parsed -- not a usable arm log")
            continue
        if tag not in ("arm1", "arm2", "arm3"):
            problems.append(f"{r['file']}: unrecognised arm '{r['arm']}'")
            continue
        if tag in seen:
            problems.append(f"{r['file']}: duplicate {tag} (already seen in {seen[tag]})")
            continue
        seen[tag] = r["file"]
        if r["minutes"] < MIN_MINUTES:
            problems.append(f"{r['arm']}: only {r['minutes']:.0f} min, the spec calls for one hour")
        # Shape is not enough. A single fabricated summary line with a large
        # t+ and zero counters satisfied every structural check and still
        # reached PASS, so the run must also look like a real run:
        st = r["stamps"]
        if len(st) != len(set(st)):
            problems.append(f"{r['arm']}: repeated t+ timestamps -- the summaries do not "
                            f"describe a progressing run")
        elif any(b <= a for a, b in zip(st, st[1:])):
            problems.append(f"{r['arm']}: t+ timestamps are not increasing")
        elif st and (st[-1] - st[0]) < MIN_MINUTES * 60 * 0.9:
            problems.append(f"{r['arm']}: summaries span only "
                            f"{(st[-1] - st[0]) / 60:.0f} min of elapsed time")
        if r["summaries"] < 30:
            problems.append(f"{r['arm']}: only {r['summaries']} summary lines for "
                            f"{r['minutes']:.0f} min -- one per minute is expected")
        if r["sent"] < 1000:
            problems.append(f"{r['arm']}: only {r['sent']} keepalives sent; an hour at 3 s is ~1200")
        if r["sent"] and r["ack"] / r["sent"] < 0.95:
            problems.append(f"{r['arm']}: only {r['ack']}/{r['sent']} keepalives acked "
                            f"({100.0 * r['ack'] / r['sent']:.0f}%) -- the link was not healthy")
        if r["fail_after_link_up"] > 5:
            problems.append(f"{r['arm']}: {r['fail_after_link_up']} failed keepalives "
                            f"(only the 2 pre-link startup ones are expected)")
        if tag == "arm3":
            # Arm 3's whole purpose is BLE under load. Without evidence the load
            # ran, a clean BLE result means nothing at all.
            # "Sustained" is a word the docs use, so it has to be earned. One
            # sample proves a moment, not an hour: require enough windows to
            # cover most of the run. The real arm 3 reports roughly every 100 s
            # (one full 64 MB fetch), so ~35 over an hour; 15 is a floor that
            # real data clears easily and a single fabricated line cannot.
            # "Sustained" is a word the docs use, so it has to be earned --
            # but the test is COVERAGE, not sample count. Each window is one
            # complete 64 MB fetch, roughly 4.4 minutes of continuous transfer,
            # so an hour yields only ~14 of them. Counting samples would have
            # rejected the real run; what actually matters is that the windows
            # SPAN the arm, which a single fabricated line cannot do.
            lt = r["load_times"]
            if not lt:
                problems.append("arm3: no load measurements -- cannot show it ran under load")
            elif len(lt) < 5:
                problems.append(f"arm3: only {len(lt)} load windows -- too few to show coverage")
            elif (lt[-1] - lt[0]) < r["minutes"] * 60 * 0.9:
                problems.append(f"arm3: load windows span only {(lt[-1] - lt[0]) / 60:.0f} min "
                                f"of a {r['minutes']:.0f} min arm -- the load was not sustained")
            elif r["load_mean_mbps"] < 1.0 or r["load_below_target"]:
                problems.append(f"arm3: load {r['load_mean_mbps']:.2f} Mbit/s mean with "
                                f"{r['load_below_target']} windows under 1 Mbps -- "
                                f"below the specified load")
        if not r["complete"]:
            # The end marker corroborates a clean capture shutdown; DURATION is
            # the substantive test for truncation and is checked above. Arm 1's
            # capture was killed by hand after its window elapsed, so it has a
            # full hour of data and no marker. Downgraded to a warning rather
            # than deleted, so a SHORT run with no marker is still rejected by
            # the duration rule and the missing marker stays visible.
            warnings.append(f"{r['arm']}: no capture-end marker "
                            f"(duration {r['minutes']:.0f} min is the check that matters)")
    for need in ("arm1", "arm2", "arm3"):
        if need not in seen:
            problems.append(f"missing {need}")

    for w in warnings:
        print(f"\nNOTE: {w}")

    if problems:
        print("\n" + "=" * 60)
        print("VERDICT: INVALID -- the inputs cannot support any verdict:")
        for p in problems:
            print(f"  - {p}")
        print("=" * 60)
        return

    any_disc = any(r["disconnects"] for r in rows)
    early    = [r for r in rows if r["arm"].startswith(("arm1", "arm2"))]
    early_gap = any(r["gap_breaches"] or r["worst_gap_s"] > GAP_LIMIT_S for r in early)
    late_gap  = any(r["gap_breaches"] or r["worst_gap_s"] > GAP_LIMIT_S
                    for r in rows if r["arm"].startswith("arm3"))

    print("\n" + "=" * 60)
    if any_disc or early_gap:
        print("VERDICT: FAIL  -- D-005 is revisited before further firmware work")
    elif late_gap:
        print("VERDICT: MARGINAL -- usable with a documented caveat about heavy Wi-Fi use")
    else:
        print("VERDICT: PASS")
    print("=" * 60)


if __name__ == "__main__":
    main(sys.argv[1:] or ["arm1.log"])
