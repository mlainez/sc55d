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

They are **off by default**, and should stay off until validated on your ROMs.

## Why off by default

Each patch is argued to be behaviour-preserving, but an argument is not a test.
The core carries a cautionary example: commit `8d6f67a`, *"mcu_timer: fix
invalid optimization"*, undoes an earlier lookup-table change in the very same
function that silently altered JV-880 output and went unnoticed until someone
tried to write integration tests for it. A timer that is subtly wrong does not
crash; it shifts interrupt timing, and you find out much later.

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

| Patch | Effect | Status |
|---|---|---|
| `0001-mcu_timer-closed-form-advancement.patch` | `TIMER_Clock()` walks one cycle at a time applying ticks that change nothing — 55M ticks produced 1561 events in a measured run. Defers the counters and schedules the next event in closed form. | `TIMER_Clock` 2295M → 38M retired instructions; whole program 7865M → 5608M. Worst case on a microbenchmark with timers actually programmed: 691 → 184 Ir/call. **Differential test + 9/9 mutants; real-ROM audio unchecked.** |
| `0002-rom_io-table-driven-unscramble.patch` | `unscramble()` rebuilds two fixed bit permutations a bit at a time per byte — ~28 tests over 2–3 MB of wave ROM. Precomputes 8.25 KiB of tables. | 5.1x faster startup. **Equivalence proved exhaustively.** |

See also [`candidates/`](candidates/) for patches that are correct but whose
value cannot be established without real ROMs, and which no build applies.

### Proofs

Both have ROM-free tests, because both transformations are decidable without
audio:

```bash
g++ -O2 -std=c++23 -o /tmp/ut patches/tests/unscramble_equivalence.cpp && /tmp/ut
./patches/tests/timer_closed_form/run.sh
```

- `unscramble_equivalence` checks all 256 data bytes and all 8388608 addresses
  against the original loops.
- `timer_closed_form/` builds the core's real `mcu_timer.cpp` **twice in one
  program** — upstream in namespace `ref`, patched in `neu` — and drives both
  through the same operations, comparing `timer.cycles`, the interrupt bitset
  and every byte read back through the public accessors. 9.6M operations over
  480 randomised runs, zero mismatches. Field-by-field comparison would be
  wrong: the patch deliberately leaves counters stale and syncs on demand, so
  what is claimed is *observational* equivalence.

Both suites are known to be capable of failing. `timer_mutants.sh` breaks the
patched timer nine ways — overshooting the bound, ignoring clear-on-match,
dropping the overflow event, failing to sync before a register read — and all
nine are caught.

There is one further piece of evidence for 0001 worth knowing about. On the
placeholder-ROM profile, **every other function in the emulator retires a
bit-identical instruction count**: `PCM_Update` 2,216,305,770 in both,
`SM_Update` 905,602,761, `MCU_Interrupt_Handle` 320,207,643, `calc_tv`,
`PCM_ReadROM`, `MCU_Read`, `SM_Read` all unchanged to the instruction. The
emulated MCU executed exactly the same instruction stream. That is not proof
across all ROMs, but it is a great deal stronger than a digest of silence.

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
