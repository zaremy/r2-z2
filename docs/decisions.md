# Decision log

Newest last. Each entry: what was decided, why, what would reverse it.

---

## D-001 — Initialize in place in `R2Z2/`, project name `r2-z2`
**2026-08-15**

The directory contained only `.DS_Store` and `R2Z2-vault/` (an Obsidian vault
with a single `Welcome.md`). Nothing to protect, no existing repo. Initialized
git here rather than nesting a project subdirectory.

Published as `github.com/zaremy/r2-z2` (private). The brief's working name was
`r2-pet`; renamed to `r2-z2` to match the directory and the vault.

The vault is left untouched and is available as durable KB/governance space;
it is gitignored rather than tracked, so notes there stay local.

*Reversed by:* nothing likely.

---

## D-002 — `reference/` is gitignored; clones are never vendored
**2026-08-15**

Six upstream repos (3800+ files, mixed MIT/Apache-2.0) are research inputs, not
dependencies. Vendoring them would bloat history and blur provenance. Exact SHAs
are pinned in [research/source-map.md](research/source-map.md); the clone
commands are in the root README.

They are kept **inside** the project at `reference/` rather than somewhere
external, so they are one `grep -r` away while working. Moved there from
`research/external/` on 2026-08-15 — same policy, shorter path, and the
now-empty `research/` tree removed so `docs/research/` is the only "research".

*Reversed by:* needing to fork one for real, at which point it becomes a proper
submodule or dependency with its own decision entry.

---

## D-003 — Reimplement the Sphero V2 packet layer rather than depend on `spherov2`
**2026-08-15**

`mac-prototype/r2_probe.py` has no `spherov2` import. Reasons:

1. The Mac prototype exists to teach us what to write in C. A traced,
   commented implementation is the porting artifact; a library dependency is not.
2. `sphero-r2d2` pins `bleak>=1.1.1` and we run 3.0.2 — an untested combination
   we would rather not have on the critical path during bring-up.
3. Writing it forced us to read the protocol properly, which is how the LED bug
   in D-004 was found.

Cost is real: we own the code. Mitigated by cross-validating every packet
against `spherov2` *and* the shipped C firmware — byte-identical across wake,
battery, LED, audio, head float (±), animation play/stop, and an escaping
stress case.

*Reversed by:* needing sensor streaming or drive control quickly, where
`spherov2`'s existing implementation would outweigh the porting benefit.

---

## D-004 — Use the 16-bit LED mask (DID `0x1A`, CID `0x0E`), not the 8-bit variant
**2026-08-15**

The probe initially used CID `0x1C` (8-bit mask). `spherov2/commands/io.py:87`
marks that variant `# Untested / Unknown Param Names`, while `bb9e.py:153`
exposes the **16-bit** version — and R2D2 subclasses BB9E.
`claude-r2d2-buddy/main/translator.c:66,74` uses CID `0x0E` with masks `0x0007`
and `0x0070` on real hardware.

Fixed, and re-verified three ways (ours = `spherov2` = C firmware, byte-exact).

*Reversed by:* hardware showing CID `0x0E` failing on our firmware version.

---

## D-005 — ESP-IDF v5.5.x + official managed BSP as the embedded foundation
**2026-08-15**

Chosen over Arduino/PlatformIO. Full analysis in
[research/embedded-path.md](research/embedded-path.md). It is the only path
where **both** hard parts are already solved by someone else: the working
ESP-IDF/NimBLE R2 BLE central in `claude-r2d2-buddy/main/r2d2_central.c`, and
first-party, revision-aware board support from Waveshare.

The Arduino path would mean rewriting the BLE central in order to reuse a UI —
while inheriting V1 display and touch drivers for a V2 board.

Explicitly *provisional*: the BSP is a managed component whose source we have
not read (it is fetched at build time). If it turns out not to handle the V2
panel cleanly, revisit.

*Reversed by:* the BSP failing on V2 hardware, or Wi-Fi/BLE coexistence proving
unworkable under IDF.

---

