# Core patches

> **Licence:** these patches contain excerpts of the emulation core's source as
> diff context, so they carry the core's licence, not sc55d's — and so do the
> equivalence tests under `tests/`, which model core logic closely enough to be
> treated as derived from it. See [`../NOTICE`](../NOTICE) for the full text and
> [`../LICENSING.md`](../LICENSING.md) for what applies to what. In short:
> redistributable, but not saleable and not for commercial use, and the notice
> must travel with them.

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

**The patched core passes all 36 of upstream's own mk2 integration cases.**

That is the strongest check available, and it has now been run. `test/integration/`
in the core's own tree carries 72 real MIDI files and, for each, the **expected
SHA-256 of the rendered audio** — an absolute reference published by upstream,
not a comparison against ourselves. 36 of them are for the SC-55mk2 family (35
`mk2-v1.01` plus one `mk2`), which is exactly the romset available here.

With a real `mk2-v1.01` ROM set, both with and without `patches/`:

```
                       36 mk2 cases from upstream's own test list

with patches/          PASS 36   FAIL 0
unpatched (control)    PASS 36   FAIL 0
```

Thirty-six independent SHA-256 matches against hashes we did not choose. This
is what makes the patch series trustworthy in a way that self-comparison never
could: a digest that agrees with itself proves only that two builds of ours
agree, whereas these agree with the reference.

The unpatched control is strictly speaking redundant — thirty-six matches
against an absolute reference cannot happen by accident, so the patched run
already rules out a broken harness — but it costs nothing but time and it
closes the question completely.

Reproducing it needs the core's `nuked-sc55-render` target and your own ROMs:

```bash
cmake -S vendor/nuked-sc55 -B build-render -DCMAKE_BUILD_TYPE=Release
cmake --build build-render --target nuked-sc55-render
# then drive test/integration/CMakeLists.txt's (romset, file, sha256) triples
# through it: --stdout <mid> --rom-directory <dir> --romset <set> --reset gm
# and compare the SHA-256 of stdout against the triple's third field.
```

Note their renderer needs RtMidi present to configure, even though it does not
use it. `scripts/validate-patches.sh` does *not* do any of this — treat it as
the quick check, and this as the real one. The other 36 cases (`mk1-v1.21` and
`jv880-v1.0.1`) remain unrun for want of those ROM sets.


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
| 0003 | `mcu-code-fetch-fast-path` | ROM cases answered in the header. −0.36%, **better on real ROMs**. **Superseded by upstream's decoder2** — see below. |
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

## Upstream's assessment, and what decoder2 changes

The fork's maintainer was sent this series and replied. Paraphrasing their
points: many of the smaller patches "should have no measurable effect" because
"compilers can do a lot of these transformations automatically"; the ones that
hoist conditions to the caller are "made redundant by
`CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`"; the instruction-fetch work is
"invalidated" by their `development/decoder2` branch; and any further
performance work should be based on that branch
(`-DNUKED_ENABLE_DECODER2=ON`, to become the default in 0.7.0), which caches
decoded instructions and avoids `MCU_Read` for the majority of memory loads —
possibly enough to run mk2 romsets on a Pi 3. Memory layout and cache
efficiency are on their list for after 0.7.0: *"I know the large structures are
not ideal."*

Most of that is right. One part is measurably not, and the difference is worth
being precise about.

### Right: the small MCU patches are noise

Their prediction matches what this file already records. 0003 is −0.36%, 0004
is −0.96%, and 0005's −2.64% is an artefact of placeholder firmware whose real
worst case is a ~0.08% *loss*. Three patches, about 1% between them, one of
which may be negative on real ROMs. Nothing in the series' headline depends on
them.

### Right: 0003 is superseded

Confirmed from the branch rather than assumed. decoder2 replaces the body of
`MCU_ReadInstruction` with a call to `decoder2::FetchDecodeExecuteNext(mcu)`,
so `MCU_ReadCodeAdvance` — the function 0003 fast-paths — is no longer on the
hot path at all. 0003 should be dropped when we move, not rebased.

### Right: decoder2 is where to work next, and moving is cheap

The branch is 63 commits and +10,628 lines, but almost all of it is new files
under `src/backend/decoder2/`. Outside that directory it touches
`mcu.cpp` (6 lines), `mcu.h` (17), `rom_loader.cpp` (12), plus build glue.
`pcm.cpp`, `mcu_timer.cpp`, `submcu.cpp`, `mcu_interrupt.cpp` and `rom_io.cpp`
are untouched.

So the series survives the move nearly intact. Applied in order against
`origin/development/decoder2` at `36fb091`, exactly as CMake applies them:

```
12 of 13 apply clean
0006-mcu-hot-field-layout.patch   hunk 4 of 4 rejected
```

0006's rejected hunk is the tail of `mcu_t`, where decoder2 appends an
`icache` member. That is a context conflict, not a design conflict — rebasing
it means deciding where `icache` belongs in the reordered struct, which is the
patch's whole subject anyway.

### Not right: IPO cannot make them redundant, because IPO was already on

`CMakeLists.txt` sets `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON`, and
compiles the core's translation units *into the sc55d executable target* rather
than linking a separate library, so the compiler sees core and frontend as one
unit before LTO even runs. Both sides of the measurement were built that way —
`build-patch-off.log` and `build-patch-on.log` each open with `sc55d:
link-time optimization enabled`.

