# Wake-word training — how to approach it, and what we can pick up

Research for **#42 (S2 · V1)**, 2026-08-17. Written because the operator asked
not to one-shot a Colab notebook, and that instinct was right: the official
notebook is bit-rotted, and the community's own published numbers say the
easy path does not reach #42's acceptance bar.

Labels follow the project convention: `OBSERVED` / `INFERRED` / `UNKNOWN`.

---

## Verdict

1. **Do not train first. Measure first, with a model that already exists.**
   The community collection ships a working, MIT-licensed `r2d2` openWakeWord
   model (208 KB `.onnx` + `.tflite`). Running our own AC1/AC3 harness against
   it costs nothing, proves the harness, and calibrates what a *published*
   recall figure actually means in the operator's room. Every training hour
   spent before that is spent blind.
2. **The official openWakeWord Colab notebook should not be the first attempt.**
   It is documented as failing out of the box in 2026, and even when it runs
   its results are reported as poor.
3. **#42's AC1 bar (9 of 10 detections) is at risk from the METHOD, not from
   the phrase.** No model in the community collection publishes a recall above
   0.710. That is a finding about community-grade openWakeWord training, and it
   needs to be reconciled with AC1 before the criterion is treated as pass/fail.

---

## Which repositories are actually canonical

`OBSERVED` 2026-08-17, via the GitHub API. SHAs recorded in `source-map.md`.

| Project | Home | Stars | Licence | Last commit | Note |
|---|---|---|---|---|---|
| openWakeWord | `dscripka/openWakeWord` | 2671 | Apache-2.0 | 2025-12-30 | Library we use. Last **release** is v0.6.0, 2024-02-11 |
| microWakeWord | `OHF-Voice/micro-wake-word` | 914 | Apache-2.0 | 2026-07-06 | The **backpack** target. `kahrendt/microWakeWord` is now a fork of this |
| HA wake-word collection | `fwartner/home-assistant-wakewords-collection` | 551 | MIT | 2026-01-13 | 102 English models, pre-trained, drop-in |
| ESPHome model index | `esphome/micro-wake-word-models` | 119 | Apache-2.0 | 2025-03-21 | The stock ESPHome set |
| 2026 Colab fork | `alfiedennen/openwakeword-colab-2026` | **1** | MIT | 2026-05-09 | Fixes the bit-rot. Single author, single commit — unvetted |

> `INFERRED`: openWakeWord's library code is still maintained (commits through
> 2025-12) but has not cut a release since Feb 2024. The training notebooks are
> the part that rotted, not the inference path we depend on.

---

## The four routes to a `z2` model

### A. Pick up an existing model — zero training

The collection has 102 English models as `.onnx` **and** `.tflite`. `.tflite`
matters: that is microWakeWord's format, so a model here is a candidate for the
backpack as well as the Mac.

`OBSERVED`: it already contains **`r2d2`**, trained on the phonetic spelling
`"r two d two"`. Not our phrase, but the nearest neighbour in the droid-name
family, and free.

Also present and thematically adjacent: `TARS`, `glados`, `hal`, `wall-e`,
`johnny_five`, `marvin`, `skynet`, `terminator`, `wheatley`, `jarvis`.

### B. Hosted training — openwakeword.com

`OBSERVED` (page read 2026-08-17): a **third-party** commercial platform built
on openWakeWord, not the library author's site. Claims cloud-GPU training in
under an hour, ONNX output, 20+ languages, 317 models / 184 wake words, and
both openWakeWord and microWakeWord targets. Offers an MCP server billed by
card or crypto with no account.

`UNKNOWN`: price. Not stated anywhere on the landing page; the web flow requires
sign-in before quoting. Also unassessed: what it does with the phrase you
submit, and its output licence.

> Note a **disagreement worth recording rather than resolving silently**: this
> site advises *"Short, catchy words work best"*, while ESPHome's microWakeWord
> guidance advises 3–4 syllables and something uncommon. They cannot both be
> good advice for the same failure mode. Our own measurement is the tiebreak.

