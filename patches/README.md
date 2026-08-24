# Core patches

> **Licence:** these patches contain excerpts of the emulation core's source as
> diff context, so they carry the core's licence, not sc55d's. See
> [`../NOTICE`](../NOTICE). In short: redistributable, but not saleable and not
> for commercial use, and the notice must travel with them.

Performance patches for the emulation core. They are applied at build time to a
copy of the core under the build tree — **the `vendor/nuked-sc55` submodule is
never modified**, so it stays a pristine checkout and everything we change is
reviewable as a patch here.

```bash
cmake -S . -B build -DSC55D_PATCH_CORE=ON
```

They are **on by default**, having been validated against real audio — see
below for exactly which romsets that covers.

## Validation status

**The strongest check available is upstream's own.** `test/integration/` in the
core's own tree carries 36 real MIDI files and, for each, the **expected
SHA-256 of the rendered audio** — an absolute reference, not a comparison
against ourselves. 35 of those cases are for `mk2-v1.01`, which is exactly the
romset here. Running them needs the core's `nuked-sc55-render` target and
`NUKED_TEST_ROMDIR` pointing at your ROMs; `scripts/validate-patches.sh` does
not do this, and should be treated as the quick check rather than the real one.

```bash
cmake -S vendor/nuked-sc55 -B build-render -DCMAKE_BUILD_TYPE=Release
cmake --build build-render --target nuked-sc55-render
# then drive test/integration/CMakeLists.txt's (romset, file, sha256) triples
# through test/integration/test_runner.py
```

Note their renderer needs RtMidi present to configure, even though it does not
use it.


**Validated: the SC-55mk2 family.** With a real `mk2-v1.01` ROM set, the
patched and unpatched cores render **bit-identical audio over 30 seconds** of
the benchmark's sixteen-part stress sequence — digest `6154f44b25c3b441` for
`mk2` and `8e12918eec7012a4` for `sc155mk2`. The aarch64 build, cross-compiled
for `-mcpu=cortex-a53` and run under qemu, produces a digest identical to
x86-64 on the same workload.

**Not validated: `st`, `mk1`, `cm300`, `jv880`, `scb55`, `rlp3237`, `sc155`.**
No ROMs for them were available. Treat the JV-880 as the one to actually worry
about: the core carries commit `8d6f67a`, *"mcu_timer: fix invalid
optimization"*, which undoes an earlier lookup-table change in the very same
timer function that silently altered **JV-880 output specifically** and went
unnoticed until someone tried to write integration tests for it. A timer that
is subtly wrong does not crash; it shifts interrupt timing, and you find out
much later. If you run one of these romsets, run the validation below first, or
build with `-DSC55D_PATCH_CORE=OFF`.

## Validating a patch

`--bench` prints an FNV-1a digest of everything it renders. Two builds that
produce the same audio print the same digest:

```bash
cmake -S . -B build-off -DCMAKE_BUILD_TYPE=Release -DSC55D_PATCH_CORE=OFF
cmake -S . -B build-on  -DCMAKE_BUILD_TYPE=Release -DSC55D_PATCH_CORE=ON
cmake --build build-off -j4 && cmake --build build-on -j4

for m in mk2 st mk1 cm300 jv880 scb55; do
  echo "== $m"
  ./build-off/sc55d --roms /path/to/roms --model $m --bench --bench-seconds 30 | grep digest
  ./build-on/sc55d  --roms /path/to/roms --model $m --bench --bench-seconds 30 | grep digest
done
```

The digests must match for every romset. If the digest is marked `(SILENT)` the
run produced no audio and the comparison is worthless — that happens with
placeholder ROM files, and it is why these patches are still unvalidated.

The core's own integration tests are a stronger check again, and need real ROMs
too.

## Patches

Applied in order. Together they take the 32-slot benchmark from **7,864M to
4,320M retired instructions, −45.1%** — and that *understates* them, because
with placeholder ROMs no voice is ever keyed on, so the PCM patches barely
register.

| # | Patch | Effect |
|---|---|---|
| 0001 | `mcu_timer-closed-form-advancement` | Defers the counters, schedules the next event in closed form. `TIMER_Clock` **2295M → 38M**. |
| 0002 | `rom_io-table-driven-unscramble` | Two fixed bit permutations into 8.25 KiB of tables. **5.1x faster startup**. |
| 0003 | `mcu-code-fetch-fast-path` | ROM cases answered in the header. −0.36%, **better on real ROMs**. |
| 0004 | `mcu_step-hoist-fixed-work` | `has_submcu` decided once; ADCSR tested at the call site. −0.96%. |
| 0005 | `mcu_interrupt-skip-fully-masked-scan` | Early return when the mask is 7. −2.64% here, **but that is an artefact**. |
| 0006 | `mcu-hot-field-layout` | Per-instruction fields into the first 336 bytes of `mcu_t`. **−5 instructions per emulated instruction on AArch64**; ~0 on x86. |
| 0007 | `pcm-hot-field-layout` | `enable_oversampling` sat at offset 15,763,532, past 15 MB of wave ROM, alone in a cache line, read once per sample. |
| 0008 | `pcm-hoist-calc-tv-per-tick-invariants` | Per-tick tap table + 256-byte type table instead of 97 recomputations per tick. |
| 0009 | `pcm-fold-address-step-boolean-algebra` | The address step is `0/−1/+1` with a per-slot sign; `reg_slots` hoisted. |
| 0010 | `pcm-template-calc-tv-on-the-envelope-index` | `e` is a literal at all four sites; `e==2` drops its dead half. |
| 0011 | `pcm-gate-reverb-injection-switches` | Only 6 of 32 slots inject. Two indirect jumps per slot become one predictable test — **worth more than its instruction count on an in-order A53**. |
| 0012 | `pcm-skip-dead-fourth-address-step` | Below the `sub_phase_of` threshold the tail *and one of five wave ROM reads* are dead. |
| 0013 | `submcu-collapse-the-sub-MCU-timer-loop` | `SM_UpdateTimer` runs exactly three iterations per call, mostly decrementing a prescaler. **−2.70%** on real firmware, which programs a reload of 124 against a break-even of 3. |

