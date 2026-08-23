# Core patches

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
| `0001-mcu_timer-skip-uneventful-cycles.patch` | `TIMER_Clock()` steps one cycle at a time — six iterations per emulated instruction, most doing nothing. Steps to the next divider edge instead. | +11% end to end; `TIMER_Clock` 2295M → 2142M instructions. **Proved by differential test; real-ROM audio still unchecked.** |
| `0002-rom_io-table-driven-unscramble.patch` | `unscramble()` rebuilds two fixed bit permutations a bit at a time per byte — ~28 tests over 2–3 MB of wave ROM. Precomputes 8.25 KiB of compile-time tables. | 5.1x faster startup. **Equivalence proved exhaustively.** |

### Proofs

Both patches have ROM-free tests under `tests/`, because both transformations
are decidable without any audio:

```bash
g++ -O2 -std=c++23 -o /tmp/ut patches/tests/unscramble_equivalence.cpp && /tmp/ut
g++ -O2 -std=c++17 -o /tmp/tt patches/tests/timer_step_equivalence.cpp && /tmp/tt
```

- `unscramble_equivalence` checks all 256 data bytes and all 8388608 addresses.
- `timer_step_equivalence` checks all 1024 divider configurations over 9.3M loop
  cases, comparing both the set of cycles the timers are clocked on *and* the
  final `timer.cycles`. It is known to be capable of failing: it rejects an
  earlier draft of 0001 that overshot the loop bound.

These prove the transformations, not the emulation. Run the digest comparison
above with real ROMs before trusting either in production.

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
