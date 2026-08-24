# MidiQueue tests

`MidiQueue` carries MIDI bytes from the sequencer thread to the render thread,
and exists so that the emulation core — whose own UART FIFO has no
synchronisation at all — is only ever written by one thread. If this queue
garbles a byte, the symptom is a stuck note or a program change that never
happens, appearing at random on hardware and never on a desktop.

## Running

```sh
./run.sh
```

Builds and runs three ways (optimised, ThreadSanitizer, ASan+UBSan), then runs
the mutants. Everything goes under `$TMPDIR`; nothing is written in the repo.
Non-zero exit on any failure.

## What is checked

**Stream correctness.** A producer thread pushes messages of 1–12 bytes whose
every byte is a function of its position in the stream, with a period of 251 —
coprime with the 8 KiB ring, so a slot left over from the previous lap cannot
pass for the right value. The consumer drains and verifies the whole stream is
delivered exactly once, in order. A duplicate, a gap, a torn message or a stale
slot all surface as a mismatch at a known offset.

The run also fails if the consumer never once found the queue empty: if the
threads did not actually interleave, the test proved much less than it looks.

**Drop accounting**, single-threaded so the arithmetic is exact: `Dropped()`
matches the bytes refused to the byte, drops are whole messages (half a sysex
reaching the MCU would be worse than none of it), and what comes out is the
correct prefix of the stream.

**Capacity**, measured rather than assumed: the queue holds 8192 bytes and
refuses 8193 whole. That number is load-bearing. `midi_in.cpp` decodes into a
buffer of exactly 8192, so a queue one byte smaller would refuse a maximal
sysex that ALSA had decoded perfectly well — every time, even when completely
empty.

**Counter wrap.** `head_` and `tail_` are 32-bit and monotonic, so they wrap
after 4 GiB. The test pushes the counters all the way round 2^32 with every
byte verified. It holds because 2^32 is an exact multiple of the ring size, so
masking is unaffected, and the counters are only ever compared with `==` or
subtracted modularly — there is not one relational comparison on them. The
mutant `counters-compared-relationally` exists to keep it that way, and only
this phase catches it.

**ThreadSanitizer** is the important one. The bug this queue exists to prevent
is a missing release/acquire pair, which on x86 is invisible at runtime — the
hardware does not reorder stores, so a functional test passes whether the
ordering is right or not. TSan is the only thing here that can see it.

## Mutants

A test that cannot fail is not evidence, so `run.sh` breaks the queue fourteen
ways on purpose and requires the test to notice — including a guard that fails
loudly if a mutation did not actually apply, which is how a mutant harness
silently turns into a no-op. Ordering mutants are built under TSan, since
nothing else can catch them; some are only caught by ASan.

Three of them (`post-empty-block`, `post-two-bytes`, `post-whole-block`) break
the *call* into the core rather than the queue's bookkeeping — a wrong length,
or a pointer into the wrong slot — because the stub standing in for
`Core::PostMidi` would otherwise accept anything.

A mutation that turns out not to change behaviour is removed with the argument
for why, not chased with a new assertion. Two were considered and rejected on
that basis: allowing the ring to fill completely (`head == tail` cannot alias
"full" when the counters are monotonic), and deleting the empty-queue early-out
from `Drain()` (with `head == tail` the loop body never runs and the store
writes back the value already there).

## What is *not* covered

- The wiring between `MidiIn::Run()` and this queue, and between
  `MidiIn::DrainToCore()` and the core. Those need an ALSA sequencer and a
  loaded ROM set; they are reviewed, not executed here.
- Anything about real-world MIDI timing, ALSA behaviour or realtime scheduling.
- Weak memory ordering *in practice*. These tests run on x86, which is too
  strongly ordered to exhibit the failure. TSan's model is the substitute, and
  it is a model.
