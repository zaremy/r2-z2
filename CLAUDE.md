# r2-z2 — persistent AI mind for a Sphero R2-D2

## Goal

Replace the obsolete Sphero app brain with a persistent, autonomous AI mind.
Success is *not* a robot that obeys commands; it is the sense that the same
little entity is continuously present in the household.

## Character boundary — non-negotiable

- **R2's physical body is the character interface.** Dome, locomotion, stance,
  chirps, authored animations, front/rear LEDs, logic/holo lights, idle motion.
- **The backpack touchscreen is a service/debug/settings panel.** Status,
  connectivity, diagnostics, hardware tests, provisioning. It is *not* a face.
  Do not add character rendering to the screen without an explicit decision
  recorded in `docs/decisions.md`.

## Architecture rules

- Persistent state (identity, mood, energy, boredom, relationship, memories,
  goals) lives in **our** store and survives swapping model providers.
- **LLMs are a reasoning service, not the identity.** Never hardwire the
  character to Claude or any vendor.
- **Routine life runs locally.** Idle, sleep/wake, mood drift, boredom,
  quiet hours, simple sensor reactions — no cloud round-trip.
- **Semantic behaviors above raw primitives.** The reasoning layer calls
  `express_curious()`, not `set_dome_position(-27)`. Each semantic behavior
  choreographs dome + sound + light + stance over time.
- Keep the brain decoupled from R2 BLE packets. Layer boundary:
  `brain → behavior → choreography → r2/commands → r2/protocol → r2/ble`.

## Hardware

- Sphero R2-D2 app-enabled droid (BLE name prefix `D2-`).
- Waveshare ESP32-S3-Touch-AMOLED-1.8. **Revision must be verified at runtime,
  not trusted from the retailer listing** — see `docs/research/board-revision.md`.
- Mac is the development/prototyping host.
- **No Raspberry Pi** unless a concrete, documented limitation forces one.

## Safety

- Never commit API keys or Wi-Fi credentials. Use `.env.example` and NVS.
- Never issue uncontrolled movement commands during development.
- Bring-up order is fixed: **read-only → LEDs → audio → small dome → stance →
  locomotion.** Each actuator test is individually opt-in, never bundled.
- Default to STOP. Disconnect or failure must result in stop, not last-command.
- Do not leave animation or sound loops running unattended.
- **Do not flash firmware built for another board revision.** Confirm the
  display/touch controller pair before flashing anything third-party.

## Hardware session facts that cost time to learn

- **The agent cannot start the BLE daemon.** macOS attributes Bluetooth to the
  *responsible* process; under Claude Code that is `claude.app`, which has no
  usage description, so anything it spawns that touches CoreBluetooth is
  SIGABRTed (exit 134, verified). Workaround that works: write a `.command`
  file and `open -a Terminal` it — launchd spawns it and Terminal is
  responsible. Always `ps aux | grep r2_probe` first; a stale daemon holds the
  link and the new one fails with a misleading "R2-D2 not found".
- **The dome has no resting position.** Observed at 103°, 3.3° and −0.06° in
  three sessions. `set_head_position` is absolute, so bound moves by *travel*
  from a freshly read position — never by destination, never from a remembered
  angle.
- **`spherov2` capability lookups must go through the MRO, and are still only
  claims.** `class R2D2(BB9E)`, so grepping `r2d2.py`'s class body misses
  everything inherited. And a correctly-resolved capability can still be
  refused by the firmware — `enable_idle_animations` is the standing
  counterexample. Presence means "worth probing", not "supported".
- **Running a survey where a human is the instrument?** Use `/survey-session`.
  Brief before firing, fire within ~3 s of "go", and never suppress stderr on a
  send loop — a silent crash reads exactly like a dead device.

## Research discipline

- **Read source, not README.** A README claim is a hypothesis.
- Record commit SHAs for every external repo (`docs/research/source-map.md`).
- Label every claim: `OBSERVED` / `INFERRED` / `UNKNOWN` / `SPECULATION`.
  Do not let an INFERRED claim silently graduate to OBSERVED.
- Prefer current official hardware code over blog/forum assumptions.
- When two sources disagree, record the disagreement rather than picking
  silently. Two known live conflicts are tracked in `r2-capabilities.md`.
- Cross-check protocol constants against at least two independent
  implementations before trusting them. The packet layer in
  `mac-prototype/r2_probe.py` was validated three ways (spherov2, the
  `claude-r2d2-buddy` C firmware, and a clean-room trace) — keep that bar.

## Where things are written

Two homes, and the split is not negotiable — putting a doc in the wrong one is
how plans go stale and decisions get lost.

**Obsidian vault — `R2Z2-vault/R2Z2-vault/` (gitignored, local):** planning and
thinking. The roadmap and per-stage notes, session logs, raw experiment results
from hardware sessions, open questions, behavior designs in progress, hardware
quirk notes, upstream reading notes.

**Repo `docs/` (committed, PR-reviewed):** things that must survive a fresh
clone and travel with the code. `intent.md`, `architecture.md`, `decisions.md`,
and `research/*.md` findings cited to `file:line`.

> **Rule of thumb: if it cites code, it goes in the repo. If it is thinking, it
> goes in the vault.** When a vault note hardens into a decision, write the ADR
> in `docs/decisions.md` — the vault note is the reasoning, the ADR is the ruling.

Consequence to accept knowingly: vault content is **not** in git, so it is not
backed up by the remote and not reviewable in a PR. That is the deliberate
trade for a fast, linkable thinking space.

### Writing for Obsidian

Vault notes are Obsidian-native, not plain markdown dropped in a folder:

- **YAML frontmatter on every note** — `type`, `status`, `updated`, `tags`.
  Keep `updated` honest; it is the only staleness signal.
- **`[[Wikilinks]]`, not relative paths.** Backlinks and the graph are the
  point. A link to a note that does not exist yet is fine — it marks work.
- **Exit criteria and checklists are `- [ ]` tasks**, so progress is visible and
  searchable rather than prose to re-read.
- **Hierarchical tags** — `#stage/s1`, `#status/blocked`, `#track/robot`.
- **Callouts** (`> [!warning]`, `> [!info]`) for the things that bite.
- Conventions live in `Meta/Vault Conventions.md`; templates in `Templates/`.
  Only core plugins are enabled — do not write notes that require Dataview.

## Working agreements

- `reference/` holds the upstream clones and is gitignored. Never vendor a
  clone into our history.
- Prefer adding a project-side tool over patching an external clone.
- Prototype in Python on the Mac; do **not** assume Python code ports to the
  ESP32. The Mac layer exists to learn behavior abstractions, not to be moved.
