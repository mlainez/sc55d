# Running sc55d

Everything sc55d does from the command line: choosing an audio device, trading
latency against safety, using a second CPU core, and proving the box makes a
sound. `sc55d --help` lists every option; this page explains the ones where the
right answer is not obvious.

```bash
./build/sc55d --roms /usr/share/sc55d/roms
```

`SIGINT`/`SIGTERM` shut it down cleanly.

## Audio device and sample rate

The core renders at its native rate — 66207 Hz for the SC-55mk2 family, 64000 Hz
for the mk1 and JV-880, halved again when the emulated machine turns
oversampling off. sc55d opens the ALSA device at that rate through the plug
layer and lets ALSA resample; `--audio-device` picks something other than
`default` (`aplay -L` lists them).

66207 Hz is not a rate any sound card supports, so ALSA converts every sample.
Which converter it uses is worth one line of config — see
[Performance § System tuning](performance.md#system-tuning).

## Latency and buffers

Buffer latency is `--period-frames` × `--periods`; the defaults (256 × 3) are
about 11.6 ms at 66207 Hz. `--render-ahead <n>` adds another `n` periods on top
of that, because the renderer is working that far in front of the speaker.
Raise `--periods` if the log shows xruns — each one is reported as it happens
and the total is printed at shutdown.

On a Pi 3, `--period-frames 512 --periods 4` is about 31 ms and far harder to
starve than the 11.6 ms default.

## Render-ahead: using a second core

`--render-ahead <n>` is what lets sc55d use a second core: the render thread
fills a ring of `n` periods while the output thread blocks in ALSA. `0` puts
both back on one thread for the lowest possible latency, and is the default on
a single-core machine, where a ring only buys a mutex, a condition variable and
a period of latency.

The default is **a fixed ~15 ms** rather than a fixed number of periods — 8 at
128 frames, 4 at 256, 2 at 512. That is not arbitrary. The stalls a ring exists
to absorb are scheduling artefacts, and they last a fixed number of
milliseconds no matter how you have sized your periods; measured over 80
minutes of rendered audio here, the emulator's *own* worst period never
exceeded 0.8x of its budget, while the worst observed period was 4–70 ms at
every period size, and an emulator-free control loop on the same machine
stalled by the same amounts. So the useful depth is an amount of time, and the
period count that takes scales with `1/period-frames`. At shutdown sc55d
prints how many times the output thread found the ring empty ("starves") and
how close it came at the worst moment; a non-zero starve count means the core is
genuinely too slow here, not that something interrupted it.

Starves and xruns mean different things, and the distinction is the whole
diagnosis — see [Performance § Known ceiling](performance.md#known-ceiling).

## Pinning and realtime priority

`--cpu <n>` pins the render thread to one core and `--output-cpu <n>` the output
thread. sc55d also calls `mlockall()` and asks for `SCHED_FIFO` (renderer at
priority 70, `--priority` to change); both are best effort and warn rather than
fail without privileges. `--no-realtime` skips them.

Thread priorities, highest first: output, MIDI, render. The reasoning is in
[Architecture](architecture.md).

## Keeping the emulator quiet

The emulator's own log messages are capped at 100 (`--core-log-limit`, 0 for no
cap) and `--quiet-core` silences them. This is not cosmetic: a ROM the core is
unhappy with can emit millions of messages per second, and on a real device that
flood alone will cause xruns.

## Does it make a sound? `--selftest`

`--selftest <seconds>` plays the benchmark's sixteen-part stress sequence to the
audio device in realtime, through the same MIDI queue the sequencer feeds. It is
the quickest way to answer the first question anyone has on a fresh box — *does
this thing make a sound?* — without setting up `aconnect` and a MIDI file
player:

```bash
sc55d --roms /path/to/roms --gs --selftest 20
```