### C. The maintained 2026 Colab fork

`alfiedennen/openwakeword-colab-2026` exists because, in its author's words,
the official notebook *"has bit-rotted hard since 2023"* and *"fails out of the
box for at least eight separate reasons"* on a default 2026 Colab runtime —
Python 3.12 wheels, `torchaudio 2.x` API changes, YAML key changes, and
`piper-sample-generator` restructuring.

It replaces upstream's `auto_train` with ~250 lines of PyTorch implementing the
original curriculum: three-stage LR schedule (1e-4 → 1e-5 → 1e-6), negative
weight ramp (1 → 1500), hard-negative mining, and false-positives-per-hour
validation against ACAV100M.

Cost: 75–90 min on a Colab Pro L4 ($10/mo), ~2.5 h on the free T4. Datasets
pulled: FMA-small (~8 GB), ACAV100M features (~17 GB), ~270 MIT impulse
responses.

`INFERRED` risk: **1 star, 1 commit, 1 author.** The fixes are plausible and
specific, which is evidence it was really debugged, but nothing here is vetted
by the upstream project. Read it before running it.

### D. The official notebook

`automatic_model_training.ipynb`. `OBSERVED` from the upstream tracker: issue
#110 reports accuracy 0.627 / recall 0.257 / **1.06 FP per hour** against the
notebook's own stated targets of 0.7 / 0.5 / 0.2, using `n_samples: 10000` and
`steps: 50000`. The same reporter got better results from the *older, manual*
`training_models.ipynb` with only 10,000 synthetic samples.

`INFERRED`: the "automatic" path is the worse path. That inverts the obvious
reading of the docs, where it is presented as the easy on-ramp.

### E. Local training on the Mac — not assessed

Would need the same ~25 GB of corpora plus a Piper pipeline, on arm64 /
Python 3.14, with a notebook written for Colab GPUs. `UNKNOWN` whether it
converges here. Only worth attempting if A–C are all rejected.

---

## The measured evidence: what community training actually achieves

32 of the 102 English models publish their training parameters and validation
results in their READMEs. Extracted 2026-08-17 (`FP/hr` = false positives per
hour on the validation corpus; openWakeWord's own targets are recall ≥ 0.5 and
FP/hr ≤ 0.2):

| model | phrase trained | recall | FP/hr |
|---|---|---|---|
| johnny_five | johnny five | 0.362 | **0.00** |
| hey_chatterbox | hey chatterbox | 0.436 | **0.09** |
| jarvis | jarvis | 0.262 | **0.18** |
| ok_tau | ok tau | 0.522 | 0.35 |
| hey_rick | hey rick | 0.464 | 0.71 |
| hey_nabu | hey nabu | 0.584 | 1.06 |
| hey_kitt | hey kitt | 0.449 | 1.15 |
| … 24 more between 1.86 and 5.75 … | | | |
| **r2d2** | **"r two d two"** | **0.681** | **4.69** |
| hey_snips | hey snips | 0.595 | 6.90 |

**The headline, `OBSERVED`: zero of 32 models meet both targets.** Three meet
the FP/hr target, and all three do it by sacrificing recall (0.262–0.436). The
best recall anywhere in the collection is **0.710** (`choo_choo_homie`).

`r2d2`'s own numbers — recall 0.681, FP/hr 4.69 — are *above average on recall*
and roughly 23× the FP/hr target. Its published training profile:

```
number_of_examples        = 25000
number_of_training_steps  = 500000
false_activation_penalty  = 5000
target_words              = ["r two d two"]
```

That parameter vocabulary is the reusable "training profile" — it is the
HA-community Colab path's interface, and `false_activation_penalty` is the dial
that trades recall for false accepts. `r2d2` also demonstrates the technique we
need for `z2`: **spell the phrase phonetically for Piper** (`"r two d two"`,
not `"R2-D2"`), so the synthetic speech matches how it is actually said.

