#!/usr/bin/env python3
"""Tests for r2_survey.py — issue #6 acceptance criteria.

All run WITHOUT hardware and WITHOUT a running daemon. Plain stdlib unittest so
there is no test-runner dependency to install before the survey can start.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_survey as S

VALID_OBS = {"outcome": "played", "energy_cost_class": "low",
             "wear_class": "none", "recommended_cooldown_s": 0}


class TempSurvey(unittest.TestCase):
    """Redirect the survey dir at module level so tests never touch .survey/."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        d = Path(self._tmp.name)
        self._saved = (S.SURVEY_DIR, S.MANIFEST, S.ATTEMPTS, S.STATE)
        S.SURVEY_DIR, S.MANIFEST = d, d / "manifest.json"
        S.ATTEMPTS, S.STATE = d / "attempts.jsonl", d / "state.json"

    def tearDown(self):
        S.SURVEY_DIR, S.MANIFEST, S.ATTEMPTS, S.STATE = self._saved
        self._tmp.cleanup()

    def make_manifest(self, tier=None):
        return S.main(["manifest"] + (["--tier", tier] if tier else []))


class TestManifest(TempSurvey):
    """AC1: exactly 104 items, byte-identical across runs."""

    def test_item_count_is_104(self):
        items = S.build_manifest()
        self.assertEqual(len(items), 104)
        kinds = {}
        for i in items:
            kinds[i["kind"]] = kinds.get(i["kind"], 0) + 1
        self.assertEqual(kinds, {"led": 8, "sound": 40, "animation": 56})

    def test_sound_sampling_matches_derivation(self):
        """19 families: 8 singletons, R2_SCREAM has 2, 10 have >=3 -> 40."""
        picked = S.sampled_sounds()
        self.assertEqual(len(picked), 40)
        per_family = {}
        for name, _ in picked:
            f = S.family_of(name)
            per_family[f] = per_family.get(f, 0) + 1
        self.assertEqual(len(per_family), 19)
        self.assertEqual(sum(1 for v in per_family.values() if v == 1), 8)
        self.assertEqual(per_family["SCREAM"], 2)
        self.assertEqual(sum(1 for v in per_family.values() if v == 3), 10)

    def test_all_animation_ids_0_to_55_including_gaps(self):
        ids = {int(i["item_id"].split(":")[1]) for i in S.build_manifest()
               if i["kind"] == "animation"}
        self.assertEqual(ids, set(range(0, 56)))
        by_id = {i["item_id"]: i for i in S.build_manifest()}
        for gap in (20, 23, 28, 29, 30):
            self.assertIn("unnamed", by_id[f"anim:{gap}"]["label"])

    def test_driving_animations_flagged(self):
        """translator.c:101-104 documents 8/9/11 as moving the body."""
        by_id = {i["item_id"]: i for i in S.build_manifest()}
        for aid in (8, 9, 11):
            self.assertTrue(by_id[f"anim:{aid}"]["may_drive"], f"anim:{aid}")
        self.assertFalse(by_id["anim:35"].get("may_drive"))

    def test_manifest_is_deterministic(self):
        a, b = S.build_manifest(), S.build_manifest()
        self.assertEqual(S.manifest_digest(a), S.manifest_digest(b))
        self.make_manifest()
        first = S.MANIFEST.read_bytes()
        self.make_manifest()
        self.assertEqual(first, S.MANIFEST.read_bytes(), "manifest not byte-identical")

    def test_tier_filter_partitions_the_manifest(self):
        total = 0
        for tier in S.TIERS:
            self.make_manifest(tier)
            n = json.loads(S.MANIFEST.read_text())["count"]
            total += n
        self.assertEqual(total, 104)


class TestSchema(TempSurvey):
    """AC5: schema-invalid observations are rejected."""

    def test_valid_passes(self):
        S.validate_observation(dict(VALID_OBS))

    def test_missing_required_field_rejected(self):
        for field in S.REQUIRED_FIELDS:
            obs = dict(VALID_OBS)
            del obs[field]
            with self.assertRaises(S.SurveyError, msg=field):
                S.validate_observation(obs)

    def test_bad_enum_values_rejected(self):
        for field, bad in (("outcome", "worked"), ("energy_cost_class", "huge"),
                           ("wear_class", "wheels")):
            obs = dict(VALID_OBS, **{field: bad})
            with self.assertRaises(S.SurveyError, msg=field):
                S.validate_observation(obs)

    def test_cooldown_must_be_numeric(self):
        with self.assertRaises(S.SurveyError):
            S.validate_observation(dict(VALID_OBS, recommended_cooldown_s="soon"))


