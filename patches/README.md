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
| `0001-mcu_timer-skip-cycles-with-no-divider-edge.patch` | `TIMER_Clock()` steps once per 2 MCU cycles — six iterations per emulated instruction, nearly all doing nothing. Jumps to the next divider edge instead. Callgrind put `TIMER_Clock` at 29% of retired instructions, ahead of `PCM_Update`. | +7–13% on a synthetic 32-slot load, x86-64. **Audio unvalidated.** |

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
