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
on the same thread as the emulation. The README's blunt advice to use `linear`
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

## The biggest remaining target

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

## Where the effort actually belongs

Ranked by measured evidence rather than novelty:

1. **Build flags** — `-mcpu`, LTO, PGO. Free, and PGO in particular suits a
   dispatch-table interpreter. See the README.
2. **`patches/0001`** — `TIMER_Clock` edge skipping, +11%. Proved by
   differential test; still needs real-ROM validation.
3. **Romset choice** — `scb55` has no sub-MCU, deleting 12% of the workload
   outright with no patch at all.
4. **`patches/0002`** — startup, done and proved.
5. **Closed-form timer advancement** — see above, ~25%, the biggest remaining
   item and the only one that needs real design work.
6. **Re-profile on the target with real ROMs.** Everything above was measured on
   x86-64 with placeholder ROMs, which under-represents the MCU interpreter. The
   ranking could change; `--bench` and callgrind are all it takes.

Not on this list: NEON and the GPU.
