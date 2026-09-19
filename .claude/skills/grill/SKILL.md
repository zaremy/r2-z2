---
name: grill
description: R2Z2's /grill. Adversarial review-to-convergence of a plan, spec or diff in this repo with an independent reviewer model. Use when asked to "grill", "harden this plan", "review loop", or to drive an artifact to zero meaningful defects. Adds R2Z2 reviewer-brief rules, then runs the global /grill.
---

# /grill for R2Z2: brief rules, then the global skill

This wraps the global `/grill`. Read `~/.claude/skills/grill/SKILL.md` and follow
its loop exactly, with the R2Z2 rules below added to every reviewer brief.

## Why it exists (2026-09-19)

Grilling the slice 3 plan, codex round 3 reported *"D-030 does not exist"* as a
finding. D-030 had merged in #191; the shared checkout was simply behind
`origin/main`, and codex reads the primary checkout's files. The false positive was
caught only by checking it by hand.

## Add to every brief

1. **The local checkout may lag `origin/main`.** Tell the reviewer to read repo
   files with `git show origin/main:<path>` (or `origin/<branch>:<path>` for a PR),
   and never to report "X does not exist" without checking that ref. Run
   `git fetch -q` before the round.
2. **Worktree code is invisible to codex** (it resolves to the primary checkout).
   For a branch in a linked worktree, write `git diff origin/main origin/<branch>`
   to a file in the scratchpad and point the reviewer at it plus `git show`.
3. **Write the brief to the session scratchpad, not `/tmp/grill-brief.md`.**
   Other sessions use that path concurrently, and overwriting theirs is a real risk.
4. **The axes this repo's grills keep needing**, beyond correctness: premise and
   order against `CLAUDE.md`'s bring-up order; falsifiability, meaning which surface
   proves each step (host test, board or droid); failure paths (link loss, STOP,
   release, timeouts); and CX on both surfaces, the glass and his body.
5. **Codex model:** on this account `gpt-5.4` is refused ("not supported when using
   Codex with a ChatGPT account"), so the "busy" downgrade is unavailable. Use
   `gpt-5.5` with the 7-minute timeout.

## Triage rule

Before applying a finding that says something is missing or false in the repo,
verify it against `origin/main` yourself. Record a rejected finding in the round
note as a stale-checkout (or plan-vs-code) false positive, with the reason.
