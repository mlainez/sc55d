# Matching libEmuSC to the SC-55: a black-box protocol, and how far it can go

The goal: make a **free** SC-55 — libEmuSC, LGPL — sound the same as the
reference this project ships, so that Nuked-SC55 and its non-commercial licence
are no longer needed. The constraint: Nuked-SC55 is a **black box**. Its output
may be measured to any depth; its source may not be read, instrumented or
consulted. Everything below respects that. What *is* fair game, and used
freely: Roland's published SC-55 documentation (the owner's manual and the GS
SysEx map are public), the ROMs themselves (libEmuSC already parses them), the
MIDI and GS standards, and any amount of measurement of the black box's output.

This builds on [`backend-options.md`](backend-options.md), which established
what libEmuSC is and catalogued its known gaps; read that first for the
licensing and provenance background. The one method from that document that is
**withdrawn** here is "instrument Nuked's PCM model to log register writes" —
that requires the source, and the black-box rule forbids it. Everything has to
come out of the audio.

**The short answer on feasibility**: 100% in the sense of bit-identical output
is not reachable by black-box methods, and the reasons are structural rather
than a matter of effort. 100% in the sense that matters — *nobody can tell
them apart on the corpus* — is reachable, but it is a research programme of
months, most of it inside libEmuSC's code, and its last stretch (effects) is
where the returns diminish hardest. Tiers, evidence and estimates are in
[§7](#7-feasibility-how-close-is-close).

---

## 1. Definitions: what "the same" means, in tiers

A single number is the wrong target. Two renders can differ at every sample
and be indistinguishable to any listener (a reverb tail whose diffusion taps
are 3 samples apart), or agree on 99.9% of samples and be obviously wrong (one
drum hit at the wrong pitch). So the protocol scores every stimulus on a
ladder, and the ladder is the deliverable:

| tier | criterion | how it is measured |
|---|---|---|
| **T0** bit-exact | identical PCM after alignment | `cmp` on raw frames |
| **T1** null-test identical | residual (A−B) below **−60 dBFS** peak, **−70 dBFS** RMS, after gain and sub-sample alignment | residual analysis, per stimulus |
| **T2** measurably identical | multi-resolution log-spectral distance below threshold in every band and every time frame; envelope, onset and pitch tracks within tolerance | the metric suite in §3 |
| **T3** perceptually identical | ABX on the Pi rigs, level-matched, blind: listeners score at chance | listening protocol in §6 |
| **T4** close | T2 fails in named, bounded ways; documented as known divergence | the scoreboard in §5 |

The end goal is **T3 on every file of the corpus, with T1 or T2 on every
isolated-instrument stimulus**. T0 is tracked because it is free to measure
and because where it *does* hold (it will, for some dry material) it is the
strongest possible evidence that a stage was reconstructed exactly — but it is
not the target, for the reasons in §7.

## 2. The rig

Most of this runs offline on x86; the two Raspberry Pis are for the last two
columns of the ladder (listening) and for the whole point of the exercise
(does the free engine run on a Pi 3).

**The oracle.** Nuked-SC55's `nuked-sc55-render` — already built here from the
patched decoder2 tree — renders any MIDI file to raw PCM deterministically.
It is bit-reproducible run to run and across x86-64 and AArch64 (every digest
this project has ever recorded agrees across the two), and upstream publishes
SHA-256 hashes of its output for 37 mk2-family files, so the oracle itself is
verified against an external reference. Output: 66207 Hz for the mkII family,
64000 Hz for the mk1, 16-bit stereo. Invoke with `--reset gm` or `--reset gs`
to fix the initial state.

**The candidate.** A headless libEmuSC renderer: ~200 lines against the
library's `get_next_frame(float&, float&)` API, feeding the same MIDI file with
the same reset, rendering at **the oracle's native rate** — 66207 or 64000 —
so that no resampler stands between the two outputs. It must be
single-threaded and deterministic (same input → same bytes), which is a
property to verify on day one, because without it no regression test means
anything. Float output is quantised to 16-bit with the same rounding at the
very end; T0/T1 are evaluated on that.

**The stimulus generator.** A programmatic SMF writer (one exists from this
project's work: `smf2serial.py` carries a parser, `bench.cpp` a generator; a
Python module that emits SMF0 with note, CC, RPN/NRPN, SysEx and precise
timing is a morning's work). Every stimulus in §4 is generated, never
hand-authored, so the whole matrix is reproducible and enumerable.

**The comparison toolkit.** Python + numpy/scipy, one script per metric in §3,
all writing to one machine-readable results table keyed by stimulus id. Nothing
here needs a GPU or a licence.

**The corpus.** Upstream's 37 mk2-family reference files (`test/integration/`
in the core tree), plus `/data/midi/*.mid` from the appliance (the tracks that
actually get played). The corpus is the *final exam*; the stimuli in §4 are the
coursework, and the order matters: fixing the corpus directly is how you end up
tuning to 37 songs.

**The Pi rigs.** Pi 3 at `192.168.0.20` and Pi 4 at `192.168.0.21`, the
appliance image with `midi-engine` (which already hosts several engines and
switches between them), the same jack/DAC path, and — from this session — a
verified live MIDI feed from the host over the DIN input at 31250 baud. That
is an ABX rig: same MIDI to either engine, same output stage, the listener
does not know which is playing.

## 3. Metrics

Every comparison first **aligns**: estimate the latency offset by
cross-correlating the first second of both renders (both engines add their own
processing delay; the SC-55 firmware has a few milliseconds of note-on latency
that libEmuSC does not reproduce yet, and that offset is itself a finding), and
estimate a **gain** ratio from RMS over the whole stimulus (a constant level
difference is a real divergence but a separate one from timbre; log it, then
normalise). Then:

- **Null residual** — peak and RMS of A−B in dBFS, plus the residual's
  spectrum (tells *what* differs: a tonal residual is pitch or phase, a
  broadband one is noise floor or interpolation, a transient one is timing).
- **Multi-resolution spectral distance** — log-magnitude STFT at 4096/1024/256
  windows, per-band (say 1/3-octave) mean absolute difference in dB, reported
  as a matrix over time × band. Thresholds per tier are calibrated against a
  known-inaudible perturbation (the oracle rendered with a 1-sample offset),
  not chosen by hand.
- **Envelope track** — RMS in 1 ms frames, both channels, in dB; the primary
  tool for TVA work. Reports attack time to −6 dB, decay slope, sustain level,
  release slope; differences in ms and dB.
- **Pitch track** — fundamental estimate per 10 ms frame (autocorrelation or
  YIN); reports cents deviation over time; catches tuning, pitch envelopes,
  vibrato rate/depth, bend curves.
- **Spectral centroid / filter track** — for TVF work, the −3 dB point of the
  smoothed spectrum over time, which is the cutoff trajectory in disguise.
- **Onset list** — detected note onsets with timing; catches voice-allocation
  and latency divergence (a stolen voice is a missing onset).
- **Stereo** — inter-channel level and correlation per frame; catches pan law
  and chorus/reverb stereo structure.
- **Perceptual summary** — an off-the-shelf open metric as a *tie-breaker*
  only (ViSQOL-style); the listening test in §6 is the authority.

The output per stimulus is one row: tier reached, and each metric's summary
number, so progress is a scoreboard and not an impression.

## 4. The phases

Each phase is a family of generated stimuli, the divergences it isolates, the
inference it supports about what the hardware does, and where the finding
goes in libEmuSC. They are ordered so each one is measured on top of a
converged previous one — an envelope cannot be fitted while the sample under
it is wrong.

### Phase 0 — infrastructure (days)

Build the candidate renderer, the stimulus generator, the toolkit, the results
table. Verify: oracle determinism (known), candidate determinism (unknown),
alignment accuracy on a trivial stimulus, and that a **null comparison** —
oracle versus oracle rendered with a 1-sample delay — scores T1, so the
metrics are calibrated against something known-inaudible before they judge
anything real.

### Phase 1 — which sample plays, at what pitch, at what level (1–2 weeks)

*Stimuli*: every tone in the GS map (capital tones and their variations, 300+)
× 6 keys spanning the range × 3 velocities, one note each, 2 s held, 1 s
release, dry (reverb and chorus send 0 via CC91/CC93 and the GS part
parameters), no LFO reachable (mod wheel 0). Plus every drum kit × every note.

*What it isolates*: sample selection (key ranges, velocity splits), loop
points, base tuning, sample-level gain, pan per tone, and the **interpolation
algorithm** — a low-pitched sample played high produces imaging products whose
pattern is a fingerprint of the interpolator (linear, 2-point, cubic, sample
rate conversion ratio). The residual spectrum on a held note tells you which.

*Inference*: these are the parts libEmuSC reads out of the control ROM, so
most should already agree; the divergences found here are parsing errors and
the known loop-length-exceeds-sample-length cases (the Orchestra kit's Concert
Cymbal), whose correct behaviour is now *observable*: render it, look at what
the address generator does at the loop point, reproduce that.

*Expected tier*: T1 broadly; T0 on some tones if the interpolator is
identified exactly and the sample path involves no other arithmetic. This is
the phase where bit-exactness is actually plausible, and worth pursuing there
because it makes every later phase's residual cleaner.

### Phase 2 — the voice: envelopes, filter, LFOs, followers (4–8 weeks)

*Stimuli*, all on a converged Phase-1 tone set (pick ~20 tones covering the
envelope/filter design space, all velocities 1..127 in steps of 8, keys
across the range):
- TVA: held notes with note-off at fixed times; **envelope track** gives
  attack/decay/sustain/release shapes per velocity and key. Segment shapes
  (linear in dB? exponential? stepped at an update rate?) come out of the
  track directly — a stepped envelope shows its update period as a staircase.
- TVF: same notes; **filter track** gives cutoff trajectory and its velocity
  and key sensitivity — the exact gap libEmuSC's `tvf.cc` names
  ("Cutoff freq V-sens"). Resonance from the peak height. Filter *topology*
  (order, response shape) from a sustained note on a bright sample: fit
  2-pole/4-pole candidates to the measured magnitude response.
- Pitch envelope: **pitch track** on tones with audible pitch envelopes
  (many percussive ones); initial offset and decay shape.
- LFOs: mod wheel at several depths; pitch track gives vibrato rate, depth,
  delay and waveform (triangle vs sine has a visible harmonic signature in the
  pitch track); TVA/TVF LFO from envelope/filter tracks likewise.
- Key/velocity followers: the same measurements across keys/velocities give
  the follower curves as data.

*Inference*: this is curve fitting against measured trajectories — a
well-conditioned problem. The hardware's envelope arithmetic is fixed-point
and updates at a rate set by the firmware timer; both the step size and the
update period are visible in a high-resolution envelope track, so the
reconstruction can be *exact in shape* even without knowing the arithmetic.
Whether libEmuSC reproduces it exactly depends on it adopting the same
update rate and quantisation, which is a design decision inside libEmuSC.

*Expected tier*: T2 across the board, T1 for many tones once envelopes are
stepped identically. This phase resolves most of the "sounds a bit off" that
motivates the whole exercise.

### Phase 3 — the firmware's control law (4–8 weeks, overlapping Phase 2)

Everything the CPU does between a MIDI byte and a chip register. None of it
is in the ROM tables; all of it is observable.

*Stimuli*:
- Controllers one at a time on a reference tone: CC7 volume (the dB curve),
  CC11 expression, CC10 pan (the pan law — measure L/R gain across all 128
  values), CC1 modulation depth curve, CC64 hold, CC65/CC5 portamento (time
  curve, the "PORTAMENTO NOT COMPLETE" gap), CC91/93 send curves.
- Pitch bend: full sweep at each RPN 0 range setting; pitch track gives the
  curve and any stepping.
- RPN/NRPN: every documented one (vibrato rate/depth/delay, TVF cutoff and
  resonance offsets, envelope A/D/R offsets, drum instrument pitch/level/pan/
  sends), each swept through its range on a reference tone; the sensitivity
  curves come straight out of the tracks.
- GS SysEx: master volume/tune/pan, part parameters (the whole 0x40 0x1x
  block), scale tuning, reset behaviour (GM vs GS reset leaves different
  states — measurable by rendering the same notes after each).
- **Voice allocation** — the one that bites in real music: the SC-55 has 24
  voices (28 on the mkII) and steals under load. Stimuli: N simultaneous
  held notes for N from 20 to 40, across one part and across several parts,
  with and without GS voice reserve settings; the **onset list** and envelope
  tracks show which note was stolen, when, and how (hard cut vs fast release).
  The stealing policy is a small decision procedure and it can be inferred
  from enough cases; it is a named gap in libEmuSC (`FIXME: Reduce voice
  count`, `TODO: Prioritize parts`).
- **Timing**: note-on to first sample, across polyphony and MIDI density;
  the firmware's processing latency and its variation under load are part of
  the sound of a dense passage.
- Running status, active sensing, SysEx in the middle of a stream, and the
  "all notes off" / "reset all controllers" semantics.

*Inference*: mostly deterministic mappings recoverable from sweeps; voice
stealing needs a hypothesis-and-test loop but is finite.

*Expected tier*: T2 on all controller behaviour; voice stealing either exact
or documented-divergent (T4) — some of it may depend on internal firmware
state (note age counters) that only long random tests expose.

### Phase 4 — effects: reverb and chorus (6–12 weeks; the long tail)

*Stimuli*: a short percussive tone with the dry path suppressed as far as the
GS parameters allow (part level low, send 127, effect level 127), one hit,
10 s of tail, for each reverb type (Room 1–3, Hall 1–2, Plate, Delay,
Panning Delay) and chorus type (Chorus 1–4, Feedback, Flanger, Short Delay,
Short Delay FB), then each effect parameter (level, time, feedback, pre-LPF,
delay) swept. The result is a **measured impulse response** per setting (the
residual dry component is estimated from a send-0 render and subtracted).

*Inference*: an IR reveals a delay network's tap positions, feedback gains,
diffusion structure and any pre-filter — the classic reverb reverse-
engineering exercise, and it converges to *a* network that produces the same
IR to within measurement noise. Chorus is a modulated delay; its LFO rate and
depth and its stereo structure come out of the pitch and stereo tracks.

*Expected tier*: **T3, not T1.** Two delay networks that differ in
implementation detail (accumulator width, tap rounding, modulation
quantisation) produce residuals that are large in the sample domain and
inaudible. This is the phase where the ladder's distinction earns its keep:
insisting on T1 here would consume unbounded effort for no audible return.

### Phase 5 — the mix bus and output stage (1–2 weeks)

*Stimuli*: many parts at maximum level to drive the summing bus into
**clipping** — the SC-55's 20-bit accumulator saturates in a particular way
that dense GS arrangements actually reach; a full-scale test of pan law on the
bus; master volume steps; silence (noise floor, DC offset, any dither); and
the output rate behaviour (the mkII's 66207 Hz and the oversampling switch
the firmware controls — observable as a change in the output spectrum's
upper band).

*Inference*: saturation curve, bus headroom and rounding are measurable from
controlled overload. libEmuSC sums in float and would need an explicit model
of the fixed-point bus to match; this is small code but it is the difference
between "dense passages sound harder" or not.

### Phase 6 — the corpus (continuous)

Render all 37 reference files plus the appliance corpus through both engines
after every change; score every file on the ladder; keep the history. Two
rules: the corpus is never used to *fit* anything (that is what the
stimuli are for), and a regression on any file blocks the change that caused
it. The final exam is passed when every file is T3 and the T4 list is empty
or consists only of documented, accepted divergences.

## 5. The scoreboard

One table, versioned in the repository, one row per stimulus and per corpus
file: `id, phase, tier, gain_dB, offset_samples, null_peak_dBFS,
null_rms_dBFS, spec_dist_max_dB, env_err_ms, env_err_dB, pitch_err_cents,
onsets_missing, notes`. Progress is the tier histogram over time. Every
divergence that is *not* going to be fixed gets a `notes` entry saying why,
so the T4 residue is a documented list rather than a shrug.

## 6. Listening on the rigs

For T3 — the tier that decides the goal — a protocol, not an impression:

- Same MIDI, live over the DIN wire from the host (the feed verified in this
  session), to `midi-engine` on the same box switching between the two
  engines, so the output stage is identical.
- Level-matched to within 0.1 dB LUFS (measured, from the offline renders).
- **ABX**: the listener hears A, B, then X (randomly A or B) and names it;
  at least 16 trials per stimulus; the engines are "identical" for that
  stimulus if the listener is at chance (p > 0.05). Two listeners minimum;
  headphones on the DAC, not the jack, for the decisive runs.
- Stimuli for listening: the corpus files (in 30-second excerpts chosen where
  the offline metrics show the largest T2 residuals — listen where the
  numbers say it is worst) and the isolated tones that Phase 2/4 could not
  bring to T1.

The Pi 4 renders both engines comfortably; the Pi 3 is where libEmuSC's
performance is then confirmed, which is the other half of why this is worth
doing at all.

## 7. Feasibility: how close is close

### T0, bit-exact: not by this route

Nuked-SC55's output is the product of a fixed-point pipeline — a hardware
address generator, an interpolator, envelope and filter arithmetic on
20-bit-ish accumulators, a saturating summing bus, and two delay-network
effects — driven by firmware whose control decisions are timed by an
interrupt scheduler. Reproducing that bit-for-bit means reproducing the
rounding at every stage and the exact order and timing of every update.
From the outside, that is an inverse problem with far more unknowns than
equations: many different internal quantisation choices produce outputs that
differ by a bit or two per sample, which is exactly the regime where the
output stops telling you which one is right. Phases 1 and (partly) 2 can
reach T0 because their arithmetic is shallow; Phases 4 and 5 cannot, not for
lack of effort but because the information is not in the signal at a
recoverable level. And libEmuSC is a float engine at arbitrary sample rate by
design; matching T0 would mean rebuilding it as a fixed-point engine at
66207 Hz — at which point one has re-derived the chip model that Nuked
already is, by a slower path. The honest reading: **T0 is the wrong goal**,
and the tier ladder exists so that nobody spends a year on it.

### T1/T2 on isolated stimuli: yes, with effort

Phases 1–3 and 5 are well-conditioned measurement problems with finite
parameter spaces, and most of the findings land in code libEmuSC already has
(`tva`, `tvf`, `pitch`, `envelope`, `synth`) as *corrections to parameter
interpretation* — which is precisely how its own authors characterise their
gap list. The realistic outcome is T1 on most dry tones, T2 on the rest, and
a filled-in answer to every `TODO: Verify!` in that gap list, obtained without
reading a line of the black box.

### T3 on the corpus: plausible; the effects decide it

Real music is dry voices plus reverb and chorus; once Phases 1–3 converge the
dry component will pass, and the corpus verdict rests on Phase 4. Perceptual
matching of a delay-network reverb from its IR is standard practice and
converges, so T3 is reachable — but the last 10% of that convergence
(diffusion density, modulation, high-frequency damping) is where each
increment costs weeks, and it should be steered by the ABX results, not by
the residual numbers.

### Effort and shape

- Phase 0: **days**. Everything it needs exists or is trivial.
- Phases 1–3 and 5: **3–5 months** of part-time work, most of it inside
  libEmuSC, in collaboration with its maintainer — this is not a fork-and-
  match job, both because the LGPL obligations make upstreaming the right
  move and because the maintainer already knows which questions to ask.
- Phase 4: **2–3 months** to T3 on the common types; open-ended beyond.
- Phase 6 runs throughout; the "final exam" pass is realistically **9–12
  months** away with steady effort, sooner with two people, never with T0 as
  the criterion.

### Risks worth naming

- **Provenance.** Findings derived from measuring Nuked's output are clean;
  libEmuSC's maintainer may still ask, and should be told the method before
  the first patch, not after. This document is the answer to that question.
- **The ROMs are still Roland's.** "Pure free" describes the emulator's
  code; both engines interpret the same copyrighted ROM contents and always
  will. Nothing here changes that.
- **libEmuSC's architecture** may resist some findings (the fixed-point bus,
  a stepped envelope clock). Each such case is a design conversation upstream.
- **Overfitting to the oracle.** The oracle is itself an emulator. Where a
  real SC-55 recording is available, spot-check the oracle against it too;
  Nuked's upstream did that work, but a second look costs little.

### What this session already provides

A verified, hash-checked deterministic oracle for any MIDI stimulus; a
stimulus writer and parser; per-second instrumentation of renders; the
digest discipline; two configured Pi rigs with a verified live MIDI feed and
an engine-switching front end; and the knowledge, measured tonight, of what
the reference engine costs to run — so that when libEmuSC is close enough,
the comparison of *cost* is as rigorous as the comparison of sound.
