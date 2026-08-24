# Moving off Nuked-SC55: what a high-level backend would mean

An investigation into replacing the emulation core, prompted by the observation
that mt32-pi runs on a Pi 3 and this does not.

**Summary:** Munt cannot be used for the SC-55 at all — it is structurally an
MT-32 emulator. The real candidate is **libEmuSC**, and it is a more serious
option than expected: contained to swap in, faster by construction, renders at
whatever sample rate we ask for, and — the finding that may matter most —
**LGPL-2.1, with none of Nuked-SC55's non-commercial restriction**. The cost is
fidelity, which its own authors are upfront about.

## Why not Munt

Munt's engine is named after the chip it emulates. `mt32emu/src/` contains
`LA32WaveGenerator`, `LA32FloatWaveGenerator`, `LA32Ramp`, `Partial`,
`PartialManager`, `Poly`, `BReverbModel`, and a `ROMInfo` table that identifies
MT-32/CM-32L ROMs by hash. There is no generic sample-playback path and no
configurable instrument model — it is Roland LA synthesis, end to end.

The SC-55 is a PCM ROMpler with a different voice architecture, different
effects and the GS dialect. "Configuring Munt for the SC-55" is not a small
project; it is writing a Sound Canvas synthesiser and keeping none of Munt.

## The actual candidate: libEmuSC