class TestRecord(TempSurvey):
    def test_unknown_item_id_exits_nonzero(self):
        self.make_manifest()
        self.assertEqual(S.main(["record", "anim:999", "--json", json.dumps(VALID_OBS)]), 2)

    def test_invalid_observation_exits_nonzero(self):
        self.make_manifest()
        bad = json.dumps(dict(VALID_OBS, outcome="nope"))
        self.assertEqual(S.main(["record", "anim:7", "--json", bad]), 2)

    def test_duplicate_record_rejected(self):
        self.make_manifest()
        obs = json.dumps(VALID_OBS)
        self.assertEqual(S.main(["record", "anim:7", "--json", obs]), 0)
        self.assertEqual(S.main(["record", "anim:7", "--json", obs]), 2)

    def test_amend_supersedes_without_destroying_history(self):
        self.make_manifest()
        S.main(["record", "anim:7", "--json", json.dumps(VALID_OBS)])
        amended = json.dumps(dict(VALID_OBS, outcome="no_effect"))
        self.assertEqual(S.main(["record", "--amend", "anim:7#1",
                                 "--reason", "misheard", "--json", amended]), 0)
        attempts = S.load_attempts(warn=lambda m: None)
        self.assertEqual(len(attempts), 2, "original must be retained")
        state = S.derive_state(attempts)
        obs = [a for a in state["items"]["anim:7"]["attempts"]][-1]["observation"]
        self.assertEqual(obs["outcome"], "no_effect")

    def test_amend_requires_reason(self):
        self.make_manifest()
        S.main(["record", "anim:7", "--json", json.dumps(VALID_OBS)])
        self.assertEqual(S.main(["record", "--amend", "anim:7#1"]), 2)


class TestResumeAndDurability(TempSurvey):
    """AC3 resume, AC7 durability."""

    def test_next_does_not_replay_observed_items(self):
        self.make_manifest("motion")
        S.main(["record", "anim:0", "--json", json.dumps(VALID_OBS)])
        state = S.derive_state(S.load_attempts(warn=lambda m: None))
        self.assertTrue(state["items"]["anim:0"]["done"])
        items = S.load_manifest()
        pending = [i for i in items
                   if not state["items"].get(i["item_id"], {}).get("done")]
        self.assertNotIn("anim:0", [i["item_id"] for i in pending])
        self.assertEqual(len(pending), 55)

    def test_state_cache_rebuilds_identically(self):
        self.make_manifest()
        for aid in (0, 1, 2):
            S.main(["record", f"anim:{aid}", "--json", json.dumps(VALID_OBS)])
        before = S.STATE.read_bytes()
        S.STATE.unlink()
        self.assertEqual(S.main(["rebuild"]), 0)
        self.assertEqual(before, S.STATE.read_bytes(),
                         "state.json must be fully derived from attempts.jsonl")

    def test_truncated_final_line_is_discarded_with_warning(self):
        self.make_manifest()
        S.main(["record", "anim:0", "--json", json.dumps(VALID_OBS)])
        S.main(["record", "anim:1", "--json", json.dumps(VALID_OBS)])
        with open(S.ATTEMPTS, "a") as f:
            f.write('{"attempt_id": "anim:2#1", "item_id": "anim:2", "sta')
        warnings = []
        attempts = S.load_attempts(warn=warnings.append)
        self.assertEqual(len(attempts), 2, "the two good records must survive")
        self.assertTrue(any("truncated" in w for w in warnings), warnings)

    def test_sigkill_during_record_loses_at_most_the_inflight_attempt(self):
        """Real SIGKILL, real child process. Tests may spawn; r2_survey.py may not."""
        self.make_manifest()
        S.main(["record", "anim:0", "--json", json.dumps(VALID_OBS)])
        good = S.ATTEMPTS.read_text()

        script = (
            "import sys, time, json;"
            f"sys.path.insert(0, {str(Path(__file__).parent)!r});"
            "import r2_survey as S;"
            f"S.SURVEY_DIR = __import__('pathlib').Path({str(S.SURVEY_DIR)!r});"
            "S.ATTEMPTS = S.SURVEY_DIR / 'attempts.jsonl';"
            "S.append_attempt({'attempt_id':'anim:1#1','item_id':'anim:1',"
            "'state':'observed_complete','ts':1.0,'observation':{}});"
            "print('done', flush=True); time.sleep(30)"
        )
        proc = subprocess.Popen([sys.executable, "-c", script],
                                stdout=subprocess.PIPE, text=True)
        proc.stdout.readline()          # wait until the append has been fsync'd
        proc.send_signal(signal.SIGKILL)
        proc.wait(timeout=10)

        attempts = S.load_attempts(warn=lambda m: None)
        self.assertEqual(len(attempts), 2, "fsync'd append must survive SIGKILL")
        self.assertTrue(S.ATTEMPTS.read_text().startswith(good),
                        "existing records must not be corrupted")


