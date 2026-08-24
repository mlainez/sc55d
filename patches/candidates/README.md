# Candidate patches

Patches that are **not applied by any build**. The build globs `patches/*.patch`
only, so nothing in this directory is picked up — that is deliberate.

A patch lands here when it is correct but its *value* cannot be established
without real ROMs. Shipping a change whose sign is unknown is worse than not
shipping it.

## `0001-submcu-collapse-the-sub-MCU-timer-loop.patch`

`SM_UpdateTimer()` runs exactly three iterations per call, most of them only
decrementing a prescaler. The loop is **44.3% of `SM_Update`**, which is 11.5%
of all retired instructions. Collapsing it removes 54% of the loop in a
plausible regime.

**Why it is not shipped.** Whether it wins depends on the prescaler reload the
firmware programs, and zero-filled ROMs drive that value to 0–1 — a regime no
real SC-55 would use, since it would mean a timer interrupt every ~16 sub-MCU
cycles. Across synthetic regimes the program-wide effect ranges from **−1.2% to
+0.8%**. Below a reload of about 3 it is a regression.

**The one measurement that decides it.** Instrument
`device_mode[SM_DEV_PRESCALER]` during a few seconds of real playback:

- reload **≥ 3** → worth roughly 1% of total instructions on an mk2; move the
  patch and its test up into `patches/`
- reload **0 or 1** → discard it

**Correctness is not in question.** `tests/sm_timer_equivalence.cpp` passes
6,374,220 cases with no ROMs and rejects seven deliberate bugs:

```bash
g++ -O2 -std=c++17 -o /tmp/smtt patches/candidates/tests/sm_timer_equivalence.cpp && /tmp/smtt
```

## Context worth keeping in view

The entire sub-MCU is about **13.7%** of retired instructions, and a user who
can run the `scb55` romset removes all of it: **6,649M instructions versus
7,865M for mk2, −15.5%**. That single option is more than ten times this
patch's best case. Optimising the sub-MCU is worth doing only for people who
specifically need an mk2.
