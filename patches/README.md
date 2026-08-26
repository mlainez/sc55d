# Core patches

> **Licence:** these patches contain excerpts of the emulation core's source as
> diff context, so they carry the core's licence, not sc55d's — and so do the
> equivalence tests under `tests/`, which model core logic closely enough to be
> treated as derived from it. See [`../NOTICE`](../NOTICE) for the full text and
> [`../LICENSING.md`](../LICENSING.md) for what applies to what. In short:
> redistributable, but not saleable and not for commercial use, and the notice
> must travel with them.

Four performance patches for the Nuked-SC55 core, applied at build time to a
copy of the core under the build tree — **the `vendor/nuked-sc55` submodule is
never modified**. They exist for one purpose, and were kept to the minimum that
serves it: **running an SC-55mkII in realtime on a Raspberry Pi 3 at its stock
clock, with headroom.** Everything here is bit-exact — the audio is identical
to the unpatched core, byte for byte — and each patch has a differential proof
with deliberate mutants. Twelve further patches that were also bit-exact but
did not move the target number are in [`attic/`](attic/README.md), with the
measurements that retired them.

## For upstream: stock decoder2 versus this series

All figures below are the core's `development/decoder2` branch at `36fb091`,
unpatched, against the same sources with these four patches. Worst-second
realtime ratio from `sc55d --bench` (30 s of a dense sixteen-part sequence,
the lowest one-second ratio of the run; 1.0x is the line), medians of three
interleaved runs, digest `6154f44b25c3b441` (mk2) / `c090f4a7b860f585` (mk1)
identical on every run:

| | stock decoder2 | + this series | |
|---|---|---|---|
| **Raspberry Pi 3B, 1200 MHz, mk2** | 0.500x | **1.236x** | **2.47x** faster |
| Raspberry Pi 3B, 1200 MHz, mk1 | 0.821x | 1.603x | 1.95x |
| Raspberry Pi 4, 1500 MHz, mk2 | 1.065x | **2.676x** | 2.51x |
| Raspberry Pi 4, 1500 MHz, mk1 | 1.750x | 3.415x | 1.95x |
| x86-64 Xeon 2.8 GHz, mk2 | — | 13.6x | — |
| x86-64, retired instructions per audio-second, mk2 | 3.198 G | 1.396 G | −56.4% |

The practical statement: an SC-55mkII on a Pi 3 goes from half speed to
realtime with a quarter of headroom; `17.mid`, the densest track in the test
corpus, plays through the appliance's production configuration (640-frame
periods, 7 periods of render-ahead, SCHED_FIFO) with **0 starves** — the ring
never below 3 of 7 periods — on both boards.

### Which patches matter on ARM but not on x86

This is the point the series is built around, so it is stated plainly. An
in-order Cortex-A53 does not hide instruction-count-neutral costs the way an
out-of-order x86 does, and two of the four patches are worth nothing — or
next to nothing — in retired instructions while being decisive on the board:

| patch | x86 retired instructions | Pi 3 worst-second | what it is really about |
|---|---|---|---|
| 0001 timer closed form | −80.9% of the series' total | +57% | algorithmic: everywhere |
| 0002 **ARM hot-field layout** | **0.00%** | **+4.7%** | AArch64 addressing-mode reach; x86 has 32-bit displacements and never had the problem |
| 0003 PCM per-tick work | −11.2% | +10.6% | algorithmic, with one branch-predictor-shaped part (the reverb gating: +0.4% instructions, +0.5% on the board) |
| 0004 sub-MCU idle fast-forward | −9.0% | **+26%** on top of 0001, then +8% more once the timer advance is O(1) | skipped work was branchy, dependent-load work an instruction count undervalues; the closed-form timer advance is exactly zero on x86 wall-clock and worth 8% on the A53 |

(Board percentages are the step each patch adds in the ladder below, on the
Pi 3, mk2.)

## The ladder: what each patch adds, on the target

Pi 3B, stock 1200 MHz, mk2, worst-second, medians of three interleaved runs,
every run at the reference digest:

```
stock decoder2 (36fb091), unpatched            0.500x
+ 0001  mcu_timer closed-form advancement      0.784x   (+57%)
+ 0004  sub-MCU idle-loop fast-forward         0.986x   (+26%)
+ 0003  PCM per-tick work                      1.091x   (+11%)
+ 0002  ARM hot-field layout                   1.142x   (+4.7%)
        ... with 0004's O(1) timer advance     1.236x   (+8.2%)   <- the series as shipped
        (the former sixteen-patch series       1.170x)
```

On the Pi 4 the same comparison reads 2.434x for the former sixteen and
2.676x for these four. One honest footnote: on the mk1, which has no sub-MCU
and runs none of 0004, the four-patch series reads about 1% below the
sixteen-patch one on both boards (Pi 3 1.603x vs 1.608x; Pi 4 3.415x vs
3.453x) — most plausibly code-layout movement on the in-order core, inside
the mk1's very large margin, and recorded rather than explained away.

The last two lines are the reason the series is four patches and not
sixteen: with the sub-MCU's idle loop skipped, the constant-time timer
advance that the skip hands thousands of ticks to is worth more than the
twelve patches that were dropped, combined.

## The patches