## D-006 — Drop the BLE peripheral role; ESP32 is central-only
**2026-08-15**

`claude-r2d2-buddy` is dual-role (Nordic UART peripheral for Claude Desktop +
central for R2), hence `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`. Our backpack is
autonomous and needs only central → R2. Dropping `nus_peripheral.c` and setting
`MAX_CONNECTIONS=1` removes dual-role radio contention before Wi-Fi is added.

Claude-activity awareness, if ever wanted, arrives over Wi-Fi as one sensor
among many — not as a second BLE role.

*Reversed by:* a concrete requirement to pair R2 with a desktop over BLE.

---

## D-007 — Do not flash `vthinkxie` firmware to the board
**2026-08-15**

`src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h:52,55` sets
`BOARD_DISPLAY_CO5300 0` and uses FT3168 @ `0x38` — the **V1** stack. The
expected board is V2 (CO5300 + CST820 @ `0x15`). Flashing it would send an
SH8601 init sequence to a CO5300 panel and probe touch where nothing answers.

Repo remains valuable as a **service-panel architecture reference**
(`src/hw/` HAL split, NVS persistence, battery UI, PSRAM framebuffer).

**Substantially weakened 2026-08-15.** The retailer listing for the unit
actually purchased (Amazon B0DSVK5576) names **SH8601 + FT3168** in its title —
the V1 stack. If that holds, `vthinkxie`'s 1.8" env is the *correct* firmware
for this board and this decision's premise is void.

The decision stands only as "do not flash before the I²C probe confirms the
revision." The direction of the hazard is now unknown, not known.

*Reversed by:* `08_i2c_tools` reporting `0x38` (FT3168) — which the listing
predicts. Re-file this decision with the probe output either way.

---

## D-008 — Project-local `R2Probe.app` instead of patching Homebrew's Python
**2026-08-15**

macOS SIGABRTs any process touching CoreBluetooth whose bundle lacks
`NSBluetoothAlwaysUsageDescription`; Homebrew's `Python.app` lacks it. The
common fix is editing that plist in place, which is global, invisible, and
reverted by `brew upgrade`.

Instead `mac-prototype/tools/R2Probe.app` is a project-local, ad-hoc-signed
copy with the key added, launched by `mac-prototype/r2`, rebuildable via
`./r2 --rebuild`.

Does **not** remove the need for a one-time interactive TCC grant — see
`mac-prototype/README.md`.

*Reversed by:* Homebrew shipping the key upstream.

---

## D-009 — Split the `motion` tier into `dome` and `stance`
**2026-08-16**

The permission ladder topped out at `motion`, documented as "dome and
animations". **That description was false in the direction that matters.**

#11 played one authored animation — id 21 `EMOTE_YES`, a *nod* — at
`--allow motion`. It emitted `WADDLE`, `WADDLE`, `WADDLE`, `TRANSITIONING`,
`TWO_LEGS`, and **put R2 on the floor**, with `perform_leg_action` never
called by us. An authored animation is a stance command whose contents we do
not get to inspect before sending it.

So a session opened to survey the *dome* could change his stance and topple
him, while the ceiling's own name promised it could not. The ladder is this
project's core safety mechanism; a rung that grants more than it says is worse
than no rung, because it is trusted.

**Decision.** `TIERS = ["read", "leds", "audio", "dome", "stance"]`.

- `dome` — `set_head` plus the `DID 0x17` writes that cannot change stance
  (`notify`, `idle`). Bounded by `bounded_head_move`; worst case is a 45°
  turn of the head.
- `stance` — anything that can put him on the floor. `animation` sits here
  **because of the observation**, not by category.

`motion` is accepted as a deprecated alias and resolves **down** to `dome`,
never up. Someone who typed the old name now gets a refusal on `animation` —
a message on a terminal. Resolving it up would silently re-grant the ability
to knock the robot over. **When a rename is ambiguous, resolve toward less
capability.**

The alias is resolved once at daemon start, so the banner, the refusal
messages and the lock file all name the tier actually in force. Printing
`MOTION` while enforcing `dome` is the class of mismatch this removes.

