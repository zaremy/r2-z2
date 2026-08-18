# voice — ears for the backpack (S2 V1, #42)

The input half of the C-3PO side of the pair. R2 never speaks (D-011); this is
how Threepio hears. **Nothing here touches BLE or the robot** — it runs with R2
powered off, on purpose, so that when the composed behaviour misbehaves we can
tell the microphone from the choreography.

```
mic -> capture -> wake -> (V4) transcribe -> (V5) reason -> behavior -> r2
       ^^^^^^^^^^^^^^^^
```

## The wake phrase is `z2`

Operator decision, 2026-08-17. It is not a stock phrase in any free engine, so
**it has to be trained once** — the step below. Until it exists, every command
here fails with an error that says so.

> **Recorded risk, to be settled by measurement, not argument.** `z2` is two
> syllables. ESPHome's guidance for microWakeWord — the framework that will run
> this on the backpack — is 3–4 syllables, uncommon, so it does not fire by
> mistake. "Zee-two" also sits close to ordinary speech: *these two*, *he's
> two*, *she's due*. AC3 below is what turns that worry into a number. If the
> false-accept rate comes back unusable, a longer phrase is a config change,
> not a rewrite.

## Why the engine is swappable

`wake.py` is an interface, not a Porcupine wrapper. Picovoice discontinued its
Free Tier on 2026-06-30 and its FAQ now states there are *"no dedicated free or
paid plans for personal or non-commercial use"* — so the engine named in the
spec became unavailable between speccing and building. Three implementations
sit behind one seam:

| engine | key needed | frame | notes |
|---|---|---|---|
| `openwakeword` | no | 1280 samples / 80 ms | default. Code Apache-2.0, models CC BY-NC-SA |
| `porcupine` | yes, paid | 512 samples / 32 ms | kept working in case a key is ever bought |
| `scripted` | no | configurable | tests only — no wheels, no models, no mic |

The two real engines disagree about frame size, which is exactly why
`capture.Chunker` exists and why nothing upstream may assume one. The ESP32's
own ESP-SR detector drops into the same seam at backpack time.

## Setup

```bash
cd mac-prototype
.venv/bin/pip install sounddevice openwakeword
```

## Get a model to test with, before training anything

Model binaries are gitignored — fetched, not vendored. The community `r2d2`
model is the cheapest way to exercise the whole path, and it is what the room
measurements in `docs/research/wake-word-training.md` were taken with:

```bash
cd mac-prototype/voice/models
gh api repos/fwartner/home-assistant-wakewords-collection/contents/en/r2d2/r2d2.onnx \
   -H "Accept: application/vnd.github.raw" > r2d2.onnx
```

MIT licensed, 208 KB. Then:

```bash
cd mac-prototype && .venv/bin/python -m voice listen --keyword voice/models/r2d2.onnx
```

Measure with this before spending an hour training `z2` — see the recommended
sequence in the research doc. It answers what a *published* recall figure means
in your actual room, which is the number you need to aim a training run at.

## Train the `z2` model (once, ~1 hour, free)

openWakeWord trains from **synthetic speech** — you never record your own
voice. Piper generates thousands of spoken variants of the phrase, and those
are trained against real-world audio as the negative set.

1. Open openWakeWord's automated training notebook in Google Colab
   (`notebooks/automatic_model_training.ipynb` in github.com/dscripka/openWakeWord).
2. Set the target phrase to **`z2`**. Consider adding `zee two` as a spelling
   variant so Piper's pronunciation is what you actually say.
3. Run it. It emits `z2.onnx` (and `z2.tflite`).
4. Drop `z2.onnx` into `mac-prototype/voice/models/`.

The same Piper-generated clips retrain this for **microWakeWord** when the
backpack arrives, so the phrase chosen here is the phrase that ships.

Smoke-test the pipeline before training, with a stock model:

```bash
.venv/bin/python -m voice listen --keyword hey_jarvis
```

## AC1 — does it hear you across the room?

> Wake phrase spoken at 2 m in Room B (50–60 dBA ambient, TV or music at
> conversational level) is detected in 9 of 10 trials.

```bash
.venv/bin/python -m voice listen --save /tmp/z2-ac1
```

Stand 2 m away with the TV at conversational level. Say `z2` ten times, leaving
a couple of seconds between each. Every detection prints a `WAKE` line with the
timestamp, score and input level; every captured utterance prints its duration
and how it ended. Count the `WAKE` lines. The `.wav` files let you listen back
to what it actually caught — a miss and a mis-segment look identical in the
count and completely different in the audio.

## AC3 — how often does it fire at nothing?

> False-accept and false-reject counts over a 30-minute ambient sample are
> recorded, with the sensitivity value that produced them.

```bash
.venv/bin/python -m voice ambient --minutes 30
```

Leave it running in the room with normal life happening — TV, conversation,
kitchen noise. **Do not say the phrase.** Every fire is a false accept. It
prints a rate per hour and the ambient peak level at the end.

Then re-run at a different `--sensitivity` (higher = fewer false accepts, more
misses) until AC1 still passes at 9/10 and the false-accept rate is liveable.
**That** is the number that goes in the issue — sensitivity set from a
measurement, not chosen in the abstract.

## Microphone permission — the agent CAN open it

**OBSERVED 2026-08-17.** The BLE restriction does **not** extend to the
microphone. `claude.app` cannot start the BLE daemon (macOS attributes
CoreBluetooth to the responsible process, which has no usage description), and
the reasonable expectation was that audio input would be gated the same way. It
is not: opening a 16 kHz / int16 / mono `RawInputStream` from an agent-spawned
process succeeds and reports cleanly.

Predicted `blocked`, measured `works` — recorded here because the same wrong
inference would otherwise be made again next session, and because a capability
assumed absent is never retried.

Two consequences:

- The agent can run the pipeline and take AC1/AC3 measurements itself.
- **AC1 still needs a human**, because a human has to say the phrase ten times
  from 2 m away. AC3 does not — but it records 30 minutes of a room in the
  house, so it is the operator's call to make deliberately, not something to
  start unasked.

## Tests

38 tests, no microphone, no wheels, no robot:

```bash
.venv/bin/python -m unittest discover -s mac-prototype -p 'test_*.py'
```
