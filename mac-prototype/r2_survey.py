#!/usr/bin/env python3
"""r2_survey.py — resumable state tracker for the S1 capability survey.

Issue #6. See the epic (#5) for the full spec.

WHAT THIS IS NOT
    It does not fire commands. It never opens BLE, never touches the bridge
    queue, never spawns a process. `../CLAUDE.md` and README.md forbid a
    "run all tests" wrapper — an unattended actuator sweep is exactly the
    failure mode the safety model exists to prevent. So this tool *emits* the
    command for the next item and a human runs it, deliberately, one at a time.

    That constraint is enforced by a test, not by good intentions: every
    command here must still work with subprocess/os.system/os.popen/socket
    monkeypatched to raise.

DURABILITY
    `attempts.jsonl` is the source of truth, append-only and fsync'd per write.
    `state.json` is a derived cache, rebuilt from it at any time. A single
    mutable blob rewritten per record can be truncated by a crash and lose the
    whole survey — the thing this harness exists to protect.

Usage:
    python r2_survey.py manifest --tier motion
    python r2_survey.py next --count 5
    python r2_survey.py record anim:7 --json '{"outcome":"played", ...}'
    python r2_survey.py record --amend <attempt_id> --reason "misheard"
    python r2_survey.py status
    python r2_survey.py export
"""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
import re
import sys
import time
from collections import defaultdict
from pathlib import Path

from r2_assets import (ANIMATION_ID_MAX, ANIMATION_ID_MIN, ANIMATIONS,
                       LED_CHANNELS, R2_SOUNDS)

SURVEY_DIR = Path(__file__).parent / ".survey"
MANIFEST = SURVEY_DIR / "manifest.json"
ATTEMPTS = SURVEY_DIR / "attempts.jsonl"
STATE = SURVEY_DIR / "state.json"

# Mirrors r2_probe.TIERS above `read`. `animation` moved from `motion` to
# `stance` after #11: an authored animation drives leg actions and put R2
# on the floor. An item labelled with the wrong tier tells the operator to
# open a session that will refuse it — or worse, one that can topple him.
TIERS = ("leds", "audio", "dome", "stance")
SOUNDS_PER_FAMILY = 3

# ── Attempt lifecycle (epic §"Attempt lifecycle") ────────────────────────────
# An item is done only when it has one `observed_complete` attempt. Everything
# else exists so a survey that goes wrong cannot masquerade as one that went
# right.
ATTEMPT_STATES = (
    "issued",                # emitted, not yet confirmed
    "acked",                 # daemon returned ok, observation not yet recorded
    "observed_complete",     # recorded and schema-valid — the only terminal success
    "aborted_unknown",       # link/battery/timeout died after issue; MAY have fired
    "needs_reobserve",       # fired and seen, observation unusable
    "unsafe_replay_review",  # aborted on an item that may drive the body
)
TERMINAL_OK = "observed_complete"
UNRESOLVED = {"issued", "acked", "aborted_unknown", "needs_reobserve",
              "unsafe_replay_review"}

OUTCOMES = ("played", "error", "no_effect")
ENERGY_CLASSES = ("low", "medium", "high")
WEAR_CLASSES = ("none", "dome", "drivetrain")

REQUIRED_FIELDS = ("outcome", "energy_cost_class", "wear_class",
                   "recommended_cooldown_s")
EXPORT_COLUMNS = ["item_id", "kind", "label", "outcome", "duration_s",
                  "moved_body", "moved_dome", "made_sound",
                  "energy_cost_class", "wear_class", "recommended_cooldown_s",
                  "idle_contaminated", "reads_as", "free_text"]


class SurveyError(Exception):
    """Anything the user did wrong. Caught in main(), exits non-zero."""


# ── Manifest ─────────────────────────────────────────────────────────────────

def family_of(sound_name: str) -> str:
    m = re.match(r"R2_([A-Z]+)", sound_name)
    return m.group(1) if m else sound_name


