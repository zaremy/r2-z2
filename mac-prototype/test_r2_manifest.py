"""Tests for the session manifest (#88).

The manifest exists to spend the operator's attention ONCE instead of four
times, and the danger in anything that reduces approvals is that it also
reduces authorisation. So the refusals come first and outnumber the rest: this
file is mostly about what a manifest CANNOT do.
"""
import json
import tempfile
import unittest
from pathlib import Path

import r2_manifest as M
import r2_probe as P


def m(**over):
    base = {"title": "t", "ops": [{"op": "battery"}]}
    base.update(over)
    return M.parse(base)


class TestTheTierIsComputedNotDeclared(unittest.TestCase):
    """AC1. The whole point. A manifest whose ops need `dome` cannot launch at
    `leds`, and cannot be hand-annotated to claim otherwise."""

    def test_the_tier_is_the_highest_any_op_needs(self):
        self.assertEqual(m(ops=[{"op": "battery"}]).required_tier, "read")
        self.assertEqual(m(ops=[{"op": "battery"}, {"op": "leds"}]).required_tier, "leds")
        self.assertEqual(
            m(ops=[{"op": "battery"}, {"op": "leds"}, {"op": "dome"}]).required_tier,
            "dome")
        # Order must not matter: the highest wins wherever it sits.
        self.assertEqual(
            m(ops=[{"op": "dome"}, {"op": "battery"}]).required_tier, "dome")

    def test_a_manifest_may_not_declare_its_own_tier(self):
        """Refused, not ignored. A silently-dropped field is indistinguishable
        from an honoured one, so the file would read as though the ceiling it
        names had been granted."""
        for key in ("tier", "ceiling", "allow", "max_tier", "required_tier"):
            with self.subTest(key=key):
                with self.assertRaises(M.ManifestError) as cm:
                    M.parse({"title": "t", key: "stance",
                             "ops": [{"op": "battery"}]})
                self.assertIn("COMPUTED", str(cm.exception))

    def test_the_tier_cannot_be_set_after_parsing(self):
        """No setter, no backing field. If `required_tier` were a stored value
        rather than a computation, everything above would still pass and the
        value could be overwritten between approval and use."""
        man = m(ops=[{"op": "battery"}])
        with self.assertRaises(Exception):
            man.required_tier = "stance"
        self.assertEqual(man.required_tier, "read")

    def test_an_unknown_op_is_refused_rather_than_skipped(self):
        """Skipping it would compute the ceiling from the ops it recognised —
        a guess presented as a computation."""
        with self.assertRaises(M.ManifestError) as cm:
            M.parse({"title": "t", "ops": [{"op": "battery"},
                                           {"op": "definitely_not_an_op"}]})
        self.assertIn("unknown op", str(cm.exception))


class TestAManifestCanOnlyNarrow(unittest.TestCase):
    """AC7. The manifest is a ceiling DECLARATION, not an exemption."""

    def test_membership_refuses_an_op_not_listed(self):
        man = m(ops=[{"op": "battery"}, {"op": "leds"}])
        self.assertTrue(man.permits("battery"))
        self.assertFalse(man.permits("dome"))
        self.assertFalse(man.permits("animation"))

    def test_the_refusal_names_what_WAS_permitted(self):
        """AC2. A refusal that only says 'no' makes the operator go and read
        the manifest file to find out what they approved."""
        man = m(title="LED survey", ops=[{"op": "battery"}, {"op": "leds"}])
        r = man.refusal("animation")
        self.assertFalse(r["ok"])
        self.assertEqual(r["manifest"]["ops"], ["battery", "leds"])
        self.assertEqual(r["manifest"]["tier"], "leds")
        self.assertEqual(r["manifest"]["title"], "LED survey")

    def test_every_op_in_a_manifest_is_still_subject_to_the_ladder(self):
        """The manifest never grants. For every op it can name, the daemon's
        own tier check must still be the thing that decides — so a manifest
        listing an op is NEVER sufficient on its own."""
        for op in P.OPS:
            man = m(title="t", ops=[{"op": op}],
                    hazards=["stance tier"] if P.op_tier(op) == "stance" else [])
            needed = P.op_tier(op)
            # The manifest's tier IS the op's tier — it cannot be lower, which
            # is what an exemption would look like.
            self.assertEqual(man.required_tier, needed, op)
            # And the ladder agrees the op is permitted at exactly that tier
            # and refused below it.
            self.assertTrue(P._tier_ok(needed, needed), op)
            below = P.TIERS.index(needed) - 1
            if below >= 0:
                self.assertFalse(P._tier_ok(P.TIERS[below], needed), op)

    def test_stop_is_permitted_even_when_a_manifest_forgets_it(self):
        """DEFAULT TO STOP. Refusing `stop` is the one refusal that can leave
        him MOVING, so a manifest that forgot to list it would make the session
        more dangerous than no manifest at all."""
        man = m(ops=[{"op": "battery"}])
        self.assertFalse(man.permits("dome"))
        self.assertTrue(man.permits("stop"))
        # And it cannot raise the ceiling: it is `read` tier.
        self.assertEqual(P.op_tier("stop"), "read")
        self.assertEqual(man.required_tier, "read")
        # The exemption is exactly one op, not a category.
        self.assertEqual(M.ALWAYS_PERMITTED, frozenset({"stop"}))
        for op in P.OPS:
            if op != "stop":
                self.assertFalse(m(ops=[{"op": "battery"}]).permits(op)
                                 and op not in ("battery",), op)

    def test_an_empty_manifest_is_refused(self):
        """A manifest permitting nothing is not a thing anyone means to
        approve; it is a file somebody forgot to finish."""
        with self.assertRaises(M.ManifestError):
            M.parse({"title": "t", "ops": []})


