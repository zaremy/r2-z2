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
.venv/bin/pip install sounddevice openwakeword pywhispercpp
```

`pywhispercpp` ships a prebuilt arm64 wheel — no compilation. It downloads
`base.en` (147 MB) on first use.

## Transcription (V4, #45)

`transcribe.py` turns a captured utterance into text. The model is held
**resident**: shelling out to whisper.cpp's CLI would reload 147 MB per
utterance and spend the whole latency budget on process spawn.

```python
from transcribe import create_transcriber
tr = create_transcriber("whisper.cpp", warm=True)
print(tr.backend_report())        # {'metal': True, 'device': 'Apple M3 Pro', ...}
print(tr.transcribe(pcm).text)
```

**Check `backend_report()` after any dependency bump.** #45 rules out
`faster-whisper` because it is CPU-only on Apple Silicon and forfeits the GPU
silently — a prebuilt wheel can do exactly the same thing, and every functional
test would still pass. The check reads back what whisper.cpp actually
initialised.

MEASURED on an M3 Pro, `base.en`, warm, p50 over 10 runs:

| audio | p50 | realtime factor |
|---|---|---|
| 5.0 s real quiet-room recording (sparse) | 0.061 s | 0.012x |
| 7.4 s continuous speech (dense tokens) | 0.125 s | 0.017x |

Against #45's 1.5 s budget that is roughly 12x headroom on the harder case.

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

## Reasoning (V5, #46)

`reason(heard) -> Behaviour`, which composes a `Beat` from the choreography
layer. Provider is config, not code:

```bash
python3 voice/reason.py "Hey R2, good morning!"
```

**The model never names an op.** The tool schema offers three closed choices —
a sound name from `R2_SOUNDS`, a dome angle, a mood — and this module composes
the beat from primitives. So `animation` and `set_stance` are not filtered out
of model output; they are *unreachable from it*. A confused or hostile response
cannot express them because the vocabulary has no word for them.

**The prompt is not enforcement.** MEASURED across five models with
"under 12 degrees does not move" written into the parameter description, they
returned **0, 8, 12 and 20 degrees**. Two of four sit below
`MIN_DOME_TRAVEL_DEG`, where the dome silently ignores the command and reports
success anyway. Every number is re-checked in `_validate` and every correction
is logged onto the `Behaviour`.

**Mood does not pick a colour.** All six reachable RGB corners already carry a
status meaning (D-012 Amendment A), so hue is not available for expression.
Mood selects a sound and a gesture; the rest colour stays a status claim.

**Failure lands on yellow, not red.** `BASE_DANGER` is "danger and stop, ONLY";
a timed-out API call is "needs monitoring". `error_beat()` shows PENDING and
then hands him back neutral, so he is never left lit on a warning.

Model latency measured on this task, p50 of 3 calls:

| model | p50 | valid sound id |
|---|---|---|
| gpt-5.4-mini | **0.63 s** | 3/3 |
| gpt-5.4-nano | 0.74 s | 3/3 |
| gpt-4o-mini | 1.62 s | 3/3 |
| gpt-5-mini | 14.37 s | 3/3 |
| gpt-5-nano | 14.73 s | 3/3 |

All five pick valid ids, so latency is the discriminator — and R2 stands lit in
Processing for that whole time. Note "nano" is not the fast one.

## Threepio's voice (V6, #47)

**R2 never speaks.** He chirps, from his own body. C-3PO is a *second
character* who rides in the backpack and does the talking — which is canon,
and which dissolves the speaker-displacement worry in D-011: Threepio's voice
*should* sound like it comes from somewhere other than R2's body, because it
is somebody else.

The character lives in `STEERING`, a plain-language prompt. `gpt-4o-mini-tts`
takes direction, so the register is iterated by editing that string rather
than by tuning a synthesiser. `LINE_SYSTEM` embeds the same string, so there
is exactly one description of who he is — two would drift, and the voice would
stop matching the words.

The prompt describes *a* protocol droid and deliberately names no performer
and no character. A test asserts that, because cloning a specific performance
raises a likeness question this project has no reason to raise.

### The voice

`TTS_VOICE=ballad` — operator ruling 2026-08-18, chosen after auditioning all
thirteen voices on the same line with the same steering prompt. Recorded as a
taste call, because that is what #47 AC1 makes it.

The full set, which the API will enumerate if you send it an invalid one:
`alloy echo fable onyx nova shimmer coral verse ballad ash sage marin cedar`.

**The audition is retained, so revisiting costs nothing.** All thirteen, the
same line, the same steering prompt, plus ten varied in-character lines:

```
R2Z2-vault/R2Z2-vault/Experiments/data/threepio-audition-20260818/
```

Re-listen before regenerating — the set already exists and a fresh render
would differ subtly from the one the ruling was made on, which would make the
comparison dishonest. If `STEERING` changes, though, the audition IS stale:
the voice was chosen against that prompt, and a different character brief
deserves a fresh pass.

### Quiet hours are enforced here, not only upstream

#47 AC3 assigns quiet hours to V7. This module gates anyway, defaulting **on**
(22:00–08:00). V7 not calling `speak()` is a policy held by a caller, and
`CLAUDE.md` is explicit that a guard belongs where the effect happens —
`FORBIDDEN_OPS` was once enforced on the construction path, so any caller that
skipped construction skipped the guard. Sound is the one output that reaches a
sleeping household through a closed door.

An audition overrides it at the **call site**, never by weakening the default:

```python
speak(line, quiet_hours=QuietHours(enabled=False))
```

### Generating audio and making noise are separate decisions

`play_audio` defaults to `False`. The one that makes a sound is the one you
have to ask for.

```bash
python3 -c "import sys; sys.path.insert(0,'voice'); import speak as S; \
  print(S.compose_line('R2 bumped into the doorframe again'))"
```

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