def sampled_sounds() -> list[tuple[str, int]]:
    """min(3, family_size) per R2_* family, deterministic.

    19 families: 8 singletons, R2_SCREAM has 2, 10 have >=3 -> 40 ids.
    Sorted by (family, id) so the selection never depends on dict order."""
    fam: dict[str, list[tuple[str, int]]] = defaultdict(list)
    for name, sid in R2_SOUNDS.items():
        fam[family_of(name)].append((name, sid))
    out: list[tuple[str, int]] = []
    for family in sorted(fam):
        members = sorted(fam[family], key=lambda kv: kv[1])
        out.extend(members[:SOUNDS_PER_FAMILY])
    return out


def build_manifest() -> list[dict]:
    """The 104-item playback manifest. Deterministic by construction."""
    items: list[dict] = []

    for bit in sorted(LED_CHANNELS):
        items.append({
            "item_id": f"led:{bit}", "tier": "leds", "kind": "led",
            "label": LED_CHANNELS[bit],
            "command": f"""./r2 send leds --params '{{"channels":{{"{bit}":255}}}}'""",
        })

    for name, sid in sampled_sounds():
        items.append({
            "item_id": f"sound:{sid}", "tier": "audio", "kind": "sound",
            "label": name, "family": family_of(name),
            "command": f"""./r2 send sound --params '{{"id":{sid}}}'""",
        })

    by_id = {v: k for k, v in ANIMATIONS.items()}
    for aid in range(ANIMATION_ID_MIN, ANIMATION_ID_MAX + 1):
        items.append({
            "item_id": f"anim:{aid}", "tier": "stance", "kind": "animation",
            # ids 20/23/28/29/30 are unnamed upstream — probed anyway (#12 AC2)
            "label": by_id.get(aid, "<unnamed — probe and record the error>"),
            "may_drive": aid in (8, 9, 11),   # translator.c:101-104
            "command": f"""./r2 send animation --params '{{"id":{aid}}}'""",
        })
    return items


