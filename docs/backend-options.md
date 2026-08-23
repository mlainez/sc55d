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
