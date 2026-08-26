# The attic: patches that were kept honest, then dropped

Every patch here is bit-exact — each has a differential proof with deliberate
mutants (in `tests/`), each passed the digest gate and upstream's integration
cases — and every one was measured alone, on x86-64 in retired instructions
and, where its value was claimed to be architecture-specific, on a Raspberry
Pi 3B in worst-second realtime ratio. They were dropped from the series in
August 2026 for one reason: **the series exists to run an SC-55mkII on a
Raspberry Pi 3 with headroom, and these do not move that number.** A patch
that is provably correct and worth 0.2% is still noise to a maintainer.

Nothing here is lost. The files are unchanged; `git log --follow` on any of
them has the whole story; and the measurements below are the record of why.

## What was measured, and how the decision was made

The reference point is the four-patch series in `../` on a Pi 3B at its stock
1200 MHz, mk2 romset, decoder2 core (`36fb091`):

```
stock decoder2, unpatched         0.500x
+ 0001 timer closed form          0.784x
+ 0004 sub-MCU idle fast-forward  0.986x
+ 0003 PCM per-tick work          1.091x
+ 0002 ARM hot-field layout       1.142x     (the four-patch series)
  ... all sixteen former patches  1.170x
```

So the twelve patches now in this directory were, together, worth 2.5% on the
target — and 0013's share of that (the constant-time timer advance the
fast-forward needs) was folded *into* 0004 rather than dropped, which closes
most of the gap. Per patch, with 0016 present (leave-one-out, retired
instructions per audio-second on x86-64 with real mk2 ROMs; board deltas from
the earlier audit where they exist):

| former patch | what it did | x86 Ir | Pi 3 | verdict |
|---|---|---|---|---|
| 0002 rom_io unscramble tables | 5.1x faster ROM unscrambling at startup | **0.00% steady state** (0.62 G at boot) | — | startup only; not a realtime lever |
| 0003 mcu code-fetch fast path | answered ROM code fetches in the header | retired earlier | — | superseded by upstream's decoder2 |
| 0004 mcu_step hoist | `has_submcu` decided once; ADCSR at the call site | +1.39% (audit) | — | below the bar |
| 0005 interrupt mask guard | early return when the mask is 7 | +0.20% | — | below the bar |
| 0006 mcu hot-field layout | see `../0002` | 0.00% | **2.7%** | **kept**, merged into 0002 |
| 0007 pcm hot-field layout | see `../0002` | 0.00% | **0.46%** | **kept**, merged into 0002 |
| 0008–0012 PCM series | see `../0003` | 11.2% together | — | **kept**, squashed into 0003 |
| 0013 sub-MCU timer collapse | partial closed form for `SM_UpdateTimer` | +4.10% with the fast-forward | — | **replaced**: a true O(1) advance is part of 0004 |
| 0014 sub-MCU interrupt-scan gate | two-byte early-out in `SM_HandleInterrupt` | +0.35% with the fast-forward (was 5.49% before it) | — | the fast-forward removed the work it gated |
| 0015 word-access fast path | one decode-ladder walk per aligned word | +1.03% | — | below the bar |
| 0016 sub-MCU idle fast-forward | see `../0004` | −9.0% Ir, +19% wall | **+19.4%** | **kept**, as 0004, with 0013's job folded in |

Two lessons the table records. First, an instruction count is not time on an
in-order ARM core: 0006 is exactly zero on x86 and the second-largest
contribution on the Pi 3; 0016 is a 9% instruction saving and a 19% time
saving. Second, a patch's value depends on its neighbours: 0014 was the
second-largest patch in the series until 0016 skipped the loop it was
guarding.

## Rebuilding any of them

They apply to the decoder2 core in their original numbering order (0006 in
its `.decoder2` variant on that tree) and are excluded by `CMakeLists.txt`
only because they are not in `patches/`. Copy one back and reconfigure.
