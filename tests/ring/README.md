# PeriodRing correctness harness

`src/ring.cpp` is the only place in sc55d where two threads touch the same
memory without a lock held across the touch: the render thread writes a slot's
audio outside the mutex, the output thread reads it outside the mutex, and the
only thing keeping them apart is the head/tail arithmetic. This harness exists
to make that claim testable without a sound card.

    ./tests/ring/run.sh

Needs no ROMs, no ALSA and no audio device. Build artifacts go to
`$TMPDIR/sc55d-ring-tests` (override with `OUT=`); nothing is written inside
the repository.

## What it covers

`ring_test.cpp` links only `src/ring.cpp` -- no emulator core, no audio
backend. Every slot carries a sequence number, a payload derived from it and a
checksum over the whole slot, and the consumer verifies each slot twice. A
torn slot fails the checksum, a slot handed out twice fails the sequence
check, a dropped slot leaves a gap, and a producer writing into a slot the
consumer still holds is caught in one of the two verification passes.

Six phases:

| phase | state | what it proves |
| --- | --- | --- |
| A saturated | consumer held back until the producer is a full ring ahead | the steady state main.cpp runs in: `Starves()` is 0 and `MinFill()` never reaches 0 when the ring is never allowed to empty |
| B starving | producer sleeping, consumer free | `Starves()` actually counts, and `MinFill()` bottoms out at 0 |
| C matched | both sides free-running with random stalls, ~1M periods | the full/empty boundaries hit head-on, at speed |
| E narrow | rings of 1, 2 and 3 slots | `--render-ahead 1` is a ring where every write is on the full boundary and every read on the empty one |
| D shutdown | `Shutdown()` from a third thread mid-flight | `BeginWrite()` and `BeginRead()` both return `nullptr`, `WaitPrefilled()` reports `Closed`, and neither thread is left asleep |
| F counters at shutdown | a closed ring read once more, and a reader woken by `Shutdown()` | the counters are diagnostics main.cpp shows the user, so a `BeginRead()` that arrives on an already-closed ring must move neither of them, while a reader that really waited still counts its starve |

Accounting checked at the end of every phase: nothing is consumed that was
never committed, phases A-C come out exactly even, phase D leaves no more
queued than the ring can hold, and `MinFill()` is never above the smallest
fill the consumer actually observed.

A lost wake-up does not fail, it stops, so the test cannot rely on returning
at all: a watchdog thread fails the run if no period moves for 10 seconds.

`run.sh` builds it three ways -- plain `-O2`, ThreadSanitizer, and
AddressSanitizer + UndefinedBehaviorSanitizer -- then runs `mutants.sh`.

`mutants.sh` breaks a *copy* of `src/ring.cpp` (the original is never touched)
nine different ways and requires the test to catch each one: the two dropped
`notify_one()` calls, `head_++` outside the mutex, `BeginWrite()` off by one
slot, `<=` instead of `<` in the full check, `BeginRead()` ignoring `done_`,
the two counters left untouched, and the counters moved back in front of the
`done_` check. Each copy is diffed against the original
first, so a mutation whose pattern has drifted out of the source is reported
as an error rather than silently scoring a survivor. A mutant counts as killed
if either the plain or the ThreadSanitizer build catches it: `head_++` outside
the mutex reorders nothing on x86, so what it actually costs is the ordering
between the increment and the consumer's predicate -- some runs lose a wake-up
on the 1-slot ring and stop, others get through the plain build untouched and
are only caught by TSan reporting the race on `head_`.

## What it does not cover

* **ALSA.** Nothing here opens a device, so it says nothing about
  `snd_pcm_writei()`, xruns, buffer sizing or recovery.
* **Realtime scheduling.** `SCHED_FIFO`, CPU pinning and priority are not
  exercised; phase A deliberately synchronises on the producer's own counter
  rather than on timing, precisely so that a descheduled thread cannot fail a
  run that is not about scheduling.
* **Whether the emulator sustains realtime.** The ring cannot make the core
  faster; that is what the starve counter is for at runtime.
* **Non-x86 memory models.** The plain build proves little about ordering on a
  weakly ordered machine -- that is what the ThreadSanitizer stage is for, and
  it is the stage that catches an unsynchronised `head_++`.
* **More than one producer or one consumer.** `PeriodRing` is SPSC by
  contract and the harness only ever runs one of each.
