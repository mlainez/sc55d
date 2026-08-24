# SIMD, GPU offload, and ARM libraries

Whether a Raspberry Pi's vector unit, its GPU, or an off-the-shelf ARM-optimised
library can be pointed at this workload. Short answers first:

| Idea | Verdict |
|---|---|
| NEON across the 32 PCM voice slots | **No** — the slots are a serial, order-dependent accumulator chain |
| NEON inside a single slot | **Marginal** — the work is branchy bit manipulation, not arithmetic |
| VideoCore GPU offload | **No** — the MCU and PCM chip interlock per instruction; nothing can be batched |
| ARM-optimised DSP libraries for the core | **No** — this is bit-exact hardware emulation, not signal processing |
| ARM-optimised resampler for the *output* | **Yes** — this is the one real DSP stage, and it is a config change |
| Table-driving fixed bit permutations | **Yes, done** — `patches/0002`, 5.1x faster startup |

All line references are to the `vendor/nuked-sc55` submodule.

## Why this is so much heavier than mt32-pi

mt32-pi runs comfortably on a Pi 3. It is reasonable to ask why an SC-55 cannot.
The answer is not that mt32-pi is better written — it is solving a fundamentally
cheaper problem.

**Munt, which mt32-pi uses, is a high-level emulator.** It reads the MT-32's
ROMs and reimplements LA synthesis — partials, envelopes, filters — as ordinary
DSP code. It does not execute the MT-32's firmware instruction by instruction,
and it does not model its synthesis chip gate by gate. It computes the samples
the hardware would have produced.

**Nuked-SC55 is a low-level emulator.** It runs the SC-55's H8/532 firmware one
instruction at a time, runs a *second* CPU (the sub-MCU) alongside it on the
mk2, models the PCM chip's 32 voice slots at gate level 33103 times a second,
and steps the timers every two MCU cycles. It does not compute what the hardware
would produce; it simulates the hardware and lets the samples fall out.

Measured by others on the same machine with the same MIDI file: **Munt in
integer mode 5–10% CPU, Nuked-SC55 30–40%.** Roughly a 5x difference, and it is
inherent to the approach rather than a defect to be optimised away. nukeykt has
said he intends to write a *low-level* MT-32 emulator "similar to Nuked SC-55",
which is the clearest confirmation that Munt is not one — and when that exists,
it will be expensive too.

One more difference that is *not* the reason: mt32-pi is bare metal, on Circle,
with no Linux underneath. That buys it superb jitter behaviour, but jitter is
not our problem. Ours is raw throughput, and bare metal does not make
instructions cheaper.

### What that means for a Pi 3