| # | patch | what it does | proof |
|---|---|---|---|
| 0001 | `mcu_timer-closed-form-advancement` | The main MCU's timer block was stepped cycle by cycle; the counters are deferred and the next event scheduled in closed form. `TIMER_Clock` 2295 M → 38 M instructions on the 32-slot workload. | `tests/timer_closed_form/` — the real `mcu_timer.cpp` built twice in one program, upstream in namespace `ref` and patched in `neu`, compared after each of 9.6 M operations (observational equivalence: the patch deliberately leaves counters stale and syncs on demand); 9 mutants. |
| 0002 | `arm-hot-field-layout` | Reorders `mcu_t` and `pcm_t` so everything touched per instruction and per tick sits inside the AArch64 immediate's reach ahead of 664 KiB / 15 MiB of arrays. Layout only; neither struct is serialised, memcpy'd or aggregate-initialised. | Layout has no behaviour to prove; the digest and the 37 integration cases are the check. Measured: −5 instructions per emulated instruction on AArch64, 0 on x86. |
| 0003 | `pcm-per-tick-work` | Five hoists in the PCM inner loop: per-tick invariants of `calc_tv` computed once instead of 97 times, address-step algebra folded, `calc_tv` templated on the envelope index, reverb injection gated (6 of 32 slots), the dead fourth address step and its wave-ROM read skipped. −21% of PCM per tick at any activity level. | `tests/pcm/` — `pcm.cpp` built twice into a driver that digests the entire mutable PCM state each tick over 44 cases in 10 modes, two of them wide-pitch modes added so the fourth-step branch is exercised; the runner refuses to run if the patch fails to apply or the "patched" source is identical to the pristine one. |
| 0004 | `submcu-fast-forward-idle-loop` | The sub-MCU's idle loop — 98.2% of its instructions, touching only zero-page RAM — is detected as stationary and its clock advanced across whole passes to the next timer, UART or interrupt event, rewound exactly on any external access; the timer is advanced in constant time (closed form over prescaler, counter and reloads). | `tests/submcu/` — the real `submcu.cpp` built twice, upstream renamed, both driven by the same simulated main MCU over the real firmware and three synthetic programs; 5 mutants. The timer advance separately against upstream's tick loop over 27 M states; 3 mutants. |

Proofs are run with:

```bash
./patches/tests/timer_closed_form/run.sh
./patches/tests/pcm/run.sh
./patches/tests/submcu/run.sh <build>/core-patched <pristine core checkout> <build>
#   SM_ROM=<path to mk2 sub-MCU ROM> adds the real firmware to the sub-MCU run
```

## Validation

**Upstream's own integration cases.** The core's `test/integration/` carries
real MIDI files with the **expected SHA-256 of the rendered audio** — an
absolute reference published by upstream, not a comparison against ourselves.
All **37** mk2-family cases match with the four patches applied (36 on the
core's master; the decoder2 branch adds one). Reproducing it needs the core's
`nuked-sc55-render` target and your own ROMs; note their renderer needs
RtMidi present to configure, even though it does not use it.

**Digest.** `--bench` prints an FNV-1a digest of everything it renders. With a
real `mk2-v1.01` set the patched and unpatched cores render **bit-identical
audio** (`6154f44b25c3b441` over the 30-second bench, `c090f4a7b860f585` for
mk1), and the aarch64 builds produce the same digests as x86-64 — every one
of the board runs behind the tables above printed it. A digest marked
`(SILENT)` means the run produced no audio and the comparison is worthless.
`scripts/validate-patches.sh` runs the patched-versus-unpatched comparison
against your own ROMs and romsets.

**Not validated: `st`, `cm300`, `jv880`, `scb55`, `rlp3237`, `sc155`.** No
ROMs for them were available. If you run one of these, run
`scripts/validate-patches.sh --roms <dir>` first, or build with
`-DSC55D_PATCH_CORE=OFF`.

## How the four were chosen

Every patch that has ever been in this series was measured alone — leave-one-out
builds under callgrind on x86-64 with real ROMs, and worst-second A/B on the
Pi 3 and Pi 4 for anything whose value was claimed to be architecture-specific
— with a fully unpatched control and a digest check on every build. The
per-patch numbers, the additivity cross-checks (the x86 contributions sum to
102.3% of the directly measured whole; the board deltas to within 0.01 point),
and the reasons each dropped patch was retired are in
[`attic/README.md`](attic/README.md). The bar was simple: does removing it
move the mkII on a Pi 3? Twelve did not, or did so by less than the noise of
the measurement, and a patch that is provably correct and worth 0.2% is still
noise to a maintainer.

## What was tried and rejected

Measured and thrown away, so nobody repeats them:

- **Branch-free wave ROM banking.** `PCM_ReadROM()` re-derives its bank layout
  on every call. Hoisting it into a precomputed `{base, mask}[8]` table made
  things *slightly slower*: the branches are perfectly predictable, and the
  table replaced them with a dependent load.
- **Making the core's UART FIFO thread-safe** with an atomic write pointer.
  Bit-identical, and rejected because GCC's AArch64 backend will not fold an
  atomic load into an offset addressing mode: three instructions instead of
  one `ldp` on a poll that runs once per sub-MCU instruction, about 0.5% of the
  whole program, forever. The fix belongs on the frontend's side of the
  boundary (`src/midi_queue.h`), where it is.
- **Batching the sub-MCU or PCM steps** across main-MCU instructions. Not
  equivalence-preserving: `PCM_Write`/`PCM_Read` do not self-sync, and the
  sub-MCU's interleaving with the main MCU is observable through shared RAM.
  The fast-forward in 0004 is the exact version of this idea — it skips only
  what is provably unobservable.
- **`-O3`**: a measured regression on the A53 (−4.6% worst-second), consistent
  with the decoder's 1400 indirect-branch targets and I-cache pressure.
  **`-Os`**: −35% in dynamic instructions. **PGO**: worth about 5% on top of
  the series and plumbed (`SC55D_PGO=generate|use`), but no longer required.

`candidates/` holds one further patch, written and proven but parked: skipping
idle voice slots is −51% of PCM per tick on typical material and +2% at full
polyphony, and realtime is decided by the worst case.