def manifest_digest(items: list[dict]) -> str:
    return hashlib.sha256(
        json.dumps(items, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


# ── Durable store ────────────────────────────────────────────────────────────

def _atomic_write(path: Path, text: str) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


@contextlib.contextmanager
def survey_lock():
    """Exclusive lock across the read-compute-append sequence.

    attempt_id is derived from the count of existing attempts, so two concurrent
    `record` calls both read n=0 and both write `anim:7#1` — a duplicate id in an
    append-only log, which then makes `--amend` ambiguous about which one it
    superseded. Two terminals during one survey session is a completely ordinary
    thing to do, so this is not theoretical."""
    SURVEY_DIR.mkdir(parents=True, exist_ok=True)
    lock = SURVEY_DIR / ".lock"
    with open(lock, "w") as f:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)


def append_attempt(rec: dict) -> None:
    """Append-only + fsync. The one write path that must survive a crash."""
    SURVEY_DIR.mkdir(parents=True, exist_ok=True)
    with open(ATTEMPTS, "a") as f:
        f.write(json.dumps(rec, sort_keys=True) + "\n")
        f.flush()
        os.fsync(f.fileno())


def load_attempts(warn=lambda m: print(m, file=sys.stderr)) -> list[dict]:
    """Read the log. A truncated final line is discarded with a warning rather
    than aborting — a crash mid-write must cost the in-flight attempt, not the
    survey."""
    if not ATTEMPTS.exists():
        return []
    out, lines = [], ATTEMPTS.read_text().split("\n")
    for i, line in enumerate(lines):
        if not line.strip():
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            if i == len(lines) - 1 or not any(l.strip() for l in lines[i + 1:]):
                warn(f"warning: discarding truncated final line in {ATTEMPTS.name}")
            else:
                warn(f"warning: skipping corrupt line {i + 1} in {ATTEMPTS.name}")
    return out


def derive_state(attempts: list[dict]) -> dict:
    """Rebuild the cache from the log. Amendments supersede by attempt_id."""
    superseded = {a["supersedes"] for a in attempts if a.get("supersedes")}
    by_item: dict[str, list[dict]] = defaultdict(list)
    for a in attempts:
        if a["attempt_id"] in superseded:
            continue
        by_item[a["item_id"]].append(a)
    items = {}
    for item_id, atts in by_item.items():
        atts.sort(key=lambda a: a["ts"])
        items[item_id] = {
            "attempts": atts,
            "state": atts[-1]["state"],
            "done": any(a["state"] == TERMINAL_OK for a in atts),
        }
    return {"items": items, "count": len(attempts)}


def save_state_cache(state: dict) -> None:
    _atomic_write(STATE, json.dumps(state, indent=2, sort_keys=True))


def load_manifest(verify: bool = True) -> list[dict]:
    """Load the stored manifest and re-verify it against freshly generated data.

    `next` prints `item["command"]` for a human to run. If the stored manifest
    drifts — hand-edited, half-written, or left over from an older r2_assets.py —
    it could print an animation command under an LED item's id, and `record`
    would then mark the wrong item observed. The operator has no way to notice.
    The manifest is a cache of generated data, so treat any mismatch as fatal
    rather than trusting the file."""
    if not MANIFEST.exists():
        raise SurveyError("no manifest — run: r2_survey.py manifest")
    items = json.loads(MANIFEST.read_text())["items"]
    if verify:
        canonical = {i["item_id"]: i for i in build_manifest()}
        for stored in items:
            fresh = canonical.get(stored["item_id"])
            if fresh is None:
                raise SurveyError(
                    f"manifest is stale: {stored['item_id']!r} is not a generatable "
                    f"item. Regenerate with `r2_survey.py manifest`.")
            if stored.get("command") != fresh["command"] or \
               stored.get("may_drive", False) != fresh.get("may_drive", False):
                raise SurveyError(
                    f"manifest drift on {stored['item_id']!r}: stored command does "
                    f"not match generated. Refusing to emit a command that may not "
                    f"match its item. Regenerate with `r2_survey.py manifest`.")
    return items


def new_attempt_id(item_id: str, n: int) -> str:
    return f"{item_id}#{n}"


# ── Schema validation ────────────────────────────────────────────────────────

def validate_observation(obs: dict) -> None:
    missing = [f for f in REQUIRED_FIELDS if f not in obs]
    if missing:
        raise SurveyError(f"observation missing required field(s): {', '.join(missing)}")
    if obs["outcome"] not in OUTCOMES:
        raise SurveyError(f"outcome must be one of {OUTCOMES}, got {obs['outcome']!r}")
    if obs["energy_cost_class"] not in ENERGY_CLASSES:
        raise SurveyError(f"energy_cost_class must be one of {ENERGY_CLASSES}, "
                          f"got {obs['energy_cost_class']!r}")
    if obs["wear_class"] not in WEAR_CLASSES:
        raise SurveyError(f"wear_class must be one of {WEAR_CLASSES}, "
                          f"got {obs['wear_class']!r}")
    cooldown = obs["recommended_cooldown_s"]
    # isinstance(True, int) is True in Python, so bool must be excluded explicitly
    # or `"recommended_cooldown_s": true` silently becomes a 1-second cooldown.
    if isinstance(cooldown, bool) or not isinstance(cooldown, (int, float)):
        raise SurveyError("recommended_cooldown_s must be a number, not "
                          f"{type(cooldown).__name__}")
    if cooldown < 0:
        raise SurveyError("recommended_cooldown_s must be >= 0")


# ── Commands ─────────────────────────────────────────────────────────────────

def cmd_manifest(args) -> int:
    items = build_manifest()
    if args.tier:
        items = [i for i in items if i["tier"] == args.tier]
    SURVEY_DIR.mkdir(parents=True, exist_ok=True)
    payload = {"generated_from": "r2_assets.py", "tier": args.tier or "all",
               "count": len(items), "items": items}
    _atomic_write(MANIFEST, json.dumps(payload, indent=2, sort_keys=True))
    print(f"manifest: {len(items)} items"
          f"{' (tier ' + args.tier + ')' if args.tier else ''} -> {MANIFEST}")
    print(f"sha256: {manifest_digest(items)}")
    for tier in TIERS:
        n = sum(1 for i in items if i["tier"] == tier)
        if n:
            print(f"  {tier:7} {n}")
    return 0


def cmd_next(args) -> int:
    items, state = load_manifest(), derive_state(load_attempts())
    pending = []
    for it in items:
        st = state["items"].get(it["item_id"])
        if st and st["done"]:
            continue
        if st and st["state"] == "unsafe_replay_review":
            print(f"⛔ {it['item_id']} held at unsafe_replay_review — an aborted "
                  f"attempt on an item that may drive the body.\n"
                  f"   Resolve deliberately before retrying:\n"
                  f"   r2_survey.py resolve {it['item_id']} --state needs_reobserve",
                  file=sys.stderr)
            continue
        if st and st["state"] in UNRESOLVED:
            print(f"⚠ {it['item_id']} has an unresolved attempt "
                  f"({st['state']}) — resolve it before re-firing", file=sys.stderr)
            continue
        pending.append(it)
        if len(pending) >= args.count:
            break

    if not pending:
        print("nothing pending (or everything pending is blocked — see above)")
        return 0

    print(f"# next {len(pending)} item(s) — run ONE AT A TIME, observe, then record\n")
    for it in pending:
        warn = "   ⚠ MAY DRIVE THE BODY — clear floor, stay in reach\n" if it.get("may_drive") else ""
        print(f"# {it['item_id']}  ({it['kind']}: {it['label']})\n{warn}{it['command']}\n")

    # Hand-off is a state change for anything that can drive the body. Without
    # this, an operator who fires anim:11, then loses the session before
    # recording, leaves no trace — and the next `next` re-emits a driving
    # animation as though it had never run. Cheap items are not marked: a lost
    # record there costs a replayed LED, which is harmless.
    driving = [it for it in pending if it.get("may_drive")]
    if driving:
        with survey_lock():
            state = derive_state(load_attempts(warn=lambda m: None))
            for it in driving:
                prior = state["items"].get(it["item_id"], {}).get("attempts", [])
                append_attempt({
                    "attempt_id": new_attempt_id(it["item_id"], len(prior) + 1),
                    "item_id": it["item_id"], "state": "issued", "ts": time.time(),
                    "note": "emitted by `next` — resolve or record before re-firing",
                })
            save_state_cache(derive_state(load_attempts(warn=lambda m: None)))
        print(f"# {len(driving)} driving item(s) marked `issued`. If you do not run one,"
              f"\n# clear it:  r2_survey.py resolve <item_id> --state needs_reobserve"
              f" --reason 'not fired'\n")
    print("# then, for each:")
    print("""#   r2_survey.py record <item_id> --json '{"outcome":"played",""")
    print("""#     "energy_cost_class":"low","wear_class":"none","recommended_cooldown_s":0}'""")
    return 0


def cmd_record(args) -> int:
    with survey_lock():
        return _record_locked(args)


def _record_locked(args) -> int:
    attempts = load_attempts()
    state = derive_state(attempts)

    if args.amend:
        target = next((a for a in attempts if a["attempt_id"] == args.amend), None)
        if target is None:
            raise SurveyError(f"unknown attempt_id {args.amend!r}")
        if not args.reason:
            raise SurveyError("--amend requires --reason")
        obs = json.loads(args.json) if args.json else dict(target["observation"])
        validate_observation(obs)
        item_id = target["item_id"]
        n = len(state["items"].get(item_id, {}).get("attempts", [])) + 1
        rec = {"attempt_id": new_attempt_id(item_id, n), "item_id": item_id,
               "state": TERMINAL_OK, "ts": time.time(), "observation": obs,
               "supersedes": args.amend, "amend_reason": args.reason}
        append_attempt(rec)
        save_state_cache(derive_state(load_attempts()))
        print(f"amended {args.amend} -> {rec['attempt_id']} ({args.reason})")
        return 0

    item_id = args.item_id
    if item_id is None:
        raise SurveyError("record needs an item_id (or --amend <attempt_id>)")
    manifest_ids = {i["item_id"] for i in load_manifest()}
    if item_id not in manifest_ids:
        raise SurveyError(f"unknown item_id {item_id!r} — not in the manifest")
    if not args.json:
        raise SurveyError("record needs --json '<observation>'")
    obs = json.loads(args.json)
    validate_observation(obs)

    existing = state["items"].get(item_id, {}).get("attempts", [])
    if any(a["state"] == TERMINAL_OK for a in existing):
        raise SurveyError(f"{item_id} already has a completed observation; "
                          f"use --amend <attempt_id> to correct it")
    rec = {"attempt_id": new_attempt_id(item_id, len(existing) + 1),
           "item_id": item_id, "state": TERMINAL_OK, "ts": time.time(),
           "observation": obs}
    append_attempt(rec)
    save_state_cache(derive_state(load_attempts()))
    print(f"recorded {rec['attempt_id']}: {obs['outcome']}")
    return 0


def cmd_resolve(args) -> int:
    """Move an unresolved attempt to another state — the manual escape hatch
    for a session that aborted."""
    with survey_lock():
        return _resolve_locked(args)


def _resolve_locked(args) -> int:
    if args.state not in ATTEMPT_STATES:
        raise SurveyError(f"state must be one of {ATTEMPT_STATES}")
    # `observed_complete` is the ONE state that carries an observation. Letting
    # resolve mint it produced a terminal-success attempt with no `observation`
    # key: status counted the item done, and export then died on a raw KeyError.
    # Manufacturing success is exactly what the lifecycle exists to prevent.
    if args.state == TERMINAL_OK:
        raise SurveyError(f"{TERMINAL_OK!r} requires an observation — "
                          f"use `record {args.item_id} --json '<observation>'`")
    # Validate against the MANIFEST, not against prior attempts. A session that
    # aborts mid-run must be able to mark items that were fired but never
    # recorded — those have no attempt yet, which is precisely why they need
    # marking. Requiring a prior attempt made the abort path unreachable and
    # let `next` re-emit a possibly-driving animation. Caught by
    # test_export_blocked_by_unresolved_attempt / test_next_refuses_unsafe_replay_item.
    if args.item_id not in {i["item_id"] for i in load_manifest()}:
        raise SurveyError(f"unknown item_id {args.item_id!r} — not in the manifest")
    state = derive_state(load_attempts())
    st = state["items"].get(args.item_id)
    if st and st["done"] and args.state != TERMINAL_OK:
        raise SurveyError(f"{args.item_id} is already observed_complete; "
                          f"use `record --amend` to correct it")
    n = len(st["attempts"]) + 1 if st else 1
    rec = {"attempt_id": new_attempt_id(args.item_id, n),
           "item_id": args.item_id, "state": args.state, "ts": time.time(),
           "note": args.reason or ""}
    append_attempt(rec)
    save_state_cache(derive_state(load_attempts()))
    print(f"{args.item_id} -> {args.state}")
    return 0


def cmd_status(args) -> int:
    items, state = load_manifest(), derive_state(load_attempts())
    per_tier: dict[str, list[int]] = {t: [0, 0] for t in TIERS}
    for it in items:
        st = state["items"].get(it["item_id"])
        per_tier[it["tier"]][1] += 1
        if st and st["done"]:
            per_tier[it["tier"]][0] += 1
    unresolved = all_unresolved(state)      # whole log, not just this manifest
    total_done = sum(v[0] for v in per_tier.values())
    total = sum(v[1] for v in per_tier.values())
    print(f"observed {total_done}/{total}")
    for tier in TIERS:
        done, n = per_tier[tier]
        if n:
            print(f"  {tier:7} {done:3}/{n}")
    if unresolved:
        print(f"\n{len(unresolved)} unresolved attempt(s) — export is blocked:")
        for item_id, s in unresolved:
            print(f"  {item_id:14} {s}")
    return 0


def all_unresolved(state: dict) -> list[tuple[str, str]]:
    """Every unresolved attempt in the LOG, regardless of the current manifest.

    Scoping this to the loaded manifest meant `manifest --tier leds` silently
    dropped an outstanding `unsafe_replay_review` on a motion item — the export
    gate passed while a possibly-driving animation sat unaccounted for. The log
    is the source of truth; the manifest is just the current view."""
    return sorted((item_id, st["state"]) for item_id, st in state["items"].items()
                  if not st["done"] and st["state"] in UNRESOLVED)


def cmd_export(args) -> int:
    items, state = load_manifest(), derive_state(load_attempts())
    unresolved = all_unresolved(state)
    if unresolved:
        print("export blocked — unresolved attempts:", file=sys.stderr)
        for item_id, s in unresolved:
            in_view = " " if any(i["item_id"] == item_id for i in items) else " (not in the current manifest view)"
            print(f"  {item_id}: {s}{in_view}", file=sys.stderr)
        raise SurveyError(f"{len(unresolved)} attempt(s) not observed_complete")

    rows = []
    for it in items:
        st = state["items"].get(it["item_id"])
        if not (st and st["done"]):
            continue
        obs = next(a["observation"] for a in reversed(st["attempts"])
                   if a["state"] == TERMINAL_OK)
        row = {"item_id": it["item_id"], "kind": it["kind"], "label": it["label"]}
        for c in EXPORT_COLUMNS[3:]:
            v = obs.get(c, "")
            row[c] = "" if v is None else str(v)
        rows.append(row)

    print("| " + " | ".join(EXPORT_COLUMNS) + " |")
    print("|" + "|".join("---" for _ in EXPORT_COLUMNS) + "|")
    for r in rows:
        print("| " + " | ".join(str(r[c]).replace("|", "\\|") for c in EXPORT_COLUMNS) + " |")
    print(f"\n<!-- {len(rows)} observed of {len(items)} manifest items -->", file=sys.stderr)
    return 0


def cmd_rebuild(args) -> int:
    """Rebuild state.json from attempts.jsonl — proves the cache is derived."""
    save_state_cache(derive_state(load_attempts()))
    print(f"rebuilt {STATE} from {ATTEMPTS.name}")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("manifest", help="generate the item manifest")
    m.add_argument("--tier", choices=TIERS)
    m.set_defaults(fn=cmd_manifest)

    n = sub.add_parser("next", help="print the next item(s) as commands — does NOT run them")
    n.add_argument("--count", type=int, default=5)
    n.set_defaults(fn=cmd_next)

    r = sub.add_parser("record", help="record an observation")
    r.add_argument("item_id", nargs="?")
    r.add_argument("--json", help="observation as a JSON object")
    r.add_argument("--amend", metavar="ATTEMPT_ID")
    r.add_argument("--reason")
    r.set_defaults(fn=cmd_record)

    rs = sub.add_parser("resolve", help="move an unresolved attempt to another state")
    rs.add_argument("item_id")
    rs.add_argument("--state", required=True, choices=ATTEMPT_STATES)
    rs.add_argument("--reason")
    rs.set_defaults(fn=cmd_resolve)

    sub.add_parser("status", help="progress per tier").set_defaults(fn=cmd_status)
    sub.add_parser("export", help="emit the markdown results table").set_defaults(fn=cmd_export)
    sub.add_parser("rebuild", help="rebuild state.json from attempts.jsonl").set_defaults(fn=cmd_rebuild)

    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except SurveyError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except json.JSONDecodeError as e:
        print(f"error: --json is not valid JSON: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