class TestExportGate(TempSurvey):
    """AC6 parseable table, AC8 export blocked while attempts are unresolved."""

    def test_export_blocked_by_unresolved_attempt(self):
        self.make_manifest("motion")
        S.main(["record", "anim:0", "--json", json.dumps(VALID_OBS)])
        self.assertEqual(S.main(["resolve", "anim:1", "--state", "aborted_unknown"]), 0,
                         "resolve must work on an item with no prior attempt")
        self.assertEqual(S.main(["export"]), 2)

    def test_export_table_reparses_with_fixed_columns(self):
        self.make_manifest("leds")
        for bit in range(8):
            S.main(["record", f"led:{bit}", "--json", json.dumps(
                dict(VALID_OBS, lights=f"channel {bit} lit", reads_as="n/a"))])
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = S.main(["export"])
        self.assertEqual(rc, 0)
        lines = [l for l in buf.getvalue().strip().split("\n") if l.startswith("|")]
        header = [c.strip() for c in lines[0].strip("|").split("|")]
        self.assertEqual(header, S.EXPORT_COLUMNS)
        body = lines[2:]
        self.assertEqual(len(body), 8)
        for row in body:
            self.assertEqual(len([c for c in row.strip("|").split("|")]),
                             len(S.EXPORT_COLUMNS), row)


class TestUnsafeReplayGate(TempSurvey):
    """AC9: next refuses to re-emit an item held at unsafe_replay_review."""

    def test_next_refuses_unsafe_replay_item(self):
        self.make_manifest("motion")
        self.assertEqual(S.main(["resolve", "anim:8", "--state", "unsafe_replay_review",
                                 "--reason", "aborted mid-drive"]), 0)
        import io, contextlib
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            S.main(["next", "--count", "60"])
        self.assertNotIn("anim:8", out.getvalue(), "must not be emitted as runnable")
        self.assertIn("unsafe_replay_review", err.getvalue())


class TestResolve(TempSurvey):
    def test_resolve_works_with_no_prior_attempt(self):
        """The session-abort path: fired but never recorded."""
        self.make_manifest("motion")
        self.assertEqual(S.main(["resolve", "anim:5", "--state", "aborted_unknown"]), 0)
        st = S.derive_state(S.load_attempts(warn=lambda m: None))["items"]["anim:5"]
        self.assertEqual(st["state"], "aborted_unknown")
        self.assertFalse(st["done"])

    def test_resolve_rejects_unknown_item(self):
        self.make_manifest("motion")
        self.assertEqual(S.main(["resolve", "anim:999", "--state", "aborted_unknown"]), 2)

    def test_resolve_will_not_silently_undo_a_completed_observation(self):
        self.make_manifest("motion")
        S.main(["record", "anim:0", "--json", json.dumps(VALID_OBS)])
        self.assertEqual(S.main(["resolve", "anim:0", "--state", "needs_reobserve"]), 2)


class TestCannotFireCommands(TempSurvey):
    """AC2 — the safety-critical one.

    A source grep for bleak/REQ_DIR/cmd_send is necessary but NOT sufficient: it
    would pass a module that shells out via subprocess. So this asserts
    behaviourally that every command still works with every process- and
    socket-spawning primitive sabotaged."""

    SABOTAGE = [(subprocess, "run"), (subprocess, "Popen"),
                (subprocess, "call"), (subprocess, "check_output"),
                (os, "system"), (os, "popen"), (os, "execv"), (os, "fork")]

    def test_source_has_no_bridge_or_ble_references(self):
        src = Path(S.__file__).read_text()
        body = src.split('"""', 2)[2]          # skip the module docstring
        for forbidden in ("bleak", "REQ_DIR", "RESP_DIR", "cmd_send", "BleakClient"):
            self.assertNotIn(forbidden, body, f"r2_survey.py references {forbidden}")

    def test_every_command_works_with_process_spawning_sabotaged(self):
        import socket as socket_mod

        def boom(*a, **k):
            raise AssertionError("r2_survey.py tried to spawn a process or socket")

        saved = [(m, n, getattr(m, n)) for m, n in self.SABOTAGE if hasattr(m, n)]
        saved.append((socket_mod, "socket", socket_mod.socket))
        for mod, name, _ in saved:
            setattr(mod, name, boom)
        try:
            self.assertEqual(S.main(["manifest"]), 0)
            self.assertEqual(S.main(["next", "--count", "3"]), 0)
            self.assertEqual(S.main(["record", "anim:0", "--json",
                                     json.dumps(VALID_OBS)]), 0)
            self.assertEqual(S.main(["status"]), 0)
            self.assertEqual(S.main(["rebuild"]), 0)
            self.assertEqual(S.main(["export"]), 0)
        finally:
            for mod, name, orig in saved:
                setattr(mod, name, orig)

    def test_next_emits_commands_it_does_not_run_them(self):
        self.make_manifest("leds")
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            S.main(["next", "--count", "2"])
        out = buf.getvalue()
        self.assertIn("./r2 send leds", out, "should print the command")
        self.assertIn("ONE AT A TIME", out, "should tell the operator to pace")
        self.assertFalse(S.SURVEY_DIR.joinpath("..", ".bridge").resolve().exists()
                         and any((S.SURVEY_DIR / ".." / ".bridge" / "requests")
                                 .resolve().glob("*.json")),
                         "next must not enqueue bridge requests")


if __name__ == "__main__":
    unittest.main(verbosity=2)
