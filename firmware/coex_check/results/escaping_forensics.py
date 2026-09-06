#!/usr/bin/env python3
"""Why 8 battery replies went missing in the A1 endurance run (D-024).

Run: python3 escaping_forensics.py

The A1 arms logged every response R2 sent, and nothing else about the requests.
That is enough, because the sequence byte is a counter: consecutive battery
replies are exactly 21 apart (20 keepalives + the read itself), so a gap of 42
is one request that was never answered. The missing sequence number is then
arithmetic, not guesswork.

The point of the script is that the conclusion is re-derivable from committed
evidence rather than asserted. It reads the logs; it does not embed the answer.
"""
import re
import pathlib

SPECIAL = {0x8D, 0xD8, 0xAB}          # SOP, EOP, ESC -- must be escaped in a body
HERE = pathlib.Path(__file__).parent


def mangled_seqs(flags, did, cid):
    """Sequence numbers for which the OLD unescaped encoder produced a bad frame.

    Two ways in: the sequence byte itself is a special value, or the checksum
    computed over it is. The second was the easy one to miss -- it triples the
    hit rate rather than leaving it at three-in-256.
    """
    bad = set()
    for seq in range(256):
        chk = (~(flags + did + cid + seq)) & 0xFF
        if any(b in SPECIAL for b in (flags, did, cid, seq, chk)):
            bad.add(seq)
    return bad


BAD_BATTERY = mangled_seqs(0x0A, 0x13, 0x03)


def analyse(path):
    txt = path.read_text()
    seqs = [int(m, 16) for m in
            re.findall(r"rx \(10 bytes\): 8D 09 13 03 ([0-9A-F]{2})", txt)]
    if not seqs:
        return None
    lost = []
    for a, b in zip(seqs, seqs[1:]):
        gap = (b - a) % 256
        if gap == 42:                 # exactly one reply missing between them
            lost.append((a + 21) % 256)
        elif gap != 21:
            lost.append(None)         # something we do not model; report it
    return seqs, lost


def main():
    total_lost = total_explained = 0
    print("sequence numbers the old encoder mangled for a battery read: %s\n"
          % sorted(hex(s) for s in BAD_BATTERY))
    for path in sorted(HERE.glob("arm*.txt")):
        got = analyse(path)
        if got is None:
            print("%-24s no battery responses logged (predates the read)" % path.name)
            continue
        seqs, lost = got
        print("%-24s %d replies, %d lost" % (path.name, len(seqs), len(lost)))
        for seq in lost:
            if seq is None:
                print("      unmodelled gap -- inspect by hand")
                continue
            total_lost += 1
            hit = seq in BAD_BATTERY
            total_explained += hit
            print("      lost reply to seq 0x%02X   mangled by the old encoder? %s"
                  % (seq, "YES" if hit else "no"))
    print()
    if total_lost:
        print("%d/%d lost replies explained by the escaping bug (%.0f%%)"
              % (total_explained, total_lost, 100.0 * total_explained / total_lost))
    print("\nAnd the fix, measured on hardware:")
    print("  firmware/link_check/results/first-contact-2026-09-05.txt")
    print("  sends a battery read at each mangled sequence number -- 6/6 answered.")


if __name__ == "__main__":
    main()