`stance` (the read op, `get_leg_action`) stays at tier `read`: finding out
whether he is stable must never require opening a session that can
destabilise him.

*Reversed by:* evidence that `play_animation` cannot reach the legs — which
would contradict six `leg_action_complete` events already recorded.

*Does not fix:* `set_leg_position` and `perform_leg_action` have no op yet
(#22), and locomotion via `DID 0x16` remains unimplemented (#23). This makes
the ladder honest about what exists today; it does not add the missing rungs.

---

## D-010 — Excitement is choreographed by us; authored animations are not a behavior library
**2026-08-17**

The plan in `architecture.md` assumed semantic behaviors could delegate to
authored animations — `celebrate()` plays `EMOTE_LAUGH`, and so on. The S1d
survey (#12) measured what those animations actually do to the body.

**Of the first 19 ids classified, 13 emit `WADDLE`.** Not as an outlier: it is
the norm. Id 14 `EMOTE_SHORT_CIRCUIT` emitted 12 waddles across 9.1 s.

The survey asserted and **read back** `THREE_LEGS` before every single id.
All 13 waddlers started from tripod, and all 13 **retracted the leg
themselves** and ended in bipod:

```
started from 'three_legs': [2,3,4,5,7,8,9,10,12,13,14,15,21]
ended in    'two_legs':    [2,3,4,5,7,8,9,10,12,13,14,15,21]
```

The operator was supporting him and he still went down three times during the
sweep — twice on ids 14 and 15 consecutively.

**Decision.** Semantic behaviors compose dome + sound + light + explicit
stance themselves. They do **not** call `play_animation` for anything
emotional. The authored library stays available for a *supported* or
*seated* R2 and as a source of timing reference, but it is not the vocabulary
the character is built from.

Two rules follow, both from the operator's own framing:

1. **Tripod down whenever he is "active".** Any behavior with energy in it
   deploys the stabiliser first and holds it for the duration. Stability is a
   precondition of expression, not a reaction to losing it.
2. **The mechanical leg sound belongs ON the deployment.** That sound is
   `R2_MOTOR = 2970` (`r2_assets.py:152`, `r2d2.py:315`), surveyed in S1b as
   *"long mechanical lift with clocklike ticking"* and the longest clip in the
   set — it is literally the sound of a leg coming down. Fire it synchronised
   with the leg moving, not after speech with the leg already down; the sound
   is the deployment's own voice. Several authored animations get this
   ordering wrong, which is part of why they read as canned. Deployment
   settles in 2.28–2.56 s (#22), so the clip has room to run underneath it.

**Pre-deploying the tripod does not make an authored animation safe.** That is
the finding, and it is the reason this is a decision rather than a guideline:
an animation overrides whatever stance we set, so the only control available
is not playing it.

*Reversed by:* a way to inspect an animation's leg track before playing it,
or a per-animation stance lock in firmware. Neither is known to exist.

*Does not fix:* ids 18-55 are unclassified, so the 13 is a floor, not a total.
`WWM_*` (31-54) is entirely unmeasured and is the set `architecture.md` leans
on hardest.

---

## Open — to be decided on hardware

- **Animation ID table.** `spherov2` and `claude-r2d2-buddy` disagree
  (1 of 7 overlapping entries agree). Blocks the semantic behavior library.
  Resolve by surveying ids 0-55 on our unit. Also probe the gaps: 20, 23, 28,
  29, 30.
- **`enable_idle_animations` on or off.** Off during bring-up so every motion is
  attributable in logs; re-evaluate as a feature afterwards.
- **Board revision — confirm, do not assume.** Record `00_board_check` and
  `08_i2c_tools` output here with a date.
- **Flash size** — 8 MB (`vthinkxie`'s claim) vs 16 MB (Waveshare's
  `sdkconfig.defaults`). Affects partitioning and OTA.
- **Persistence media** — NVS / LittleFS / microSD split.
- **Keepalive period** — 3 s is inherited, not measured.
- **Backpack power and mounting.** Entirely unaddressed and not a software
  problem.
