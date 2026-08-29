# Performance

Why sc55d needs the board it needs, what was measured, and what to change first
if the audio glitches. If you only read one section, read
[System tuning](#system-tuning) — and if you only run one command, run
[`--bench`](benchmarking.md).

## What the core patches buy

**Measured with a real SC-55mk2 ROM set** (`mk2-v1.01`), rendering the
benchmark's dense sixteen-part sequence for 30 seconds, against the baseline
sc55d pins: upstream's decoder2 core. On x86 the series removes **56.4% of
retired instructions** (3.198 G → 1.396 G per emulated second, callgrind) —
but a desktop already holds realtime on the stock core, so there it is a
nicety. The patches are validated bit-identical on this romset — see
[patches/README.md](../patches/README.md).

### On ARM, where it decides what runs

Worst-second realtime ratios on real boards, stock clocks, patched and
unpatched cross-compiled identically, `-mcpu=` matched to the board,
`performance` governor, no throttling:

| board | romset | stock decoder2 | patched | |
|---|---|---|---|---|
| Pi 3 (A53, 1200 MHz) | mk1 | 0.821x | 1.603x | +95% |
| **Pi 3** | **mk2** | **0.500x** | **1.236x** | **+147%** |
| Pi 4 (A72, 1500 MHz) | mk1 | 1.750x | 3.415x | +95% |
| Pi 4 | mk2 | 1.065x | 2.676x | +151% |

**The patches roughly double-to-2.5x throughput on ARM** against −56% of
instructions on x86. That is the expected direction: work removed is work
genuinely saved on an in-order A53, where a wide out-of-order core hides some
of it — and one patch (the hot-field layout) is exactly 0.00% on x86 and +4.7%
on the A53, which is why it exists at all.

Two rows carry the whole argument for this repo existing:

- **mk2 on a Pi 3 renders at 0.500x on the stock core** — half of realtime.
  Patched it holds 1.236x at stock clock on the original 1.2 GHz 3B, and the
  densest track in the corpus plays through with zero starves.
- **mk2 on a Pi 4 renders at 1.065x on the stock core** — realtime with
  nothing spare. Patched, 2.676x.

So for the mkII the patches are the difference between working and
not-really-working on both boards; the mk1-generation romsets hold unpatched
only on a Pi 4 and gain the same ~2x everywhere.

Every digest above is **identical between the patched and unpatched builds**,
and identical between the Pi 3, the Pi 4 and x86-64: `c090f4a7b860f585` for mk1,
`6154f44b25c3b441` for mk2. The patches are bit-exact, and the emulation agrees
across architectures on real hardware rather than only under qemu.

Where the time goes with real firmware, unpatched (profiled on the pre-decoder2 core; decoder2 shrinks the CPU share, the shape stands): `PCM_Update` 31.4%,
`TIMER_Clock` 26.0%, `SM_Update` 11.9%, `unscramble` 4.2% (startup),
`PCM_ReadROM` 3.7%, `calc_tv` 3.5%.


The core is an interpreter running a cycle-level model of two CPUs and a PCM
chip. It is one serial dependency chain, so it lives or dies on single-core
throughput and memory latency. In rough order of how much they buy you:

## Build flags

All measured, in retired instructions, on the 32-slot workload. The defaults
are the defaults because of this table, not by assumption.

| Configuration | vs default | Verdict |
|---|---|---|
| **gcc `-O3` + LTO** (default) | — | keep |
| LTO off | **+8.9%** | LTO earns its default |
| `-O2` + LTO | **+11.5%** | `-O3` earns its default |
| `-O2`, no LTO | +43.3% | — |
| clang `-O3` + LTO | **+21.6%** | stay on gcc |
| **+ PGO** | **−1.3%** | not worth enabling by default — see below |
| `-fno-plt`, `-fno-semantic-interposition`, `-fvisibility=hidden` | **0.0%** | no effect, skip them |
| `-funroll-loops` | −0.8% on x86 | **do not use on ARM** — see below |

`-DSC55D_CPU=cortex-a53` barely changes *which* instructions are generated
(228 vs 225 in `SM_Update` against generic aarch64). Its real value is
instruction *scheduling* for an in-order pipeline, which no tool available here
can score. Harmless, keep it, but do not expect a number.

**PGO is a disappointment, and earlier advice here was wrong.** This document
previously suggested PGO might be the biggest single win, on the reasoning that
upstream PR #51's main safe technique was hand-reordering branches by hit
frequency and PGO automates exactly that. Measured, the full cycle yields
**−1.3%**, nowhere near PR #51's reported ~35%. Worse, a binary trained on one
romset ran **0.14% slower** on another — the gain is profile-specific and
inverts under a workload change.

The training run here used zero-filled ROMs, where both CPUs execute mostly
invalid opcodes, so precisely the opcode-dispatch frequencies PR #51 targeted
are the ones the profile gets wrong. That neither confirms nor refutes the 35%
claim; it does establish that PGO is not a free substitute for it, and that
training on a fake workload is worse than not training at all. **If you want
PGO, train it on the Pi, on real ROMs, playing music you care about.**

`-funroll-loops` is the clearest case of a result that does not travel: −0.8%
on x86-64, but on aarch64 it adds 64 instructions to `SM_Update` and 4.5 KiB to
`.text`. An A53 has a 32 KiB L1I and no out-of-order engine to hide the extra
code.

The cheap flags measure as *exactly* zero — single-digit instructions out of
7.86 billion, not "too small to see". LTO has already merged the hot code into
one unit, and none of it crosses a shared-object boundary, so there is nothing
for them to act on.

## Pick a cheaper machine

Not every SC-55 costs the same to emulate, and the differences are large:

- **`--model scb55` (or `rlp3237`) has no sub-MCU — measured at −15.5% of all
  retired instructions, the single largest lever here.** On an mk2 the core emulates
  a *second* CPU, stepping it after every instruction of the first. The SCB-55
  is the same sound engine on a card with no front panel, so that whole second
  interpreter collapses into cheap UART polling. On a Pi 3 this is likely the
  difference between working and not.
- **`--model mk1` and `jv880` run at 64000 Hz** rather than 66207 Hz — about
  3.3% fewer samples to produce, and the mk1 also has no sub-MCU.
- sc55d installs **no LCD backend**, so the core skips LCD emulation entirely.
  Nothing to configure; it is simply work the desktop frontend does and we do not.

## System tuning

```bash
# The kernel RT throttle. This one is not optional.
sudo sysctl -w kernel.sched_rt_runtime_us=-1
echo 'kernel.sched_rt_runtime_us=-1' | sudo tee /etc/sysctl.d/99-sc55d.conf

# CPU governor: ondemand ramps, and the ramp is an xrun
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# /boot/firmware/cmdline.txt — hand core 3 to sc55d alone, keep IRQs off it
isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2
# then run with --cpu 3 --output-cpu 2
```

- **Turn off the kernel RT throttle.** By default the kernel reserves 50 ms of
  every second for non-realtime work and simply stops any `SCHED_FIFO` thread
  that is still runnable when the budget is gone. sc55d's render thread
  saturates its core precisely when the board is struggling, so the throttle
  fires exactly when you can least afford it — and 50 ms is far longer than any
  sensible audio buffer, so every occurrence is an xrun. This is not
  speculation: a CPU-saturating `SCHED_FIFO` loop containing no emulator at
  all, measured here, stalls up to 54 ms roughly once a second; the same loop
  at normal priority never exceeds 1.6 ms. sc55d warns at startup if the
  throttle is on.

- **Give the renderer a core and the output thread a different one.** That is
  what `--render-ahead` exists to exploit; putting both on the isolated core
  gets you the serial behaviour back with extra latency. The output thread is
  cheap enough to share core 2 with the rest of the system.

- **Pick the rate converter.** 66207 Hz is not a rate any card supports, so the
  plug layer converts every sample — and it does that work *inside
  `snd_pcm_writei()`*, on the output thread, which is the other reason that
  thread wants its own core. Measured here, one 256-frame period against a
  3867 µs budget:

  | `defaults.pcm.rate_converter` | per period | of one core |
  |---|---|---|
  | `linear`          |   2.4 µs |  0.06% |
  | `lavrate`         |   5.9 µs |  0.15% |
  | `speexrate`       |  37.4 µs |  0.97% |
  | (unset)           |  38.0 µs |  0.98% |
  | `samplerate`      |  60.2 µs |  1.56% |
  | `speexrate_best`  | 255.9 µs |  6.62% |
  | `samplerate_best` | 469.9 µs | 12.15% |

  ```
  # /etc/asound.conf
  defaults.pcm.rate_converter "lavrate"
  ```

  `lavrate` is the one to use: 6x cheaper than the default and far better than
  `linear`, which is cheapest but audibly so — plain linear interpolation of a
  66 kHz source aliases. Avoid both `_best` variants; on a Pi 3 they would cost
  more than the whole rest of the output path. These are x86 numbers, so read
  the *ratios*, not the absolute microseconds.

- **The 3.5 mm jack works, with one caveat.** An I²S DAC HAT or USB DAC still
  sounds better — the jack is PWM-driven out of a divided clock — but it does
  **not** cost the renderer anything and it does **not** resample: bcm2835
  accepts 8000–192000 Hz, and sc55d was measured running on it at the mk1's
  native 64000 (`rate: 64000 (64000/1)`). A Pi 3 with no DAC at all holds mk1 at
  1.43x realtime with zero dropouts across a 20-minute soak.

  What it will not do is short periods. ALSA advertises `PERIOD_SIZE` from 80
  frames and then underruns continuously well above that, because bcm2835 audio
  goes to the VideoCore over VCHIQ — one round trip per period, where an I²S DAC
  uses DMA. Measured on a Pi 3 at 64000, 25 s of real music each:

  | | | | |
  |---|---|---|---|
  | `128 × 3` | 1487 xruns | `480 × 4` | 0 xruns |
  | `256 × 3` | 1302 xruns | `512 × 4` | 0 xruns |
  | `480 × 3` | 0 xruns | `1024 × 4` | 0 xruns |

  So the floor is between 4 ms and 7.5 ms of period time. Give the jack
  `--period-frames 512 --periods 4` or more and it is solid; the 128-frame
  settings that suit a DAC will fail on it continuously.
- **Trade latency for safety on a Pi 3.** `--period-frames 512 --periods 4` is
  about 31 ms and far harder to starve than the 11.6 ms default — and, per the
  table above, it is also the setting the onboard jack needs.
- **Watch thermals.** A Pi 3B+ throttles without a heatsink; check
  `vcgencmd get_throttled`.
- Bluetooth is already disabled if you followed the serial MIDI setup; also turn
  off Wi-Fi power saving if the box is headless and wired.

## Core patches

`patches/` holds four performance patches applied at build time to a copy of
the core; the submodule itself is never modified. They are **on by default** and
**pass all 37 of upstream's own SC-55mk2 integration cases** — real MIDI files
with published expected SHA-256 hashes of the rendered audio, an absolute
reference rather than a comparison against ourselves. Each patch also has a
ROM-free equivalence test with deliberate mutants. `-DSC55D_PATCH_CORE=OFF`
disables them.

Upstream has since assessed the series, and their `development/decoder2` branch
supersedes one patch of it and is where further work should be based. What that
overturns, what it does not, and the measurements behind the disagreement are in
[patches/README.md § Upstream's assessment](../patches/README.md#upstreams-assessment-and-what-decoder2-changes).

The **mk1** family is validated too, on hardware: patched and unpatched builds
render the benchmark sequence to the identical digest `c090f4a7b860f585` on a
Pi 3. Remaining romsets — the JV-880 especially — are **not** validated, because
no ROMs for them were available. If you run one, run
[`scripts/validate-patches.sh`](../scripts/validate-patches.sh) first. [patches/README.md](../patches/README.md) has the details, plus
what was measured and rejected.

`--bench` prints an FNV-1a digest of the audio it renders precisely so this
check is easy. A digest marked `(SILENT)` means the run produced no audio and
the comparison is meaningless.

[`arm-optimization.md`](arm-optimization.md) covers the questions that come up next: whether NEON,
the Pi's GPU, or an off-the-shelf ARM-optimised library can be pointed at any of
this. Summary: not at the emulator, which is bit-exact hardware modelling rather
than DSP — but yes at the resampler, which is a one-line `/etc/asound.conf`
change to a NEON-backed converter.

## Where the time actually goes

Callgrind, on a synthetic 32-slot PCM workload (no real ROMs — see the caveat
below):

| | share of retired instructions |
|---|---|
| `TIMER_Clock` | 29% |
| `PCM_Update` | 28% |
| `SM_Update` (sub-MCU) | 12% |
| `MCU_Interrupt_Handle` | 4% |
| `PCM_ReadROM` | 3% |

Two things worth knowing from that. `TIMER_Clock` being the largest is a
surprise, and it is what `patches/0001` targets. And `SM_Update` is 12% that
simply disappears if you can use the `scb55` romset, which has no sub-MCU.

Caveat: this profile used placeholder ROMs, so the MCU interpreter itself is
under-represented — real firmware executes varied instructions instead of
trapping. `TIMER_Clock` and `PCM_Update` are driven by cycle count rather than
ROM content, so their absolute cost per second of audio is right; the MCU's
share on top of them is not. Re-profile on the target with real ROMs before
optimising anything else.

## What it costs, in instructions

Two callgrind runs differing only in length, so every fixed cost — ROM loading,
unscrambling, SHA-256 verification, firmware boot — cancels in the subtraction:

**1.673 × 10⁹ retired instructions per second of audio**, with the voice pool
saturated. Against the 3.95x measured on a 2.80 GHz core that implies an IPC of
2.36 here. Use it to estimate another machine before buying one; it is a far
better basis than a wall-clock ratio, which carries this host's clock and
microarchitecture inside it.

Note this is the *dense* figure. The benchmark's warm-up is much cheaper —
firmware booting, no voices sounding — so an instruction count taken over a
whole run understates the steady-state cost. The differential is what a
realtime verdict should be built on.

**Cache is not the constraint**, which is worth knowing because it forecloses a
line of speculation. Cachegrind with a Pi 3's geometry (32 KB 4-way L1D, 512 KB
16-way L2, no L3):

```
I refs   8,478,743,209    I1 miss 0.04%    LLi miss 0.00%
D refs   2,688,036,610    D1 miss 0.1%     LLd miss 0.0%
LL misses    1,218,661
```

Three megabytes of wave ROM against half a megabyte of L2 looks like it should
thrash, and it does not: voices read contiguous samples, so the pattern is
localised. Even attributing every last-level miss to the dense section puts the
DRAM cost at roughly 6% of a Pi 3 core. The emulator is instruction-bound, not
memory-bound, and a board should be judged on how many instructions per second
it retires.

## Known ceiling

The thing render-ahead cannot do is make the core faster. It converts spare
cores into *tolerance of jitter*, nothing else: the render thread runs
continuously on one core while the output thread blocks in ALSA on another, and
the queued periods absorb a hiccup that would otherwise have been an xrun.

So there are two different failure modes, and sc55d now tells them apart. If
the shutdown line reports **starves**, the ring ran dry: the core is not
sustaining realtime on this board and no amount of buffering will fix it —
that is a `--bench` problem, and the answers are the patches, the build flags,
a cheaper romset, or a faster board. If there are **xruns but no starves**, the
audio path was late while the renderer was keeping up, and more buffer
(`--periods`, `--render-ahead`) or better isolation is the answer.

**A ring recovers more slowly the closer the board is to its limit**, which is
the part that does not show up on a fast machine. The ring refills at
`1/r − 1` periods per period, where `r` is how much of its budget a period
costs. On this x86 host at `r = 0.25` it refills at 0.75 periods per period; at
`r = 0.90` it refills at 0.10. The *same* stall therefore takes about seven
times longer to pay back on a board near 1.0x, and a second stall arriving
inside that window stacks on the first. So a deeper ring is worth less on a
struggling board than the arithmetic suggests, and headroom in the core is
worth more.

That is also why the RT throttle cannot be buffered away. A 50 ms hole at 256
frames needs 13 periods of ring to survive; the 262 ms stall recorded here
needs 68. No sane depth covers that — the `sysctl` does.

Beyond that: a single instance is strictly serial. The MCU, sub-MCU and PCM
chip are cycle-coupled per instruction, so the emulation itself cannot be split
across cores, and running several instances does not help a board that is short
— each one has to hit realtime independently, so N copies at 0.6x are still
0.6x. What is left is making the core cheaper, and [patches/README.md](../patches/README.md) records
what has been tried.

