---
name: spec
description: R2Z2's /spec. Author a backlog-ready spec or issue for this repo. Use when asked to "spec this out", "file an issue", "write up a ticket", or to interview the operator about work to be built. Runs an R2Z2 preflight (active plan, panel spec, origin/main, other worktrees), then the global /spec.
---

# /spec for R2Z2: preflight, then the global skill

This wraps the global `/spec`. It adds one step it cannot know about: in this
repo, most interview answers are already written down somewhere, and other
sessions are often building the thing you are about to spec.

**Why it exists (2026-09-18):** an E2E v0 interview asked the operator about a
slice another session had merged that day, then about screen and wake-word
details the vault plan and the panel spec already answered, while a third
session was building the next slice in its own worktree. The operator: *"this
is all spec'd out. find the specs"*. Memory: `survey-the-tree-before-you-spec`.

## Step 0: preflight (before the first question)

Run these, and read what they point at:

```bash
git fetch -q && git log --oneline -8 origin/main     # not the local branch; it lags
git worktree list                                    # in-flight branches of other sessions
git status --porcelain                               # untracked work in this checkout
```

Then read:

1. **The active goal:** the vault `R2Z2-vault/R2Z2-vault/Plan/` note the roadmap
   points to (today `Plan/E2E v0.md`). Its checkboxes, rulings and round notes
   are operator decisions. Do not re-ask them.
2. **The panel spec** for anything the operator sees on the backpack:
   `R2Z2-vault/R2Z2-vault/Prototypes/README.md` and `panel-v5-interactive.html`.
3. **The normative docs** the work touches: `docs/behaviour-states.md` (the only
   source of states, D-017), `docs/decisions.md` on `origin/main`, and `CLAUDE.md`
   Safety.

Stop and report, rather than interview, when:

- the work is already merged on `origin/main`, or
- another worktree's branch is building it (name the branch and its uncommitted
  files, and do not spec over it), or
- the plan already rules on what you were going to ask.

## Step 1: run the global /spec

Read `~/.claude/skills/spec/SKILL.md` and follow it from its Preamble and Phase 1,
with these R2Z2 overrides:

- **Ask only what Step 0 did not answer.** A question whose answer is in the plan,
  the panel spec or an ADR is a question you should have read, not asked.
- **One plain question at a time**, through `AskUserQuestion`. No numbered prose
  batches.
- **Honour** `CLAUDE.md` Safety (bring-up order, default to STOP) and the vault/repo
  split: the spec's thinking goes to the vault, the filed issue to GitHub.
- When the operator's answer contradicts an earlier ruling in the plan, surface the
  conflict with both rulings quoted before drafting.