[EmuSC](https://github.com/skjelten/emusc) is Munt's approach applied to the
Sound Canvas: read the control and PCM ROMs, reimplement the synthesis in C++.
It splits into `EmuSC` (a Qt desktop app, GPLv3+) and **`libEmuSC`** (the
engine, **LGPLv2.1+**), which is the part we would use. Actively developed —
copyright headers run to 2026.

Its `libemusc/src/` is a full synth: `control_rom`, `wave_rom`, `part`,
`partial`, `note`, `tva`, `tvf`, `envelope`, `pitch`, `svf`, `wave_generator`,
`wave_oscillator`, `resampler`, `chorus`, `reverb`, `system_effects`. It knows
about SC-55mkII and SC-88, not just the original SC-55 — better coverage than
some summaries suggest.

### The API fits us well

```cpp
Synth(ControlRom &cRom, WaveRom &pRom, SoundMap map = SoundMap::GS);
void midi_input(uint8_t status, uint8_t data1, uint8_t data2);
void midi_input_sysex(uint8_t *data, uint16_t length);
int  get_next_frame(float &lOut, float &rOut);
void set_audio_format(uint32_t sampleRate, uint8_t channels);
void reset(SoundMap sm, bool resetParts = false);
void panic(void);
```

Three things stand out.

**`set_audio_format(48000, 2)`** — it renders at whatever rate you ask. The
66207 Hz problem disappears, and with it the ALSA plug layer's resampling on
every sample, which is real CPU we currently spend on the render thread. We
would open the device directly at its native rate.

**`midi_input(status, data1, data2)`** takes parsed messages. Our ALSA
sequencer thread currently decodes events *back into raw bytes* to feed the
emulated UART; against libEmuSC we would pass the sequencer's own fields
straight through and skip that step.

**`get_next_frame()`** is a pull model. The render loop gets simpler: no
instruction stepping, no frame accounting, no sample callback.

## What the port would touch

sc55d was built with the emulator behind a thin `Core` interface, so the blast
radius is small:

| File | Change |
|---|---|
| `src/core.cpp` | Rewritten against `Synth`. The bulk of the work, on the order of 250 lines. |
| `src/midi_in.cpp` | Simplified — pass sequencer fields through instead of decoding to bytes. |
| `src/main.cpp` | Options: romset names and ROM loading differ. |
| `src/audio_out.cpp` | Unchanged, but now opened at 48 kHz with no resampling. |
| `src/rt.cpp`, `src/bench.cpp` | Unchanged. mlockall, SCHED_FIFO, affinity, the stress sequence and the digest all still apply. |
| `scripts/`, `contrib/`, docs | Unchanged. |

What becomes dead: `patches/` (both patches are Nuked-specific) and the
build-time patch machinery. The equivalence tests go with them.

Keeping **both** backends is also viable — `Core` is already the seam. Nuked
for fidelity on a Pi 4 or 5, libEmuSC for a Pi 3, chosen at build time. That
costs an abstraction we already have and keeps the accuracy option open.

## The licence, which may be the real headline

Nuked-SC55 is under the pre-2016 MAME licence: **no selling, no commercial
product or activity**, and GPL-incompatible. For Spin42 that rules out anything
with an invoice attached.

libEmuSC is **LGPL-2.1-or-later**. Ordinary free software, commercial use
allowed, with the usual LGPL obligations — publish modifications to the
library, and let users relink it, which dynamic linking satisfies. Not legal
advice, but the difference between "cannot be sold" and "can be sold under
conditions" is a different category of project.

If this were ever to become a product rather than a hobby build, that alone
would probably decide it.

## The cost: fidelity

EmuSC's README is candid: *"There are still some significant shortcomings in
the generated audio, varying on which instruments and settings are being
used"*, and it points readers at Nuked-SC55 for the best available emulation.
So this is a genuine trade — cheaper and legally freer, in exchange for a
synth that does not yet sound exactly like the hardware.

## Exactly what the gaps are

Read out of `libemusc/src/` rather than inferred. They cluster tightly, and the
cluster is informative: almost everything is *parameter interpretation*, not
missing machinery. The synth structure is all there — `tva`, `tvf`, `pitch`,
`partial`, `envelope`, `svf`, `chorus`, `reverb`, `wave_generator` — and what is
missing is knowing precisely what the hardware does with each value.

**Filter (tvf.cc)**
- `TODO: Add support for Cutoff freq V-sens` — velocity sensitivity of the
  cutoff frequency is simply not implemented.
- `TODO: Add test for PD#4 on TVF envelopes` — an envelope phase is unverified.
- Uncertainty about how the cutoff key-follow relates to time key-follow.

**Amplitude envelope (tva.cc)**
- `TODO: Move over to proper slew function for envelope output` — the envelope
  output shaping is known to be an approximation.
- `TODO: Rounding error has been observed` — a measured divergence from
  hardware, not yet explained.
- `TODO: Apply fade to dynamic volume if we are in portamento mode`.

**Pitch (pitch.cc)**
- `TODO: PORTAMENTO NOT COMPLETE!`
- `TODO: Is there a pre-run of the envelope logic to find _deltaInc?` — an open
  question about how the hardware initialises pitch envelopes.
- `TODO: Verify if this is only supposed to happen on mk2 models` — a
  model-specific behaviour guessed at.
- Portamento/legato needs cross-note state (which keys are active, what
  envelope phase each is in) that the current structure does not expose.

**Samples (partial.cc)**
- `FIXME: A few sample definitions in the SC-55 ROM have loop length > sample
  length. This makes EmuSC crash as it loops outside range. The following hack
  prevents a crash, but audio is wrong for these samples. TODO: Figure out why
  this works on the real hardware.` With a worked example: Concert Cymbal, #59
  of the Orchestra drumkit.

**Voice allocation (synth.cc)**
- `FIXME: Reduce voice count`, `TODO: Prioritize parts / MIDI channels` — voice
  stealing does not match the hardware's rules.

**Effects and settings** — `reverb.cc` notes the firmware fades out before
starting in a way not reproduced; `settings.cc` carries a row of
`TODO: Verify!` against parameter behaviour. SC-88's 32 parts are unsupported.

Notice the shape of these. "Figure out why this works on the real hardware."
"Is there a pre-run?" "Verify if this is only supposed to happen on mk2."
"Rounding error has been observed." Every one is a question about observable
behaviour, and the author knows exactly which question needs answering. That is
a much healthier position than a vague "sounds wrong", and it means the work is
tractable — given an oracle.

The project also explicitly welcomes contributors interested in reverse
engineering, so fixes are wanted rather than merely tolerated.

## Fixing them by reading the Nuked source — do not do this

This is the obvious idea and it is the one method that must be avoided.

Nuked-SC55 is under the pre-2016 MAME licence: **non-commercial, and
incompatible with the GPL family**. Transcribing its logic into libEmuSC makes
libEmuSC a derivative work of non-commercially-licensed code. That would:

- breach the MAME terms as soon as the result ships under LGPL;
- **destroy the exact property that made libEmuSC attractive** — its LGPL
  status, and with it any possibility of Spin42 selling anything built on it;
- be rejected by the maintainer if disclosed, and be a landmine if it is not.

Copyright covers expression, not facts. How the SC-55 hardware behaves is a
fact. Nuked-SC55's particular C++ rendering of that behaviour is expression.
The distinction is the whole game here.

## The method that does work

Use Nuked as an **oracle** and an **instrument**, never as a source to copy
from.

**1. Black-box A/B.** Render identical MIDI through both and compare audio.
This localises divergence to specific instruments and settings — turning
"sounds a bit off" into "the Orchestra drumkit's cymbal is wrong, here is the
spectrum". Cheap to build: sc55d already renders deterministically and prints a
digest, so it needs little more than a `--render-wav` option and a compare
script. Useful to EmuSC upstream immediately, and it involves reading none of
Nuked's source.

**2. Instrumented tracing — the high-value one.** Nuked runs the real firmware.
Modify a local build to log the PCM chip's per-voice register writes over time,
and you get ground truth for exactly the open questions above: what cutoff the
firmware programs for a given velocity, what envelope rates it loads per phase,
what it does with a sample whose loop is longer than the sample. The trace is a
record of what the ROM does — a fact — not a copy of how Nuked computes it.
Modifying Nuked locally is permitted; only selling it is not.

**3. Implement from observations, not from source.** Write down the observed
behaviour, then implement libEmuSC's version from that write-up. If this ever
matters commercially, keep the separation deliberate: whoever reads Nuked
produces documentation, and whoever writes libEmuSC code works only from the
documentation. That discipline is worth recording as it happens rather than
reconstructing later.

### "But independent work would look similar anyway"

True, and worth being precise about, because it changes what the discipline is
for.

Two implementations of the same hardware behaviour do converge. There are only
so many ways to write a cutoff lookup, and the ROM's tables are identical for
everyone because they are the same tables. Similarity in small functions,
constants and data layout is weak evidence of copying — functional constraints
dictate most of it, and any competent assessment filters those out before
comparing.

What actually fingerprints copying is the other stuff: overall architecture and
decomposition, naming, comments, and above all idiosyncratic choices — an
unusual workaround, a quirk reproduced bug-for-bug, a variable named after
something only the original author would call it. Those are not dictated by the
hardware, so their presence needs explaining.

The inference "we could not prove we did it differently, so the discipline is
pointless" inverts what the discipline does. It is not a way to win an argument
after the fact; it is (a) the means of genuinely not copying, which is the part
that actually matters, and (b) the thing that *creates* the record that would
otherwise not exist. Observation write-ups, trace logs, commit history and who
had access to what are the evidence. Clean-room procedures exist precisely
because absence of evidence is a bad position when the accusation arrives with
a diff attached.

The realistic failure mode is not a courtroom. It is libEmuSC's maintainer
declining a contribution whose shape looks Nuked-derived — not because anyone
proved anything, but because merely suspecting it puts their LGPL library at
risk. That is a rational thing for them to do, and this community is already
sensitive to it: nukeykt has declined relicensing requests, and upstream PR #51
was closed over licence concerns with the contributor withdrawing.

Note also that the risk is not symmetric. A hobbyist attracts no attention; a
company with revenue is a different proposition, and this is exactly the class
of issue that surfaces years later during due diligence rather than at the
time.

### When any of this actually matters

Only on distribution. The MAME licence permits use and modification freely — it
restricts selling and commercial activity. So:

- **Personal box, never distributed** — read whatever you like. Nothing here
  applies. Build the thing and enjoy it.
- **Contributing to libEmuSC** — discipline required, because the maintainer's
  licence integrity is on the line, not just ours.
- **Anything Spin42 sells** — discipline required, and worth a lawyer's half
  hour before starting rather than after.

Deciding which of those three this is should come before any of the work
above.

**A cleaner oracle still: real hardware.** A physical SC-55mk2 recorded against
the same test sequences sidesteps the licence question entirely, and is the
unimpeachable reference. If one is available it should be the primary source,
with Nuked used for the internal state a real unit cannot expose.

Not legal advice — if anything is ever sold on the back of this, it is worth a
lawyer's half hour. But the engineering discipline is clear enough, and it is
cheap to follow from the start and expensive to retrofit.

## What fixing the gaps would actually cost

- A/B tooling: **days**, and useful regardless of which backend wins.
- Individual TODOs with trace data in hand: **days to weeks each**. The cymbal
  loop question is probably an afternoon once you can see the address
  generator's behaviour; portamento and voice-stealing are features, not fixes.
- The whole list: **months**, and it is upstream's roadmap rather than ours.

The realistic posture is to contribute the two or three fixes that matter for
the material you care about, not to adopt the gap list.

## Using Nuked-SC55 as the reference — the strong version of this idea

Here is where the work already done keeps its value even if the backend
changes. Nuked-SC55 is a bit-exact oracle, and an oracle is exactly what a
high-level reimplementation lacks.

**1. Objective A/B.** Render identical MIDI through both and compare. We
already have deterministic rendering and an audio digest; adding a
`--render-wav` option to sc55d would turn "sounds a bit off" into a measurable
per-instrument spectral difference. That is cheap to build and immediately
useful to EmuSC upstream.

**2. Nuked as an instrument, not just a reference.** More interesting: the
gaps in a high-level Sound Canvas are almost entirely in *parameter mapping* —
what envelope rates, filter cutoffs, pitch and effect sends the firmware
actually programs for a given note on a given patch. Nuked-SC55 runs that
firmware. Instrumenting its PCM chip model to log per-voice register writes
over time yields ground truth for precisely the thing EmuSC has to guess.
That is a reverse-engineering programme with a working oracle rather than a
black box, and it is the most valuable thing the Nuked work could be turned
into if we stop shipping it.

**3. Contribute upstream.** Any mapping fixes belong in libEmuSC, which being
LGPL we can actually build on.

Item 1 is days. Item 2 is a research project measured in months, and it is
someone's PhD-shaped hobby rather than a delivery plan — worth being clear
about that before anyone commits to it.

## Recommendation

Decide on the target hardware first, because it settles everything else:

- **Pi 4 or Pi 5, fidelity matters, non-commercial is fine** — keep
  Nuked-SC55. It works, it is exact, and the project is finished apart from
  tuning.
- **Pi 3 is fixed, or this might ever be sold** — evaluate libEmuSC. Build a
  prototype behind the existing `Core` seam and listen to it against a real
  SC-55 recording or against Nuked's output. An afternoon's spike answers the
  only question that matters, which is whether the shortcomings are audible to
  you on the material you care about.
- **Both** — keep two backends behind `Core`. More build configuration, no
  architectural cost, and it makes the fidelity/cost trade a runtime decision
  for whoever deploys it.

What should not happen is more micro-optimisation of Nuked-SC55 in the hope of
reaching a Pi 3. The gap is 2.5–3.5x and the techniques left are worth single
digits each.