### What this means for #42 AC1

AC1 asks for **9 of 10** detections at 2 m. No published model in this
collection exceeds 0.710 recall.

`INFERRED`, and the caveat matters: these recall figures are measured on a
synthetic validation set containing hard negatives and adverse conditions, not
on a person speaking clearly at 2 m. Real-room recall at close range is
plausibly much higher. **We do not know the conversion factor, and that is
precisely the gap route A closes for free.**

Do not conclude AC1 will fail. Conclude that nothing published gives grounds to
expect it to pass, and that measuring `r2d2` in the room is the cheapest way to
find out which.

### The syllable question — my own pushback was not supported

The operator chose `z2` (two syllables). I pushed back citing ESPHome's 3–4
syllable guidance. Grouping the 32 models by the syllable count of their first
target phrase:

| syllables | n | mean FP/hr | mean recall |
|---|---|---|---|
| 1–2 | 17 | 3.08 | 0.505 |
| 3 | 9 | 3.31 | 0.535 |
| 4+ | 6 | 2.54 | 0.536 |

`OBSERVED`: **no meaningful relationship** in this data. The ESPHome guidance
may still be sound — n=32, self-reported, uncontrolled, wildly different
training configs and authors, and no measurement of the failure mode the
guidance actually describes (collisions with ordinary speech, which a
validation corpus of music and noise would not surface). But this is the only
quantitative evidence we have, and it does not support the objection I raised.

Recorded so the objection is not repeated as though it were established.

---

## Recommended sequence

1. **Fetch `r2d2.onnx`** (208 KB, MIT) from the collection into
   `mac-prototype/voice/models/`.
2. **Run AC1 and AC3 against it**, unchanged, with the existing harness. This
   validates the measurement rig and produces the first real-room numbers this
   project has for any wake word.
3. **Compare** the room result against `r2d2`'s published 0.681 / 4.69. That
   ratio is the thing we are missing, and it tells us what recall a trained
   `z2` has to publish in order to clear AC1.
4. **Then train `z2`** — phonetically as `"zee two"` / `"z two"` — via route B
   or C, targeting the profile `r2d2` used and raising
   `false_activation_penalty` if step 3 shows false accepts dominate.
5. **Re-run AC1/AC3** on the trained model and record both numbers with the
   sensitivity that produced them, per AC3.

Steps 1–3 cost roughly one evening and no money. Step 4 is the only one that
costs an hour of GPU or a fee, and by then it is aimed rather than speculative.

---

## MEASURED — first room session, 2026-08-17

Operator as speaker, Mac microphone as instrument, community `r2d2.onnx`
unchanged. Every figure below was re-derived from the stored score traces by
script (24 claims, 24 OK) rather than read off a console.

### What was run

| run | mic | condition | said | detected @0.5 | result |
|---|---|---|---|---|---|
| 0 | MacBook | noisy, speaker stepped further away mid-run | 5 | 2 | **2/5** |
| 0b | MacBook | quiet, ~0.5 m | 5 (4 clear + 1 deliberately mumbled) | 4 | **4/4 clear, 0/1 mumbled** |
| 1 | MacBook | 2 m + TV — **VOID** | 10 | — | instrument dead, see below |
| 0c | Logitech BRIO | quiet, near camera | 6 | 6 | **6/6** |

Instrument was validated before each phase against synthetic `say`-generated
speech: 8 of 9 clips fired at ~0.999, and a `hey_jarvis` control on its own
phrase scored 0.9994, separating "model is weak" from "pipeline is wrong".

### Finding 1 — the score distribution is near-binary, and the threshold is nearly inert

