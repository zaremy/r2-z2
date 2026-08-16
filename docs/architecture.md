# Architecture

Working document. Shaped by what the source archaeology actually found — see
`research/`. Structure is expected to evolve; the *boundaries* are not.

## Target system

```
                        INTERNET
                           │  STT / LLM / memory extraction
                         Wi-Fi
                           │
┌──────────────────────────▼──────────────────────────┐
│            ESP32-S3 R2 BACKPACK BRAIN               │
│  persistent pet state · local behavior engine       │
│  timers / boredom / sleep · semantic planner        │
│  cloud client · mic + audio · touch service panel   │
│  local persistence · diagnostics / watchdog         │
└──────────────────────────┬──────────────────────────┘
                           │ BLE (central role, 1 connection)
                           ▼
                     SPHERO R2-D2
```

No Raspberry Pi. The ESP32-S3 is the brain, not a bridge to a bigger computer.

## Layering

```
brain/      state · memory · cloud client · decision boundary
behavior/   semantic behaviors · idle · mood · scheduler
r2/         capabilities → commands → protocol → ble
platform/   display · touch · power · audio · storage · imu · connectivity
ui/         status · settings · diagnostics · hardware test
```

**The one rule that matters:** the brain never sees a BLE packet, and the BLE
layer never sees a mood. `behavior/` is the only thing that translates between
them. If a mood value ever appears in `r2/`, or a DID/CID in `brain/`, the
design has failed.

## Why the LLM is not the identity

The persistent state *is* R2. The LLM is consulted, like a person consults
memory or language. This is not philosophy — it is what makes provider swaps,
offline operation and cheap idle behavior possible at once.

Concretely, the LLM is invoked for language understanding, conversation, novel
situations, memory extraction and higher-order decisions. It is **not** invoked
for idle chatter, mood drift, boredom, sleep/wake, quiet hours, reflexes, or
anything on a timer. Animating around an API round-trip would make R2 feel
laggy *and* expensive — the two failure modes that most reliably break the
illusion.

## Semantic behaviors

The reasoning layer's vocabulary is intent, not actuation:

```
express_curious()  express_happy()  express_annoyed()  listen()  perk_up()
look_around()      approach()       retreat()          celebrate()
complain()         wander()         sleep()            wake()   investigate()
```

Each choreographs several channels over time. `express_curious()` is roughly:
subtle dome move → pause → dome the other way → questioning chirp →
holo/logic reaction → maybe a small body adjustment.

Design constraints found during archaeology (`research/r2-capabilities.md`):

- **Authored animations bundle their own sound.** Never layer `play_audio` on
  top of one — it double-talks. A behavior is *either* an authored animation
  *or* a hand-composed sequence.
- **Pick sounds from a family, not by id.** `R2_CHATTY_*` has 62 members
  precisely so the same sentiment need not repeat. Track recent picks.
- **Use only `R2_*` sounds.** 176 of the 388 ids are BB-8/BB-9E/R2-Q5 voices or
  test tones — a different droid, or a factory beep.
- Every behavior carries **duration, interruptibility, energy cost and
  cooldown**. Interruptibility is what lets R2 notice you mid-idle; cooldown is
  what stops him being twitchy.

## Choreography

Time-sequenced channel commands with a 120 ms floor between packets
(`research/r2-protocol.md` §5). The robot reports `play_animation_complete`,
`leg_action_complete` and `head_reset_to_zero` — sequence against those events,
not fixed sleeps, or we fight the robot's own timing.

`claude-r2d2-buddy/main/translator.c:126-143` is a small worked example: one
event sets animation + front LED + back LED + head angle together. That
multi-channel-at-once shape is right; its Claude-event triggering is not ours.

## Persistence — three stores, not one blob

| Store | Contents | Likely medium |
|---|---|---|
| Device state | Wi-Fi, paired R2, volume, quiet hours, screen, provider, calibration | NVS |
| Character state | mood, energy, relationship, memories, habits, goal, last interaction | LittleFS or SD |
| Operational telemetry | BLE/cloud errors, reconnect count, battery, heap, uptime, last call | ring buffer → SD |

They have different write frequencies, different durability needs and different
consequences on loss. Deciding the exact media is deferred until the board is in
hand; keeping them separate is not.

## Connectivity and failure

- **BLE central to R2** — single connection. Dropping the peripheral role from
  `claude-r2d2-buddy` removes dual-role radio contention before Wi-Fi arrives.
- **Reconnect is a feature, not error handling.** On disconnect: clear handles,
  clear handshake state, reset RX reassembly, rescan
  (`r2d2_central.c:333-349`). An entity that is "continuously there" is mostly
  an entity that reconnects boringly.
- **Degrade in a fixed order.** Cloud unreachable → local behavior continues,
  R2 unaffected. Wi-Fi busy → R2 control still wins the radio. R2 unreachable →
  the panel says so plainly and the brain keeps its state warm.
- Failure or disconnect must result in **stop**, never last-command-repeats.

## Cloud boundary

Provider-agnostic client behind an interface. No credentials in git; secrets via
`.env.example` locally and NVS/provisioning on device. TLS memory alongside LVGL
and a PSRAM framebuffer is a real unknown (`research/embedded-path.md`) — which
is why the first embedded slice has no cloud in it at all.

## Modularity note: Claude activity as a sensor

Letting R2 react to Claude Code / Cowork activity is an interesting *input*, and
`anthropics/claude-desktop-buddy` documents the protocol. It must remain one
optional sensor among many, behind the same event interface as the IMU. It must
never become the reason R2 does things, or the pet collapses back into a status
light.