class TestTheOperatorSeesWhatTheyApprove(unittest.TestCase):
    """AC4. The plan is legible BEFORE approval — that is strictly better than
    today, where a ceiling is approved with no idea what will use it."""

    def test_the_description_carries_every_op_and_its_tier(self):
        man = m(title="LED + dome", ops=[{"op": "battery"}, {"op": "leds"},
                                         {"op": "dome"}])
        out = man.describe()
        for op in ("battery", "leds", "dome"):
            self.assertIn(op, out)
        self.assertIn("dome", out)
        self.assertIn("computed from the ops", out)

    def test_hazards_are_shown(self):
        man = m(ops=[{"op": "animation", "params": {"id": 8}}],
                hazards=["animation 8 drives leg actions and can fell him"])
        self.assertIn("HAZARDS", man.describe())
        self.assertIn("fell him", man.describe())

    def test_reaching_stance_without_naming_a_hazard_is_refused(self):
        """`stance` is the tier that can put him on the floor, and an authored
        animation is a stance command whose contents we do not get to inspect
        first (#11, D-009). Approving that without the word in front of you is
        the failure this file exists to prevent."""
        with self.assertRaises(M.ManifestError) as cm:
            M.parse({"title": "t", "ops": [{"op": "animation"}]})
        self.assertIn("stance", str(cm.exception))
        # ...and it is allowed once the hazard is named.
        man = M.parse({"title": "t", "ops": [{"op": "animation"}],
                       "hazards": ["can topple him"]})
        self.assertEqual(man.required_tier, "stance")

    def test_a_typo_in_a_key_is_refused_not_ignored(self):
        """A dropped 'hazzards' means the operator approves a plan whose
        hazards were never shown to them."""
        with self.assertRaises(M.ManifestError) as cm:
            M.parse({"title": "t", "ops": [{"op": "battery"}],
                     "hazzards": ["this would vanish"]})
        self.assertIn("hazzards", str(cm.exception))


class TestParsing(unittest.TestCase):
    def test_rejects_a_non_object(self):
        for bad in ([1, 2], "hi", 42, None):
            with self.assertRaises(M.ManifestError):
                M.parse(bad)

    def test_requires_a_title(self):
        for bad in (None, "", "   ", 7):
            with self.assertRaises(M.ManifestError):
                M.parse({"title": bad, "ops": [{"op": "battery"}]})

    def test_rejects_a_boolean_duration(self):
        """True is an int in Python and would sail through a naive check."""
        with self.assertRaises(M.ManifestError):
            M.parse({"title": "t", "ops": [{"op": "battery"}], "expected_s": True})

    def test_rejects_a_negative_duration(self):
        with self.assertRaises(M.ManifestError):
            M.parse({"title": "t", "ops": [{"op": "battery"}], "expected_s": -1})

    def test_load_reports_the_path_on_bad_json(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.json"
            p.write_text("{not json")
            with self.assertRaises(M.ManifestError) as cm:
                M.load(p)
            self.assertIn(str(p), str(cm.exception))

    def test_load_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.json"
            p.write_text(json.dumps({"title": "round trip",
                                     "ops": [{"op": "battery"}, {"op": "leds"}]}))
            man = M.load(p)
            self.assertEqual(man.title, "round trip")
            self.assertEqual(man.required_tier, "leds")

    def test_a_missing_file_is_a_manifest_error(self):
        with self.assertRaises(M.ManifestError):
            M.load("/nope/nothing/here.json")


class TestOneLaunchCoversAWholeSession(unittest.TestCase):
    """AC3. A session that today needs read -> leds -> dome is three
    relaunches. One manifest, one approval, one ceiling."""

    def test_three_tiers_one_ceiling(self):
        man = M.parse({
            "title": "S1g light language",
            "ops": [{"op": "battery"}, {"op": "leds"}, {"op": "dome"}],
            "teardown": "leds off, status asserted",
        })
        self.assertEqual(man.required_tier, "dome")
        for op in ("battery", "leds", "dome"):
            self.assertTrue(man.permits(op))
            self.assertTrue(P._tier_ok(man.required_tier, P.op_tier(op)))
        # And nothing above it comes along for the ride.
        self.assertFalse(man.permits("animation"))
        self.assertFalse(P._tier_ok(man.required_tier, P.op_tier("animation")))


if __name__ == "__main__":
    unittest.main()