> **AMENDED 2026-08-17, same day.** The first version of this section said
> "sensitivity is not a dial" and "there is no sensitivity that trades recall
> against false accepts". **That was too strong**, and the full sweep below
> refutes it. The threshold is inert on clean speech and moves exactly one
> marginal utterance under noise. The original coarse sweep only sampled
> 0.1/0.3/0.5/0.7/0.9 on the one run where the answer happens to be flat
> everywhere; a 999-point sweep across all three runs found the transition.
> The headline — the distribution is overwhelmingly bimodal — survives. The
> absolute claim does not.

`OBSERVED`, all three runs pooled: of **902 frames, 818 scored below 0.001 and
56 above 0.9. Five landed anywhere in 0.01-0.9** (0.014, 0.022, 0.329, 0.402,
0.670) — 0.55 %. Between the highest non-firing frame (0.0062) and the lowest
firing one (0.9350) there is an **empty gap of 152x in score**.

Full sweep, 999 thresholds from 0.001 to 0.999, showing the largest
**contiguous** interval containing the 0.5 operating point over which the
detection count does not change:

| run | condition | said | count @0.5 | stable interval | transitions |
|---|---|---|---|---|---|
| 0 | noisy + distance change | 5 | 2 | **[0.330, 0.999]** (67 %) | 0.002: 5→3, **0.330: 3→2** |
| 0b | quiet, built-in mic | 5 | 4 | [0.002, 0.999] (100 %) | 0.002: 5→4 |
| 0c | quiet, BRIO | 6 | 6 | [0.001, 0.999] (100 %) | none |

**The one transition that matters is 0.330 in the noisy run**, and it is worth
one detection: **2/5 at threshold 0.5, 3/5 at 0.3.** A single utterance scored
0.3294 and sits alone in the middle of the distribution.

So the honest statement: the threshold does nothing on clean close speech, and
the only place it does anything is on **marginal utterances under noise** —
which are exactly the ones AC1 is about. `INFERRED`: **0.3 is a better
operating point than 0.5** for this model. The false-accept cost of that choice
is `UNKNOWN` and needs AC3's ambient sample; on one marginal utterance it is
not a general recommendation.

> **Methodological note, because the first attempt got it wrong.** Detection
> count is **not monotonic** in threshold. A *lower* threshold passes more
> frames, which extends the refractory window and merges two adjacent
> utterances into one detection — so lowering the threshold can *reduce* the
> count (visible above at 0.002, where all three runs drop). Taking min/max
> over the set of thresholds yielding N detections is therefore meaningless;
> the set is not contiguous. The first sweep did exactly that and reported a
> fictitious "1075x stable span" for a run that visibly changes at 0.33.

**This still contradicts an assumption written into #42 AC3** as originally
worded, which required false accepts to be recorded "with the sensitivity value
that produced them" and the sensitivity "set from that measurement". With
0.55 % of frames in the entire middle of the range, there is almost nothing for
a threshold to move through, and no meaningful recall/false-accept curve to
pick a point on. AC3 has since been reworded to require this sweep and branch
on its result rather than assume one.

Consequence: a `z2` model that fires too often **can be dialled back at runtime
only marginally, and mainly by retraining.** That is what `false_activation_penalty = 5000` in
`r2d2`'s published profile is for, and it makes that parameter the primary
tuning surface rather than an afterthought. AC3 needs rewording before it can
be met as written.

### Finding 2 — detection is level-invariant; input gain is irrelevant

`OBSERVED`: the same known-positive clip, digitally attenuated across a 46 dB
range, scores identically.

| peak level | −16.2 | −28.3 | −36.2 | −42.2 | −56.3 | −62.3 dBFS |
|---|---|---|---|---|---|---|
| score | 0.9996 | 0.9996 | 0.9996 | 0.9996 | 0.9996 | 0.9996 |

openWakeWord normalises internally. The session's 27 % system input gain had no
effect on detection, and raising it would have changed nothing. Level still
matters for *segmentation*, which works on raw RMS — see finding 4.

### Finding 3 — it rejects rather than degrades

