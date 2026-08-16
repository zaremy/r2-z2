# Intent

## What this is

A persistent, provider-independent AI mind for a Sphero R2-D2 that already has
a good body and an authored personality. The obsolete software brain is
replaced; the character is not.

R2 should feel like an entity that is physically present in the household —
continuously there, with his own mood, energy, boredom, routines and memory of
you, expressed through the physical vocabulary he already has:

dome/head motion · locomotion · posture/stance · chirps and sounds ·
authored animations · front and rear lights · logic and holo lighting ·
spontaneous idle behavior

## What this is not

- A remote-controlled R2-D2
- A Claude status light
- A chatbot displayed on a robot
- A screen-based virtual pet strapped to R2
- A Raspberry Pi robot project

## The distinction that drives every decision

The character lives in **persistent state plus local behavior**, and the LLM is
a service that state consults. Not:

```
user → LLM → raw robot commands
```

but:

```
persistent R2 identity/state → perception + events → behavior/goal selection
  → LLM reasoning only when needed → semantic behavior → R2 choreography → R2
```

Consequences we hold ourselves to:

- **State survives a provider swap.** Mood, energy, relationship and memories
  are ours. Changing model vendor must not change who R2 is.
- **Routine life is local.** Idle, sleep/wake, mood drift, boredom, quiet
  hours, simple sensor reactions run on the ESP32 with no network.
- **The cloud is for the unusual** — language, conversation, novel situations,
  memory extraction, higher-order decisions.
- **The LLM operates semantic behaviors, not motors.** `express_curious()`,
  never `set_dome_position(-27)`.

## The screen

The backpack's 1.8" touchscreen is a **rear-facing service panel**: status,
connectivity, diagnostics, hardware tests, provisioning, quiet hours, autonomy
level. It is not R2's face. Making it one would move the character off the body
and defeat the entire premise. Revisiting that requires an explicit entry in
[decisions.md](decisions.md).

## Order of operations

Deliberately sequential — never three unsolved problems at once:

```
R2 protocol → Mac interaction quality → board bring-up → ESP32→R2 BLE
→ service panel → local behavior engine → Wi-Fi/cloud
→ microphone / wake word / STT → persistent memory → higher autonomy
```

Voice is **not** phase zero. A microphone on the board is not a reason to start
with speech.

## Explicitly out of scope for now

Computer vision · navigation · long-term vector memory · elaborate voice
pipelines · rewriting Waveshare drivers · any dependency on Claude Desktop.

## How we will know it is working

Not by a feature list. By whether someone walking past R2 on a normal Tuesday
believes he noticed them — and by whether *we* stop thinking of it as a device.
