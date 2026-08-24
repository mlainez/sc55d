# Candidate patches

Patches that are **not applied by any build**. The build globs `patches/*.patch`
only, so nothing in this directory is picked up — that is deliberate.

A patch lands here when it is correct but its *value* cannot be established.
Shipping a change whose sign is unknown is worse than not shipping it.

The sub-MCU timer patch used to live here and has now graduated: the gate was
one measurement of the prescaler reload on real firmware, which turned out to
be 124 against a break-even of 3. It is `patches/0013`.

## `0002-pcm-skip-idle-voice-slots.patch`

Skips the whole slot body for a voice that is keyed off. The proof is real and
subtle: a key-clear slot's results are all discarded or overwritten by the
`if (!active)` clear, and `pan`/`rc` force the sample and reverb contributions
to exactly zero so the accumulator updates collapse. Three exclusions were
found **by the differential test rather than by reading** — `pcm.nfs` must be
set, slots 28–31 always take the full path because their register rows *are*
the mixer and reverb state, and slot `nslots-1` always does too because
`reg_slots` runs 1..32 so it is not always slot 31.

**Why it is not shipped:** it makes the worst case worse.

| | before | after |
|---|---|---|
| 32 voices on (**the case that decides realtime**) | 27741 | 28284 (**+2.0%**) |
| mixed key traffic | 25727 | 12491 (−51%) |
| all idle | 24842 | 7279 (−71%) |

Realtime is decided by the worst case, and this loses there — the +2% is extra
live range forced on the main body, and branch hints, a running shift mask and
an in-body branch were all tried without removing it. Ship it only if average
CPU draw matters more to you than headroom, which for a synth it usually does
not.

## Context worth keeping in view

The entire sub-MCU is about **13.7%** of retired instructions, and a user who
can run the `scb55` romset removes all of it: **6,649M instructions versus
7,865M for mk2, −15.5%**. That single option is more than ten times this
patch's best case. Optimising the sub-MCU is worth doing only for people who
specifically need an mk2.