The most useful real-world datapoint comes from
[PR #51](https://github.com/nukeykt/Nuked-SC55/pull/51) against upstream, whose
author optimised specifically for this: they got a **Pi 4 at stock 1.8 GHz to
85–90% of one thread**, which they described as making it "usable". On an M1 the
same work took CPU from 39–41% down to 26–28%.

A Pi 3 is about 1.2 GHz, and its Cortex-A53 is *in-order* where the Pi 4's A72 is
out-of-order — call it 0.6x the work per clock, and 0.67x the clock. That puts a
Pi 3 somewhere around **2.5–3.5x short of realtime**, optimised. Nothing in this
document closes a gap that size.

So: a Pi 4 is plausible and a Pi 5 is comfortable, but **a Pi 3 running
Nuked-SC55 in realtime looks out of reach**, and honest planning should assume
it. `scripts/pi-check.sh --roms <dir>` on the actual board is what settles it.

### What is worth borrowing

PR #51 is the closest thing to prior art for our exact problem. Its five
techniques, and what each is worth to us:

| Technique | Verdict |
|---|---|
| Lookup tables replacing conditionals | **Measured, rejected.** Tried on the hottest such site, `PCM_ReadROM`; slightly slower, because the branches are perfectly predictable and a table adds a dependent load. See `patches/README.md`. |
| Re-sorting `if`/`switch` cases by hit frequency | **Already automated, better.** This is hand-rolled PGO. We wire real PGO up via `-DSC55D_PGO=generate/use`, which derives the same layout from measurement rather than guesswork. |
| `inline` on hot functions | **Subsumed by LTO**, which is on by default. |
| Removing interrupt checks | **Unsafe.** The author had to re-enable them for compatibility. |
| Adjusting sub-MCU cycle counts | **Unsafe.** Changing cycle counts changes timing, which changes output. Not bit-exact. |

Two of the five are correctness hazards, two we already do automatically, and
the one genuinely novel idea measured negative here. Some of that reported 35%
was very likely bought with the two unsafe ones. PR #51 was never merged — it
was closed in April 2024 over the licence, and the author deleted their fork —
and jcmoyer's fork does not carry it either; its only performance entry is the
interrupt-handling work worth 10–16%.

### The real lesson from mt32-pi

If a Pi 3 is a hard requirement, the answer is not to keep shaving the
gate-level emulator. It is to move down the accuracy dial, exactly as mt32-pi
did by using Munt.

**Munt itself is not an option.** It emulates Roland's LA synthesis — the
MT-32, CM-32L, CM-64 and LAPC-I, all pre-General-MIDI. The SC-55 is a different
machine entirely: a PCM ROMpler with its own sample set, effects and the GS
dialect. Munt cannot load SC-55 ROMs, and there is no shared engine to port;
adapting it would mean writing a Sound Canvas synthesiser from scratch.

Somebody already did that: [EmuSC](https://github.com/skjelten/emusc) extracts
the control and PCM ROM data and reimplements the synth's behaviour in modern
C++ — Munt's approach applied to the Sound Canvas. It ships **libEmuSC** as a
separate library, which would suit sc55d well, since only `src/core.cpp` is
emulator-specific.

The catch is accuracy. Its own README says there are "still some significant
shortcomings in the generated audio, varying on which instruments and settings
are being used", and points people at Nuked-SC55 or SC-55 SoundFonts for better
results. So it is a real trade, not a free win.

The third option is what mt32-pi does for everything that is not an MT-32:
FluidSynth with an SC-55 SoundFont. Cheapest by far, entirely adequate for many
uses, and not authentic — the samples are there but none of the module's voice
handling or effects behaviour is.

Worth noting: sc55d is structured so that this would be a contained change.
Everything outside `src/core.cpp` — ALSA sequencer input, ALSA output, the
realtime setup, the benchmark — is emulator-agnostic. `Core` is a thin wrapper
over one backend and could wrap another.

## What the inner loop actually is

Per emulated instruction the core runs the MCU interpreter, then advances three
peripherals to the current cycle count. Per PCM tick — 33103 of them a second on
an SC-55mk2 — `PCM_Update()` walks up to 32 voice slots, each doing five wave
ROM reads and a few hundred boolean and shift operations.

This is a gate-level model. It is not a filter bank, and that distinction is
what decides every question below.

## NEON

### Across slots: blocked by the accumulator chain

The obvious vectorisation is four slots at a time in a 128-bit register. Three
things in `pcm.cpp` prevent it, in descending order of severity.

**1. The slots accumulate serially into shared state, non-associatively.** Each
slot folds its output into `pcm.ram1[31][1]`, `pcm.ram1[31][3]`, `pcm.rcsum[0]`
and `pcm.rcsum[1]` through `addclip20()`:

```c
constexpr inline int32_t sx20(int32_t in)          { return (in << 12) >> 12; }
inline int32_t addclip20(int32_t a, int32_t b, int32_t cin)
                                                   { return sx20(a) + sx20(b) + cin; }
```

`sx20()` sign-extends from 20 bits, so every accumulation step truncates its
inputs to 20 bits before adding, and the running total is stored back into a
`uint32_t` and re-extended on the next read. **That operation is not
associative.** Reassociating the 32 additions into a SIMD tree reduction gives
different bits wherever an intermediate crosses the 20-bit boundary — which is
exactly where the hardware's wrapping behaviour is audible. Bit-exactness is the
entire point of this emulator, so a reduction that is "nearly right" is a
regression, not an optimisation.

Worse, *which* accumulator a slot feeds rotates with its index, through a switch
on `slot2 = slot + 1`. The dependency pattern is not even uniform across lanes.

**2. Slots read each other's state, indirectly.** The phase increment comes from
another slot chosen at runtime:

```c
sub_phase += pcm.ram2[ram2[7] & 31][0];
```

That is a gather with a data-dependent index. AArch64 NEON has no gather
instruction; it would become four scalar loads and inserts, giving back most of
what vectorising won.

**3. The layout is array-of-structures.** State lives in `ram1[32][8]` and
`ram2[32][16]`, so one slot's fields are contiguous and the same field across
slots is strided. Vectorising wants the transpose. `ld4`/`st4` can interleave on
the fly, or the core could be restructured to structure-of-arrays — but that
touches every access in a 500-line function whose correctness is the product of
years of hardware reverse-engineering.

### Within a slot: the work is the wrong shape

Even setting the above aside, look at what a slot actually does: dozens of
single-bit tests, XORs and conditional increments modelling address generation
and nibble selection. NEON is wide arithmetic on independent lanes; this is
scalar control flow. Making it branchless for predication means computing every
path and selecting, which on an in-order A53 can easily cost more than the
branches it removes — the same trap that killed the `PCM_ReadROM` idea recorded
in `patches/README.md`.

The genuinely arithmetic parts — `multi()` (a 20-bit by 8-bit multiply), the
interpolation against `interp_lut`, the envelope in `calc_tv()` — are a small
slice of the per-slot cost and are chained into the same serial accumulation.

### And half the workload is not PCM at all

Profiling put `TIMER_Clock` at 29% and `SM_Update` at 12%, with the MCU
interpreter under-represented because the profile ran without real ROMs. An
instruction-set interpreter with an indirect dispatch table cannot be vectorised
at all. Even a heroic, bit-exact, fully vectorised `PCM_Update` leaves the
majority of the runtime untouched.

**Conclusion.** NEON is not where the wins are. A correct SoA rewrite of the
voice loop with branchless predication is weeks of work with a permanent
bit-exactness risk, against maybe a third of the runtime, and the biggest single
hotspot found so far (`TIMER_Clock`) is a scalar algorithmic fix worth 7–13% for
25 lines.

## Does any of this use more than one core?

Not for a single SC-55, and it cannot.

**One instance is strictly serial.** The MCU, the sub-MCU, the PCM chip and the
timers are advanced to the same cycle count after every emulated instruction,
and any of them can be written by the others in between. Splitting them across
cores would need a synchronisation barrier roughly every 12 emulated cycles,
which costs far more than the work being parallelised. This is the same coupling
that rules out a JIT and GPU offload.

**sc55d uses two threads today**: the render-and-output thread, and the ALSA
sequencer thread, which is nearly idle — it wakes on a MIDI event, decodes a few
bytes and sleeps. So one core does essentially all the work, and on a four-core
Pi the other three are idle.

**The fork does support multiple instances, each on its own thread** — up to 16,
with `StartThread()`/`JoinThread()` per instance and MIDI routed per instance or
broadcast. That is genuine multi-core use, but it is *N independent SC-55s*, not
one faster SC-55: each has its own 24-voice pool and its own effects. It raises
polyphony past what the hardware could do, which is a deliberate departure from
authenticity rather than an optimisation.

Crucially, **it does not help a board that is short of realtime.** Every
instance must individually keep up with the audio clock; two instances at 0.6x
are still 0.6x. Multi-instance is for when one instance already holds realtime
and you want more voices than a real SC-55 had.

What a second core *could* usefully do is absorb jitter rather than add
throughput — a render-ahead thread decoupling the emulation from
`snd_pcm_writei()`, as described under **Known ceiling** in [`performance.md`](performance.md#known-ceiling). That
buys tolerance to scheduling hiccups at the cost of MIDI latency, not headroom.

## JIT, virtualisation, and off-the-shelf CPU cores

The instinct is right — interpreting a CPU is slow, and dynamic recompilation is
the standard cure. The profile says it is aimed at the wrong quarter of the
work.

**With real ROMs, after the thirteen patches**, retired instructions divide
roughly like this:

| | share |
|---|---|
| `PCM_Update` + `PCM_ReadROM` — the PCM **sound chip** model | **~50%** |
| sub-MCU interpreter (`SM_*`) | ~16% |
| main MCU interpreter, memory and interrupts | ~6% |
| `TIMER_Sync` | ~5% |
| ROM hashing and unscrambling at startup | ~3% |

**Only about a quarter of the work is CPU emulation at all.** Half of it is
`PCM_Update`, which is not executing guest instructions — it is a gate-level
model of a sound chip, stepping 32 voice slots 33103 times a second. There is
no instruction stream there to translate. A JIT cannot touch it.

So a *perfect, zero-cost* CPU recompiler caps out around 25%, and a realistic
one — 3-5x on the interpreted part — saves perhaps 18%. The thirteen patches
already delivered 43.5% for a fraction of the effort.

**And the cycle coupling fights it.** `PCM_Update` and the timers are advanced
to the current cycle count after *every* emulated instruction, and the MCU can
write their registers on any instruction. A JIT block therefore cannot run
free; it would have to emit calls back into the peripherals at instruction
granularity, which is most of what makes a JIT fast thrown away. This is the
same coupling that rules out GPU offload, below.

**Virtualisation is not applicable at all**, and not because it is hard.
Hardware virtualisation — KVM, HVF, Hyper-V — runs a guest *of the same
instruction set* directly on the CPU. A Raspberry Pi's ARM core cannot execute
Hitachi H8/500 or the sub-MCU's 6502-derived instructions, so there is nothing
to virtualise. Virtualisation removes hypervisor overhead; it does not
translate architectures.

**Off-the-shelf cores exist but do not help.** MAME carries a preliminary
H8/500 family core under `src/devices/cpu/h8500/` (derived from its H8/300
work, with DTC unimplemented). It is an *interpreter*, like Nuked's, so it
would not be faster — and swapping it in would discard exactly the
bit-exactness that Nuked's reverse engineering exists to provide. The same goes
for any 6305/740 core for the sub-MCU. These are useful if you want to *write*
an emulator, not to speed one up.

**What the profile says to do instead**, in order:

1. `--model scb55` — deletes the sub-MCU outright, measured at −15.5% of
   retired instructions, for a command-line flag.
2. `PCM_Update`, at half the remaining cost, is the only target that matters.
   SIMD across its slots is blocked for the reasons in the NEON section; what
   is left is more of the algebraic work patches 0008-0012 did, and `calc_tv`
   is still the largest single piece of it.
3. Faster hardware. A Pi 5 is the cheapest 2x available and needs no code.

## GPU offload (VideoCore)

Compute is technically reachable — Mesa's `v3dv` gives Vulkan compute on Pi 4
and Pi 5, and VC4CL gives OpenCL on Pi 3's VideoCore IV. The API is not the
problem. The algorithm is.

**1. The MCU and PCM chip interlock at instruction granularity.** `PCM_Update()`
is called with the current cycle count after *every* emulated instruction, and
the MCU can write PCM registers on any instruction via `PCM_Write()`. The PCM
model therefore cannot run ahead of the CPU model by more than one instruction —
about 12 MCU cycles, half a microsecond of emulated time. GPU offload needs
thousands of independent work items batched into one dispatch; here the maximum
batch is one PCM tick, and even that must complete before the next instruction
executes. Dispatch and readback latency is measured in tens of microseconds; the
entire per-tick budget is about 30. The shape is wrong by three orders of
magnitude.

Decoupling would mean buffering timestamped register writes and running PCM as a
batched pass afterwards — a redesign that changes the timing semantics the
emulator exists to reproduce.

**2. Each sample feeds the next.** Within a tick there is the `addclip20()`
accumulator chain above; across ticks there is the noise LFSR
(`shifter = (shifter >> 1) | (xr << 15)`), the envelope state, and the
reverb/chorus delay line in `pcm.eram[0x4000]` with its custom 14-bit-mantissa
packing (`eram_pack`/`eram_unpack`). Feedback loops are the canonical
anti-pattern for GPU parallelism.

**3. The parallel width is 32, and they are not independent.** GPUs want
thousands of independent lanes. There are 32 slots, and per point 1 of the NEON
section they are chained.

**4. Integer bit-exactness.** VideoCore IV's QPUs are 16-lane float units whose
multiply path is 24-bit; exact 32-bit integer semantics need multi-instruction
emulation. Pi 4's VideoCore VI is better but still tuned for graphics. This is
the least load-bearing of the four objections — the first alone is decisive —
but it compounds them.

**Conclusion.** No. Not "hard": structurally wrong. The GPU would sit idle
waiting for a CPU that cannot run ahead of it.

## ARM-optimised libraries

### Not for the emulator

There is no library to substitute, because there is nothing generic here to
substitute *for*. Arm Compute Library, Ne10, CMSIS-DSP, Arm Performance
Libraries and Eigen all provide approximate floating-point primitives — FIR,
FFT, GEMM, activation functions. The core is a cycle-accurate integer state
machine whose value is that it reproduces a specific 1991 chipset bit for bit. A
faster FIR is not a faster SC-55; substituting a "good enough" DSP primitive for
an emulated gate would silently change the sound, which is the one thing this
project must not do.

The core's only genuinely library-shaped dependency is SHA-256 (bundled RFC code
in `src/backend/sha/`) for ROM identification. ARMv8 has SHA-2 instructions that
would make it several times faster — but the Cortex-A53/A72/A76 parts in the
Pi 3, 4 and 5 do not implement the optional crypto extensions, so there is
nothing to reach for. Check yours with:

```bash
grep -o -m1 'sha2\|aes' /proc/cpuinfo
```

It is startup-only work in any case, and linking OpenSSL to save milliseconds
once per boot would trade away the dependency-light property that makes this
easy to package.

### Yes for resampling — the one real DSP stage

The core renders at 66207 Hz and no sound card accepts that, so **every sample
is resampled** to 48000. That is real signal processing, it runs for the life of
the process, and it is entirely outside the emulator. It is the one place an
optimised library belongs — and ALSA already makes it a configuration change,
with no code and no rebuild.

`libasound2-plugins` ships pluggable rate converters:

| Converter | Backend | Notes |
|---|---|---|
| `lavrate` (`_fast`, `_faster`, `_high`, `_higher`) | FFmpeg swresample | aarch64 NEON paths; several quality tiers |
| `samplerate` (`_linear`, `_medium`, `_best`, `_order`) | libsamplerate (SRC) | `_linear` is cheapest, `_best` is very expensive |
| `speexrate` (`_medium`, `_best`) | speexdsp | NEON paths; ALSA's usual default |

Select one in `/etc/asound.conf`:

```
defaults.pcm.rate_converter "lavrate_fast"
```

```bash
sudo apt install libasound2-plugins
```

Which is best is a measurement on your board, not a matter of opinion — the
right one is the cheapest whose artefacts you cannot hear at 66207 → 48000.
Measure with the daemon running and watch the xrun count, since this cost lands
on the same thread as the emulation. [`performance.md`](performance.md#system-tuning)'s blunt advice to use `linear`
is the safe floor; a NEON `lavrate_fast` may well give better quality for
similar cost.

Going further, sc55d could resample itself with **libsoxr** (small, NEON-aware,
packaged by Buildroot) and open the device at 48000 directly, bypassing the plug
layer. That buys control over the quality/cost trade and removes a layer, at the
cost of one dependency. Worth doing only if measurement shows the plug layer is
a real share of the budget.

### Yes for fixed bit permutations — already done

`patches/0002` replaces `unscramble()`'s per-byte bit shuffling with compile-time
tables: 5.1x faster startup, proved exhaustively over all 256 data bytes and all
8388608 addresses by `patches/tests/unscramble_equivalence.cpp`. No library
needed — just noticing that a constant permutation can be precomputed.

## The biggest remaining target — now taken

*Superseded: `patches/0001` does this. Kept because the reasoning is what
found it.*

## How the timer was fixed

After `patches/0001`, `TIMER_Clock` is *still* the largest single function in
the profile — about 30% of retired instructions. That is not waste any more; it
is real work. With the common /4 divider the three free-running timers each tick
every 8 MCU cycles, so with 12 cycles per instruction they genuinely advance
about 1.5 times per emulated instruction, and each tick compares `frc` against
`ocra` and `ocrb`, increments, sets flags and tests interrupt enables.

The way out is **closed-form advancement**. Between observable events a
free-running counter just increments, so instead of stepping it one tick at a
time the core could compute the distance to the next event —
`min((ocra - frc) mod 65536, (ocrb - frc) mod 65536, 0x10000 - frc)` per timer —
take the earliest across all four, and add. Events are thousands of ticks apart;
the loop would run a handful of times per second instead of ~40 million.

That is plausibly a **~25% overall win**, the largest left on the table. It is
also a real rewrite of the timer's state machine rather than a 25-line skip, and
it has to reproduce the flag and interrupt-request behaviour exactly. It should
be built the same way 0001 was: a differential harness that drives a modelled
and a real timer side by side over random register programming, checking full
state after every call.

## The wave ROM is the Pi 3 cache risk, and this benchmark hides it

Simulated under a Cortex-A53-like configuration (`--I1=32768,4,64
--D1=32768,4,64 --LL=524288,16,64`), the emulator looks **entirely
cache-resident**: `TIMER_Clock` takes 18 D1 read misses over 637M reads,
`SM_Update` 25 over 270M, `MCU_Interrupt_Handle` 2.87% of all write misses are
the one-time zeroing of `pcm_t` (15.7 MB) and `mcu_t` (665 KB) at startup.

**That result is an artefact and should not be believed.** `PCM_ReadROM` shows
60 D1 misses out of 74.3M reads — because with a zero wave ROM the firmware
never programs a voice, so it reads essentially one address 14.9M times. On real
ROMs those same ~10.6M reads per second are spread by up to 28 voices across
2–4 MB. Modelled under the same cache geometry with 28 streams over a 4 MiB ROM:

| byte stride (set by pitch) | D1 miss | LL miss | implied Pi 3 stall per second of audio |
|---|---|---|---|
| 1 | 3.4% | 1.1% | ~11 ms |
| 4 | 5.6% | 3.4% | ~36 ms |
| 16 | 14.6% | 12.8% | ~136 ms |
| 64 | 50.6% | 50.3% | ~533 ms |

So wave ROM access is worth somewhere between **1% and 50% of a core** depending
on how far the music is pitched up, and nothing in the emulator's own data
structures comes close to it. It is invisible to every measurement in this
repository. If a Pi is short of realtime and the profile does not explain why,
this is the first place to look — and prefetching the next sample address per
voice, which is predictable from the phase accumulator, is the obvious
experiment.

(`MCU_Read`'s 98,131 misses are the same kind of artefact in reverse: `pc`
sweeps all 64 KB of page 0 forever because the ROM is zeros. Real firmware has
locality.)

## Where the per-instruction time goes

1,231 retired instructions per emulated MCU instruction, before any patches:

| | Ir/instruction | share |
|---|---|---|
| `PCM_Update` | 439 | 35.7% |
| `TIMER_Clock` | 361 | 29.4% |
| `SM_Update` | 222 | 18.1% |
| `MCU_Interrupt_Handle` | 54 | 4.4% |
| `MCU_Step` own body | 30 | 2.4% |
| code fetch via `MCU_Read` | 25 | 2.0% |
| opcode dispatch | 2 | 0.2% |

Inside `pcm.cpp` with all 32 voices sounding, the slot loop body is 45% and
**`calc_tv` is 31%** — not the 3.7% an earlier profile suggested, which counted
only the out-of-line clone and missed the two copies GCC inlines. `sx20` /
`addclip20` / `multi` are 13%, though that is inflated on x86: `(in << 12) >> 12`
is two instructions there and a single `sbfx` on AArch64.

## Measuring this yourself: two traps

Both cost a subagent real time here.

**Do not install `valgrind:arm64` alongside the x86 one.** Valgrind is not
`Multi-Arch: same`, so the arm64 package *replaces* `/usr/bin/valgrind.bin` and
silently breaks every x86 callgrind run — wrong numbers, no error. It would not
have helped anyway: valgrind does not run under `qemu-user`.

**`qemu-user` here has no TCG plugins**, so there is no way to get real aarch64
instruction counts: `-plugin` is unrecognised and there is no `libinsn.so` in
the image or the archive. Trace-counting with `-d exec,nochain` is arithmetically
possible and needs about 20 GB of log per benchmark run. The workable fallback
is static analysis — `objdump` the cross-built binary and count instructions per
path by hand, which is how the A53 figures in `patches/candidates/README.md`
were obtained.

## Where the effort actually belongs

Ranked by measured evidence rather than novelty:

1. **Romset choice** — `scb55` has no sub-MCU: **−15.5% of retired
   instructions**, measured, for a command-line flag. Nothing else here comes
   close.
2. **Build flags** — LTO (+8.9% without it) and `-O3` (+11.5% at `-O2`) are
   already the defaults and are worth having. PGO measured at only −1.3% and
   inverts on an untrained workload; the cheap flags measure at exactly zero.
   Table in [`performance.md`](performance.md#build-flags).
3. **`patches/0001`** — `TIMER_Clock` closed-form advancement. 2295M → 38M
   retired instructions. Differential test plus 9/9 mutants; real-ROM audio
   still unchecked.
4. **`patches/0002`** — startup, 5.1x, proved exhaustively.
5. **`patches/candidates/0001`** — the sub-MCU timer loop, worth about 1% on an
   mk2 *if* the firmware's prescaler reload is 3 or more, and a regression
   below that. One measurement on real hardware decides it.
6. **Re-profile on the target with real ROMs.** Everything above was measured
   on x86-64 with placeholder ROMs. Everything above was measured on
   x86-64 with placeholder ROMs, which under-represents the MCU interpreter. The
   ranking could change; `--bench` and callgrind are all it takes.

Not on this list: NEON and the GPU.