0008–0012 together are **−21.0% of PCM per tick** with all 32 voices on, and
between −19.7% and −22.7% across every activity level — they do not depend on
how busy the synth is, which is why they are trusted despite the benchmark
being idle.

**Two numbers not to budget for.** 0005's −2.64% is an artefact: the
placeholder firmware never lowers `sr` from 0x700, so the guard fires on every
instruction where real firmware would lower the mask early. The guard is one
instruction, so its worst case is a ~0.08% loss. And the whole-program figure
above is measured on an idle synth.

### Proofs

Every patch has a ROM-free test:

```bash
g++ -O2 -std=c++23 -o /tmp/ut patches/tests/unscramble_equivalence.cpp && /tmp/ut
./patches/tests/timer_closed_form/run.sh
./patches/tests/mcu/run.sh
./patches/tests/pcm/run.sh
```

- **unscramble** — all 256 data bytes, all 8388608 addresses.
- **timer** — the real `mcu_timer.cpp` built twice in one program, upstream in
  namespace `ref` and patched in `neu`, compared after each of 9.6M operations.
  Field comparison would be wrong: the patch deliberately leaves counters stale
  and syncs on demand, so the claim is *observational* equivalence.
- **mcu** — the fetch fast path against the **real `MCU_Read`** over
  188,743,680 `(cp, pc)` × romset × `rom2_mask` combinations, comparing the
  return value, the log of outward calls *and* every scalar in `mcu_t`, so
  short-circuiting an address that latches the button matrix would be caught.
  Plus `mcu_interrupt.cpp` compiled twice over 7,700,480 machine states.
- **pcm** — `pcm.cpp` built twice into a driver that digests the **entire**
  mutable PCM state each tick (ram1, ram2, all 16 KiB of eram, every scalar),
  every sample posted and every interrupt raised. 44 cases over 10 modes,
  including two wide-pitch modes added specifically because the stock ones only
  drive `sub_phase_of` to 0..1 and would have left 0012's branch untested.

All four suites are known to be capable of failing — 9 mutants for the timer,
7 deliberate mistakes for the MCU series, 4 for PCM, 7 for the sub-MCU
candidate, all detected. The PCM runner additionally refuses to run if a patch
fails to apply or if the "patched" source turns out identical to the pristine
one, because an early version of it silently compared pristine against
pristine and passed.

### The strongest evidence available without ROMs

Call counts over 6,388,732 emulated instructions are **identical** between
unpatched and fully patched: `PCM_ReadROM` 14,868,480, `Diag_Printf`
11,612,931, `SM_Read` 7,993,725, and `PCM_Update`, `TIMER_Clock`, `SM_Update`,
`MCU_Interrupt_Handle`, `MCU_Operand_Nop`, `calc_tv` all to the call. Only the
two intended differences appear. The emulated machine follows the same
trajectory. The PCM patches were additionally cross-compiled for
`-mcpu=cortex-a53` and run under qemu, producing output identical to x86-64
across all 10 modes.

### Two mistakes worth not repeating

Both were caught by measuring rather than reasoning, and both are the reason
this directory has tests in it.

**Wall-clock is the wrong metric for a Pi.** The first draft of 0001 computed
the skip distance per iteration. On x86-64 it was ~10% faster — but it retired
*more* instructions than the unpatched core (2527M vs 2295M), because
out-of-order execution hid the extra arithmetic while the removed iterations
were branchy. On an in-order Cortex-A53 that would very likely have been a
regression. Hoisting the computation out of the loop, which is valid because
every divider period is a power of two and the smallest divides all the others,
made it faster on both metrics. **Check retired instructions, not just seconds.**

**Skipping work is not the same as skipping time.** The same draft advanced
`timer.cycles` to the next divider edge even when that overshot the point the
original loop would have stopped at. The cycles skipped are only uneventful
under the divider settings in force at that moment; `timer.cycles` persists
across calls, so if the MCU then programmed a finer divider, cycles that were
run past would have become live ones. The fix is to clamp to the original exit
point.

## Rejected

Things measured and thrown away, so nobody repeats them:

- **Branch-free wave ROM banking.** `PCM_ReadROM()` runs ~10M times a second and
  re-derives its bank layout on every call — a branch, an 8-way switch and a load
  from the MCU struct to re-test `is_mk1`/`is_jv880`, all of which are fixed once
  ROMs are loaded. Hoisting it into a precomputed `{base, mask}[8]` table made
  things *slightly slower*: those branches are perfectly predictable, so they were
  already free, and the table replaced them with a dependent load. Callgrind
  confirmed `PCM_ReadROM` is only 3.4% of instructions. Not worth revisiting
  unless a profile on the target says otherwise.
