"""A session manifest: the whole hardware plan, approved once (#88).

WHAT THIS IS FOR, because it is easy to mistake for a permissions feature.

`CLAUDE.md` records a session costing FOUR manual relaunches and blames the
daemon's idle timeout. A DX review found the likelier cause: the permission
ceiling is fixed when the daemon starts, so every time a session discovers it
needs `leds` having launched at `read`, that is one relaunch -- bought with the
operator's attention, mid-session, at the moment they were doing something
else. Their attention is the scarce resource in this project.

**This is not a way to relaunch faster. It is a way to stop needing to.** The
agent declares the whole plan before the link exists, the human approves it
once, and the daemon runs the whole session against it.

AND IT IS A CEILING DECLARATION, NEVER AN EXEMPTION. This module can only ever
narrow what a session may do:

  - the required tier is COMPUTED from the ops and cannot be declared. A
    manifest that tries to name its own tier is REJECTED rather than ignored,
    because a silently-dropped field is indistinguishable from an honoured one.
  - membership is an ADDITIONAL check. `handle_request` still calls `op_tier`
    and `_tier_ok` on the send path exactly as before; a manifest cannot admit
    an op the ladder refuses.
  - a manifest that lists no ops permits nothing.

MEMBERSHIP KEYS ON THE OP NAME, NOT THE PARAMS, and that is deliberate.
`r2_probe.op_tier` is total and params-independent on purpose -- its docstring
records that params-dependent tiers were a mistake, because authorisation and
the handler then derive the answer separately. Params in a manifest are for the
OPERATOR to read before approving; they are not a second authorisation channel.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import r2_probe as P

# Keys a manifest may carry. Anything else is an error rather than a
# silently-ignored field: a typo'd "hazzards" that is quietly dropped means the
# operator approves a plan whose hazards were never shown to them.
_ALLOWED_KEYS = {"title", "why", "ops", "expected_s", "hazards", "teardown"}

# Keys that are FORBIDDEN by name, because they would look like they work.
# `tier`/`ceiling`/`allow` are the ones somebody reaches for when the computed
# tier is inconvenient, and accepting-then-ignoring them is worse than
# refusing: the file would read as though it had been honoured.
_FORBIDDEN_KEYS = {"tier", "ceiling", "allow", "max_tier", "required_tier"}


# Ops that a manifest can never exclude.
#
# `stop` is the teardown primitive and the project's standing rule is DEFAULT
# TO STOP: a disconnect or failure must result in stop, not last-command. A
# manifest that forgot to list it would make the one refusal that can leave him
# MOVING -- the manifest would have made the session more dangerous than no
# manifest at all, which is the opposite of the point.
#
# It is exactly one op and it is `read` tier, so this cannot raise a ceiling.
# Everything else a session needs, including its own teardown, must be declared
# and seen by the operator.
ALWAYS_PERMITTED = frozenset({"stop"})


class ManifestError(ValueError):
    """Refuse loudly. A manifest that cannot be trusted must not run at a
    default ceiling; it must not run."""


@dataclass(frozen=True)
class Step:
    op: str
    params: dict = field(default_factory=dict)
    note: str = ""


@dataclass(frozen=True)
class Manifest:
    title: str
    steps: tuple[Step, ...]
    why: str = ""
    expected_s: float = 0.0
    hazards: tuple[str, ...] = ()
    teardown: str = ""

    # ---- the whole point ------------------------------------------------
    @property
    def required_tier(self) -> str:
        """The highest tier any declared op needs.

        COMPUTED, never declared, and there is no setter and no field to
        override it. A manifest whose ops need `dome` cannot launch at `leds`
        and cannot be hand-annotated to claim otherwise."""
        if not self.steps:
            # No ops means no capability, not "the safe default". A manifest
            # that permits nothing should launch a daemon that can do nothing.
            return P.TIERS[0]
        return max((P.op_tier(s.op) for s in self.steps),
                   key=P.TIERS.index)

    @property
    def ops(self) -> frozenset[str]:
        return frozenset(s.op for s in self.steps)

    def permits(self, op: str) -> bool:
        return op in self.ops or op in ALWAYS_PERMITTED

    def refusal(self, op: str) -> dict:
        """AC2: the refusal carries the manifest's own contents, so the
        operator can see what WAS permitted rather than only what was not."""
        return {"ok": False, "op": op,
                "error": f"op {op!r} is not in the approved session manifest "
                         f"{self.title!r}",
                "manifest": {"title": self.title,
                             "tier": self.required_tier,
                             "ops": sorted(self.ops)}}

    # ---- what the operator reads before approving (AC4) -----------------
    def describe(self) -> str:
        lines = [f"SESSION MANIFEST — {self.title}"]
        if self.why:
            lines.append(f"  {self.why}")
        lines.append("")
        lines.append(f"  ceiling required : {self.required_tier}"
                     f"   (computed from the ops below, not declared)")
        if self.expected_s:
            lines.append(f"  expected duration: {self.expected_s:.0f}s")
        lines.append("")
        lines.append(f"  {len(self.steps)} step(s):")
        for i, s in enumerate(self.steps, 1):
            tier = P.op_tier(s.op)
            p = json.dumps(s.params, sort_keys=True) if s.params else ""
            lines.append(f"    {i:2d}. [{tier:6s}] {s.op} {p}".rstrip())
            if s.note:
                lines.append(f"        {s.note}")
        # Hazards are printed LAST and unmissably, because the operator reads
        # top-down and stops when they think they understand the plan.
        if self.hazards:
            lines.append("")
            lines.append("  HAZARDS:")
            for h in self.hazards:
                lines.append(f"    !! {h}")
        if self.teardown:
            lines.append("")
            lines.append(f"  on exit (including refusal and Ctrl-C): {self.teardown}")
        return "\n".join(lines)


def _fail(msg: str) -> None:
    raise ManifestError(msg)


def parse(data: dict, *, source: str = "<dict>") -> Manifest:
    if not isinstance(data, dict):
        _fail(f"{source}: a manifest must be a JSON object, "
              f"got {type(data).__name__}")

    forbidden = _FORBIDDEN_KEYS & set(data)
    if forbidden:
        _fail(f"{source}: manifest declares {sorted(forbidden)}, but the "
              f"required tier is COMPUTED from the ops and cannot be "
              f"declared. Remove it — a file that names its own ceiling reads "
              f"as though that ceiling were honoured.")

    unknown = set(data) - _ALLOWED_KEYS
    if unknown:
        _fail(f"{source}: unknown manifest key(s) {sorted(unknown)}. "
              f"Ignoring them would let a typo'd 'hazards' hide a hazard the "
              f"operator never saw.")

    title = data.get("title")
    if not isinstance(title, str) or not title.strip():
        _fail(f"{source}: 'title' must be a non-empty string — it is what the "
              f"operator is approving.")

    raw_ops = data.get("ops")
    if not isinstance(raw_ops, list):
        _fail(f"{source}: 'ops' must be a list")
    if not raw_ops:
        _fail(f"{source}: 'ops' is empty. A manifest that permits nothing "
              f"cannot be what you meant to approve.")

    steps = []
    for i, entry in enumerate(raw_ops, 1):
        if not isinstance(entry, dict):
            _fail(f"{source}: ops[{i}] must be an object, "
                  f"got {type(entry).__name__}")
        extra = set(entry) - {"op", "params", "note"}
        if extra:
            _fail(f"{source}: ops[{i}] has unknown key(s) {sorted(extra)}")
        op = entry.get("op")
        if not isinstance(op, str) or op not in P.OPS:
            _fail(f"{source}: ops[{i}] names unknown op {op!r}. "
                  f"An unknown op cannot have its tier computed, so the "
                  f"manifest's ceiling would be a guess.")
        params = entry.get("params", {})
        if not isinstance(params, dict):
            _fail(f"{source}: ops[{i}] 'params' must be an object")
        note = entry.get("note", "")
        if not isinstance(note, str):
            _fail(f"{source}: ops[{i}] 'note' must be a string")
        steps.append(Step(op=op, params=params, note=note))

    hazards = data.get("hazards", [])
    if not isinstance(hazards, list) or not all(isinstance(h, str) for h in hazards):
        _fail(f"{source}: 'hazards' must be a list of strings")

    expected = data.get("expected_s", 0.0)
    if not isinstance(expected, (int, float)) or isinstance(expected, bool) \
            or expected < 0:
        _fail(f"{source}: 'expected_s' must be a non-negative number")

    for key in ("why", "teardown"):
        if key in data and not isinstance(data[key], str):
            _fail(f"{source}: {key!r} must be a string")

    m = Manifest(title=title.strip(), steps=tuple(steps),
                 why=data.get("why", ""), expected_s=float(expected),
                 hazards=tuple(hazards), teardown=data.get("teardown", ""))

    # A manifest reaching `stance` must SAY so. That tier is the one that can
    # put him on the floor -- an authored animation is a stance command whose
    # contents we do not get to inspect first (#11, D-009) -- and approving it
    # without the word in front of you is the failure this whole file exists
    # to prevent.
    if m.required_tier == "stance" and not m.hazards:
        _fail(f"{source}: this manifest reaches the 'stance' tier and declares "
              f"no hazards. Anything at 'stance' can put him on the floor; "
              f"name it explicitly so the operator approves it knowingly.")
    return m


def load(path: str | Path) -> Manifest:
    p = Path(path)
    try:
        data = json.loads(p.read_text())
    except FileNotFoundError:
        raise ManifestError(f"no manifest at {p}")
    except json.JSONDecodeError as e:
        raise ManifestError(f"{p}: not valid JSON — {e}")
    return parse(data, source=str(p))
