# r2-z2

Turning a Sphero R2-D2 into a persistent AI companion — a resident entity, not
a remote-controlled toy.

> The experiment succeeds when R2-D2 stops feeling like hardware receiving
> commands and starts feeling like the same little entity is continuously there.

R2 already has a good body and an authored personality. This project replaces
the obsolete app brain, not the character. His physical vocabulary — dome,
stance, locomotion, chirps, authored animations, logic and holo lights — stays
the interface. The AI's job is to make those outputs feel *intentional*.

**The screen is maintenance infrastructure. The ESP32 is the replacement brain.
The cloud provides intelligence. R2 himself is the character.**

## Status

Initialization + code archaeology complete. No code has run against the
physical robot or the board yet.

Start here: **[docs/research/initial-findings.md](docs/research/initial-findings.md)**.

## Layout

```
CLAUDE.md              engineering rules — read before contributing
docs/
  intent.md            what we are building and what we are not
  architecture.md      layering, and why the LLM is not the identity
  decisions.md         decision log (ADR-lite)
  research/            source-traced findings, evidence-labelled
mac-prototype/         Python + bleak; proves the interaction model
firmware/              ESP32-S3 backpack (not started)
tools/                 project utilities
research/external/     upstream clones — gitignored, never vendored
```

## Quick start

```bash
cd mac-prototype && ./r2 scan
```

Requires a one-time macOS Bluetooth grant — see
[mac-prototype/README.md](mac-prototype/README.md).

To restore the external research clones (they are not in git):

```bash
mkdir -p research/external && cd research/external
git clone https://github.com/ccb/sphero-r2d2.git
git clone https://github.com/baoshi/claude-r2d2-buddy.git
git clone https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8.git
git clone https://github.com/vthinkxie/claude-desktop-buddy-esp32.git
git clone https://github.com/anthropics/claude-desktop-buddy.git
git clone https://github.com/astagi/freer2.git
```

Pinned SHAs are in [docs/research/source-map.md](docs/research/source-map.md).

## Hardware

- Sphero R2-D2 app-enabled droid (BLE name `D2-XXXX`)
- Waveshare ESP32-S3-Touch-AMOLED-1.8 — **revision must be verified at runtime**
  ([board-revision.md](docs/research/board-revision.md))
- macOS development host. No Raspberry Pi.

## Safety

R2 is physical hardware. Bring-up order is fixed and each actuator test is
individually opt-in: **read-only → LEDs → audio → small dome → stance →
locomotion**. Never flash firmware built for another board revision. Never
commit credentials.