`OBSERVED`: a deliberately mumbled "R2-D2", same speaker, same distance,
seconds after a clean one, scored **0.0009** — not 0.4. Consistent with
finding 1. The model recognises the phrase or does not see it at all, which
means a user who is misheard gets no feedback gradient to correct against.

### Finding 4 — no absolute silence threshold can serve two microphones

`OBSERVED`, both mics, same room, same speaker:

| mic | noise floor | speech |
|---|---|---|
| MacBook Pro Microphone | −68.6 dBFS | −39 to −53 dBFS |
| Logitech BRIO | −47.6 dBFS | −31 to −34 dBFS |

**The BRIO's silence is louder than the MacBook mic's speech.** No constant
separates speech from silence on both: −40 dBFS works on the BRIO and reads
most MacBook speech as silence; −60 dBFS works on the MacBook and makes BRIO
silence read as speech, so an utterance never ends.

`voice/capture.py`'s original `DEFAULT_THRESHOLD = 0.010` (−40.0 dBFS) sat
**0.2 dB above the MacBook mic's measured speech peak** — every frame of a real
sentence would have read as silence, so wake detection would fire and the
capture would immediately end empty. Fixed: the threshold is now derived from a
measured noise floor (`Segmenter.calibrate()`), with the constant demoted to a
fallback and six regression tests pinning the finding.

### Finding 5 — a held-open microphone returns digital silence, which reads as a result

Run 1 produced 560 frames and a 22-second WAV containing **zero non-zero
samples**. Presented as a score trace it is indistinguishable from "the
operator spoke ten times and nothing was detected" — i.e. it would have been
published as *0/10, model unusable*.

Cause: a Logitech BRIO appeared in the device list mid-session, shifting the
indices; macOS continued to default to the built-in microphone, which by then
returned nothing, while the BRIO worked. `OBSERVED` by probing each device
directly.

Two fixes, both applied to the survey runner: a **liveness gate** that samples
500 ms and refuses to record if no sample is non-zero (verified against the
live failure), and an **explicit device index** recorded in every trace header
— "whatever macOS picked" is not a recordable experimental condition.

This is the standing lesson restated with a new instance: the quiet answer
reads as safe, and only a known-good baseline makes the silence obviously
wrong rather than obviously bad news.

### What was NOT covered

- **AC1 was never completed.** The 2 m + ambient-noise, 10-trial run is the
  criterion, and it was attempted once and voided by finding 5. The strongest
  adjacent evidence is run 0 — **2/5** under noise with a distance change —
  and run 0c — **6/6** quiet and close on a better mic. Neither is AC1.
- **AC3 was not attempted.** No 30-minute ambient sample, so there is no
  false-accept rate. `UNKNOWN`. Note finding 1 changes what the criterion
  should even ask for.
- **No `z2` model exists yet.** Everything above is the `r2d2` community model
  standing in as a proxy for the phrase family.
- Single speaker, single room, single session. Sample sizes are 5–6 utterances.
  Nothing here supports a claim stronger than "under this protocol".

## Open questions

- `UNKNOWN` — openwakeword.com's price, data handling, and output licence.
- `UNKNOWN` — whether `alfiedennen/openwakeword-colab-2026` actually converges;
  it is one unvetted commit.
- **PARTLY RESOLVED** (was: the conversion factor between validation recall and
  real-room recall). `r2d2` publishes 0.681 validation recall; measured
  **6/6 quiet and close**, **2/5 under noise with a distance change**. So
  published recall badly understates clean close-range performance and the
  real variable is noise and distance, not the headline number. The 2 m figure
  specifically is still `UNKNOWN` — AC1 was voided, see above.
- `UNKNOWN` — whether a model trained for openWakeWord can be reused by
  microWakeWord on the backpack, or whether the phrase must be retrained from
  the same Piper clips. The collection shipping both `.onnx` and `.tflite`
  suggests the latter is the normal path.