The **+43.5% realtime ratio and −45.1% retired instructions are therefore a
measurement taken with `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` on both sides**,
which is the configuration the claim says should erase them. Turning LTO off
costs a further +8.9% on its own, so the two are additive rather than
substitutes.

The reason is what the patches carrying the weight actually do. None of these
is a transformation a compiler performs:

| | Why no compiler does it |
|---|---|
| 0001, `TIMER_Clock` 2295M → 38M | Replaces an iterative countdown with a closed-form next-event calculation. Algorithmic. |
| 0002, 5.1x startup | Materialises two fixed bit permutations as 8.25 KiB of tables. |
| 0006, 0007 | Reorders struct members. Layout is ABI — a compiler is *forbidden* from doing this. |
| 0008–0012, −21.0% of PCM per tick | Hoists invariants out of a path that recomputed them 97 times per tick, templates on a runtime value, deletes provably dead work. Stable between −19.7% and −22.7% across every activity level. |

### Layout: the part worth comparing notes on

Their post-0.7.0 plan and 0006/0007 are aimed at the same thing, so here is
what was measured, on decoder2's own `mcu_t` at `36fb091`:

```
sizeof(mcu_t)          664936 bytes
offsetof(rom1)              48
offsetof(uart_buffer)   656616
offsetof(operand_type)  664892
offsetof(icache)        664928   (8 bytes -- a handle, not an inline table)
```

`icache` is fine: 8 bytes, 36 from the operand fields. The cost is that the
**per-instruction operand fields sit at offset 664892**, behind 649 KiB of ROM
and RAM arrays — and on AArch64 that is not merely a cache question. A scaled
12-bit immediate reaches 16380 bytes for a 32-bit load, so a field that deep
cannot be addressed in one instruction:

```
             struct field at offset 664892      struct field at offset 48
aarch64      add x0, x0, 655360                 ldr w0, [x0, 48]
             ldr w0, [x0, 9532]
```

Two instructions instead of one, on every access, for every hot field past
16 KiB. That is the mechanism behind 0006's measured **−5 instructions per
emulated instruction on AArch64 and ~0 on x86-64** — x86-64 has 32-bit
displacements, so the deep field costs it nothing and the patch looks pointless
there. It is measurable in retired instructions without a cache profiler, and
it is the strongest argument for doing the layout work: on the boards this
project targets, the large structures cost instructions, not just misses.

### What this does not settle

Whether decoder2 delivers enough to run an mk2 romset on a Pi 3 is unmeasured
here, and deliberately so. Its benefit is caching *decoded real instructions*
and skipping `MCU_Read`; a benchmark with placeholder ROMs has both CPUs
executing mostly invalid opcodes, so it would measure the decode cache against
trap paths and report a number that means nothing. That comparison needs real
ROMs, and the answer for a Pi 3 needs a Pi 3.

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

- **Making the core's UART FIFO thread-safe.** `mcu_t::uart_buffer` with its
  `uart_write_ptr`/`uart_read_ptr` pair is a single-producer ring with no
  synchronisation at all: `MCU_PostUART()` stores the byte and then bumps the
  pointer, both plain stores; `SM_UpdateUART()` polls the pointer and then reads
  the byte, both plain loads. Posting to it from a MIDI thread — which is what
  upstream's RtMidi callback does, and what sc55d did until the render-ahead
  work — is a data race, and on a weakly ordered Cortex-A53 a real one: the core
  can observe the advanced write pointer before the byte it points at and take
  whatever was in that slot the previous time round the 8 KiB buffer. A
  corrupted status byte is a stuck note, and it would be rare, silent and
  miserable to debug on a Pi.

  The obvious patch makes `uart_write_ptr` a `std::atomic<uint32_t>`, publishes
  with `memory_order_release`, and polls with a *relaxed* load plus an acquire
  fence on the rare path where a byte is present — so the fence never runs in
  the hot loop. It was written and it works: bit-identical audio, digest
  `b1e9a497433a81fa` unchanged.

  It was still rejected, because GCC's AArch64 backend will not fold an atomic
  load into an offset addressing mode, even a relaxed one, and will not pair it
  with the adjacent plain load of `uart_read_ptr`:

  ```
  plain    ldp w1, w0, [x0, 160]                   # 1 instruction
  atomic   add x1, x0, 160 ; ldr w1, [x1] ; ldr w0, [x0, 164]   # 3
  ```

  `__atomic_load_n()` on a plain field generates the same three; `volatile` gets
  the offset back but gives no ordering, so it does not fix anything. The poll
  runs once per sub-MCU instruction — measured 14,822,370 times per two seconds
  of real mk2 audio, against a program total of 8,766,976,430 instructions — so
  three extra instructions is about **0.51% of the whole program**, paid forever,
  on the target architecture.

  The fix belongs on our side of the boundary instead. `src/midi_queue.h` is a
  properly synchronised SPSC byte queue; the render thread drains it and is the
  only thread that ever calls into the core, so the core's FIFO becomes what it
  was always written as — single-threaded — and the hot poll keeps its `ldp`.
  It costs one relaxed load per drain, four times per period.
