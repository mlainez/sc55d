# How sc55d fits together

Three threads, two hand-offs, and one deliberate rule: only the render thread
ever touches the emulator. This page explains why, what the fork of the core
buys us, and where everything lives in the tree.

```
   (MIDI thread)   ALSA sequencer ──► midi_in.cpp ──► midi_queue.cpp
                                                            │
   ─────────────────────────────────────────────────────────┼───────────────
                                                            ▼
   (render thread)  main.cpp ──► Emulator::PostMIDI() ──► emulated serial port
                        │                                          │
                        ├───────► Emulator::Step() ────────────────┤ Nuked-SC55
                        │                                          │
                        │            Core::Frames() ◄── sample callback
                        ▼
                     ring.cpp  ── N periods ──┐
   ─────────────────────────────────────────  │  ──────────────────────────
                                              ▼
   (output thread)              audio_out.cpp ──► snd_pcm_writei() ──► ALSA pcm
```

Three threads, and exactly two places where data crosses between them: the MIDI
queue and the period ring.

**MIDI** decodes sequencer events into raw bytes and puts them in the queue.
**Render** takes them out, hands them to the emulated serial port, steps the
core until a period of frames is ready, and drops that period into the ring.
**Output** takes periods off the ring and blocks in `snd_pcm_writei()`.

Only the render thread ever calls into the emulator, and that is deliberate.
The core's own UART FIFO is a single-producer ring with no synchronisation at
all — `MCU_PostUART()` stores the byte and then bumps the pointer, both plain
stores, while the core polls the pointer and then reads the byte, both plain
loads. Posting to it from a MIDI thread, which is what upstream's RtMidi
callback does, is a data race, and on a weakly ordered Cortex-A53 a real one:
the core can see the advanced pointer before the byte it points at and take
whatever was in that slot the previous lap of the 8 KiB buffer. A corrupted
status byte is a stuck note — rare, silent and miserable to find. Putting a
properly synchronised queue in front of it makes the core's FIFO what it was
always written as: single-threaded. [patches/README.md](../patches/README.md) has the measurement
that says why this is not fixed inside the core instead.

The blocking write still paces everything — no timers, no drift — but now it
paces the renderer *through* the ring rather than by standing in front of it.
That is the whole point: on a multi-core board the render thread keeps working
while the output thread is asleep in ALSA, and the periods queued between them
absorb a late wake-up instead of turning it into an xrun. It does not make the
emulator faster. If the core cannot sustain realtime on average the ring simply
drains, which sc55d counts and reports as *starves* — the number that tells you
"this board is too slow" apart from "this board was interrupted".

`--render-ahead 0` collapses render and output back into one thread, which is
lower latency and the only thing that makes sense on a single core.

The core hands finished frames to a sample callback, which clamps them to 16-bit
and appends to a small linear buffer. Because the loop only steps until one
period is ready, and one instruction yields at most two frames, that buffer never
holds much more than a period. One `memcpy` moves it into a ring slot; at the
default settings that is a few hundred KiB a second, and it is cheaper than
teaching the callback to cope with a slot boundary landing mid-instruction.

Thread priorities, highest first: output, MIDI, render. The output thread must
never miss a wake-up, and its work is bounded and small — the plug layer's rate
conversion, which happens inside `snd_pcm_writei()` and is measured under
[Performance § System tuning](performance.md#system-tuning). MIDI is idle until an event arrives and
only adds latency if made to wait. The renderer wants every cycle it can get, so
it goes last. `--priority` sets the renderer's; the other two sit one and two
steps above it.

## Why this fork

Upstream's frontend is not a separate file: `main()`, the SDL audio setup and
the work thread all live in `mcu.cpp` next to the MCU, and `mcu.h` includes
`SDL_atomic.h`. Building a headless frontend against it meant a stand-in SDL
header set, a compile-time rename of upstream's `main`, and no-op LCD stubs.
The [jcmoyer fork](https://github.com/jcmoyer/Nuked-SC55) removes the need for
all of that:

- the emulator is a library with **no SDL dependency** and a real API
  (`Init` / `LoadRoms` / `Reset` / `PostMIDI` / `Step` / `SetSampleCallback`);
- the LCD is an injectable backend, and passing null **skips LCD emulation**;
- ROM loading does **SHA-256 identification** with proper diagnostics, including
  specific ROM revisions;
- `PCM_GetOutputFrequency()` reports the **correct half rate** when the machine
  is not oversampling, which upstream gets wrong;
- the core's log output goes through a **callback** we can cap;
- their changelog reports *"optimized interrupt handling for a 10-16% overall
  performance improvement."*

It tracks upstream behaviour deliberately, including bugs, and carries the same
licence. Bugs reproducible on both belong upstream.

## Layout

```
CMakeLists.txt          one executable target, sc55d
cmake/                  aarch64 cross-compilation toolchain file
contrib/sc55d.service   systemd unit
docs/                   design and optimisation notes
patches/                core patches, applied at build time
scripts/                pi-check.sh, validate-patches.sh
src/                    everything sc55d adds
tests/                  tests for sc55d's own code, each with its mutants
vendor/nuked-sc55/      the emulation core, unmodified (submodule)
```

