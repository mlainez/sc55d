# sc55d

**Disclaimer**

This project has been almost entirely written by Claude. It was meant as an exploration in order to increase the performance of Nuked-SC55 to allow it to run on ARM devices such as the rpi3 and rpi4. Be advised. Use at your own risk. Hallucinations may have happened.

**Turn a Raspberry Pi into a Roland SC-55.**

The Roland SC-55 was the desktop MIDI sound module that defined how 1990s game
and MIDI-file music sounded. sc55d runs a faithful emulation of one on a
Raspberry Pi: you send it MIDI, it plays the music out of the Pi's audio output.

There is no window, no desktop and nothing to click. sc55d is a small background
program — a *daemon*, hence the `d` — that starts with the machine and sits
waiting for notes, the way the original box sat waiting on your desk. It uses
Linux's own MIDI and audio plumbing (ALSA) and nothing else, so it stays out of
the way of the emulator, which needs every cycle the board has.

The emulation itself is [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) — a
cycle-accurate model of the real hardware, used here through the
[jcmoyer fork](https://github.com/jcmoyer/Nuked-SC55) in `vendor/nuked-sc55`.
sc55d is the part that gets MIDI in, audio out, and both of those done in time.

## What the numbers say

Realtime ratio — seconds of audio produced per second of wall clock. **1.0x is
the line**; below it the board cannot keep up and no amount of buffering fixes
it. Measured on the boards themselves, patched and unpatched builds compiled
identically:

| board | romset | unpatched | with `patches/` |
|---|---|---|---|
| **Pi 3** | **mk1 / SC-155** | **0.715x** | **1.386x** |
| Pi 3 | mkII | 0.458x | 0.811x — still too slow |
| Pi 4 | mk1 / SC-155 | 1.497x | 2.916x |
| **Pi 4** | **mkII** | **0.928x** | **1.691x** |

Three things worth taking from that table.

**The core patches are not a tuning option.** For the romset each board is
actually asked to run, they are the difference between working and not: mk1 on a
Pi 3 is 0.715x without them, mkII on a Pi 4 is 0.928x. Both are under realtime.
The only combination that runs unpatched is mk1 on a Pi 4.

**They are worth about twice as much on ARM as on x86** — +94% on a Pi 3 against
+43.5% on a Xeon. Removed work is genuinely saved on an in-order Cortex-A53,
where a wide out-of-order core hides some of it.

**A Pi 3 still cannot run the mkII romsets.** 0.811x is under realtime, and that
is the honest boundary of what this does.

The patches are validated **bit-identical** to the unpatched core — same audio
digest on a Pi 3, a Pi 4 and x86-64 — and the patched core passes all 36 of
upstream's own SC-55mk2 integration cases byte for byte. Details and method:
[Performance](docs/performance.md), [Core patches](patches/README.md).

## What you'll need

- **A Raspberry Pi 4 or 5**, or a **Pi 3** for the mk1-generation romsets.
  Measured: a Pi 3 renders SC-55 mk1 at 1.39x realtime and cannot run the mkII
  romsets (0.81x, under realtime); a Pi 4 runs both, at 2.92x and 1.69x. A DAC
  is optional — the onboard jack holds mk1 on a Pi 3 with zero dropouts. See
  [Performance](docs/performance.md).
- **Raspberry Pi OS Lite, 64-bit.** The 32-bit image is markedly slower here.
- **SC-55 ROM files** — the original module's firmware and sounds. sc55d ships
  none of it; the ROMs are Roland's copyright and have to come from hardware you
  own. See [ROMs](#roms) below.
- **Somewhere for the audio to go.** A USB audio adapter or an I²S DAC HAT
  sounds best. The Pi's own 3.5 mm jack is noisier — it is PWM-driven — but it
  does work, and it does not cost the emulator anything: it takes the mk1's
  native 64000 Hz without resampling. It does need a longer period than a DAC
  (`--period-frames 512 --periods 4`), which is measured in
  [Performance](docs/performance.md).
- **Something to send MIDI** — a MIDI file player on the same Pi, another
  machine on the network, or a real MIDI socket wired to the Pi's serial port.
  See [Getting MIDI in](docs/midi-input.md).

## Build it

On Raspberry Pi OS Lite:

```bash
sudo apt install build-essential cmake git libasound2-dev
git clone --recurse-submodules <this repo>
cd sc55d
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSC55D_CPU=cortex-a72
cmake --build build -j4
```

Set `-DSC55D_CPU` to match the board: `cortex-a53` on a Pi 3, `cortex-a72` on a
Pi 4, `cortex-a76` on a Pi 5. A C++23 compiler is required; the GCC 12 that
Raspberry Pi OS Bookworm ships is enough.

Forgot `--recurse-submodules`? Run `git submodule update --init --recursive`.

The build flags matter more than anything else you can change afterwards, so if
the board is marginal, skim [Performance § Build flags](docs/performance.md#build-flags)
before you deploy. (`-DNUKED_DIR=/path/to/checkout` builds against an emulation
core checked out somewhere else, if you have one.)

## ROMs

Put the ROM files in a directory and point `--roms` at it. sc55d identifies them
by **content**, not by file name, so the names do not matter and a corrupt or
wrong-revision dump is caught at startup instead of turning into mysterious
noise later. `--no-verify-roms` falls back to matching file names.

sc55d supports more than one machine from the SC-55 family. `--list-models`
prints them all — `mk2`, `st`, `mk1`, `cm300`, `jv880`, `scb55`, `rlp3237`,
`sc155`, `sc155mk2`, and specific revisions such as `mk2-v1.01`. Without
`--model` it works out which set you gave it.

With no usable ROMs, sc55d tells you what it found and exits with an error.

## Hear something, right now

Before wiring up any MIDI, make the box prove it can make a noise. This plays a
dense sixteen-part test piece to the default audio device for 20 seconds:

```bash
./build/sc55d --roms /path/to/roms --gs --selftest 20
```

If that works, you have ROMs, an audio device and enough CPU. If it is silent,
the messages it prints say which of the three is missing.

## Play some music

sc55d appears on the system as a MIDI device called `sc55d`. Connect anything to
it with `aconnect`:

```bash
./build/sc55d --roms /path/to/roms &   # sc55d shows up as 'sc55d'
aconnect -l                            # list what's available
aconnect 'SerialMIDI-0':0 'sc55d':0    # send that device's MIDI to sc55d
```

Full details, including how to wire a real MIDI DIN socket to the Pi's serial
port: [Getting MIDI in](docs/midi-input.md).

## Will it run properly on my board?

One command answers it. `--bench` renders 30 seconds of deliberately busy music
as fast as the machine can and reports how many seconds of audio it produced per
second of real time:

```bash
./build/sc55d --roms /path/to/roms --bench --no-realtime
```

**1.0x is the line.** Above it the board can keep up; below it, it cannot, and
no amount of configuration will fix that. Watch the `worst second` figure rather
than the average — one bad second is one audible glitch. The exit status is 0
when the board holds realtime, so this works in a script.

For the whole readiness check — build, tests, system settings, benchmark — run
it on the board itself:

```bash
./scripts/pi-check.sh --roms /path/to/roms
```

It changes nothing; it only tells you what to change. See
[Testing on a Raspberry Pi](docs/testing.md) and
[Benchmarking a board](docs/benchmarking.md).

## Run it in the background

`contrib/sc55d.service` is a systemd unit, so sc55d starts with the machine and
restarts if it ever dies:

```bash
sudo install -Dm755 build/sc55d /usr/bin/sc55d
sudo install -Dm644 contrib/sc55d.service /etc/systemd/system/sc55d.service
sudo install -d /usr/share/sc55d/roms
sudo cp /path/to/roms/*.bin /usr/share/sc55d/roms/

sudo useradd --system --no-create-home --groups audio sc55d
sudo systemctl daemon-reload
sudo systemctl enable --now sc55d
journalctl -u sc55d -f
```

Edit `ExecStart` in the unit for your ROM path, audio device and CPU pinning. It
runs as a dedicated `sc55d` user in the `audio` group; the `LimitRTPRIO` and
`LimitMEMLOCK` lines are what let that unprivileged user still get realtime
scheduling. Drop the `User=`/`Group=` lines to run as root instead.

## If the audio crackles or drops out

In rough order of how much they help — the reasoning behind each one is in
[Performance](docs/performance.md):

1. **Turn off the kernel's realtime throttle.** Not optional, and the single
   biggest fix. `sudo sysctl -w kernel.sched_rt_runtime_us=-1`. Left on, the
   kernel will stop sc55d for up to 50 ms at a time — far longer than any audio
   buffer — precisely when the board is busiest. sc55d warns at startup if it is
   still on.
2. **Nail the CPU speed down.** The on-demand governor ramps up *after* the work
   arrives, and that ramp is a glitch: `echo performance | sudo tee
   /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`.
3. **Choose a cheaper audio converter.** Put
   `defaults.pcm.rate_converter "lavrate"` in `/etc/asound.conf` — six times
   cheaper than the default, and the alternatives can cost several times more.
4. **Use a bigger buffer.** `--period-frames 512 --periods 4` is about 31 ms of
   latency instead of 12, and much harder to disturb.
5. **Emulate a cheaper machine.** `--model scb55` is the same sound engine
   without the front panel, and skipping that second processor saves about 15%
   of all the work. On a Pi 3 this can be the difference between working and not.
6. **Check the small things.** If you are on the 3.5 mm jack, give it
   `--period-frames 512 --periods 4` — it will not sustain the short periods a
   DAC is happy with. Keep the board cool (`vcgencmd get_throttled`), and turn
   off Wi-Fi power saving on a wired box.

sc55d distinguishes the two failure modes for you at shutdown. **Starves** mean
the emulator itself could not keep up — a faster board, better build flags or a
cheaper model is the answer. **Xruns with no starves** mean the audio output was
late while the emulator was fine — more buffer or better isolation is the answer.

## Documentation

| | |
|---|---|
| [Running sc55d](docs/running.md) | Every option that needs explaining: devices, latency, using a second core, realtime priorities |
| [Getting MIDI in](docs/midi-input.md) | `aconnect`, and wiring a real MIDI DIN socket to the Pi's UART |
| [Benchmarking a board](docs/benchmarking.md) | `--bench` and how to read it, including per-period timing |
| [Performance](docs/performance.md) | What was measured and what to change: build flags, model choice, system tuning, profiles, and the hard ceiling |
| [Testing on a Raspberry Pi](docs/testing.md) | `pi-check.sh`, validating the core patches on your ROMs, and what is *not* yet verified |
| [Architecture](docs/architecture.md) | The three threads, why only one of them touches the emulator, why this fork, and the repository layout |
| [Core patches](patches/README.md) | The thirteen performance patches, how they are validated, and what was tried and rejected |
| [SIMD, GPU offload, ARM libraries](docs/arm-optimization.md) | Whether NEON or the GPU can help (mostly no, and why) — plus the Pi 3 verdict this document got wrong, and what the mistake was |
| [Backend options](docs/backend-options.md) | What replacing the emulation core would mean — written when a Pi 3 looked unreachable, and marked where that no longer holds |

## How it works, briefly

Three threads. One reads MIDI off the system's sequencer and puts the bytes in a
queue. One takes them out, feeds them to the emulated hardware, and runs the
emulator until a chunk of audio is ready. One takes those chunks and hands them
to the sound card, which blocks until the speaker is ready for more — and that
is what keeps the whole thing in time, with no clocks or timers anywhere.

Only the middle thread ever touches the emulator, which is deliberate and is the
reason for the queue: the emulator's own input buffer is written as
single-threaded code, and posting to it from elsewhere is a real race on a Pi
that produces stuck notes. [Architecture](docs/architecture.md) has the details.

## Licence

Two licences apply here, and the difference is the whole point of this section.

- **sc55d's own code is MIT** — everything in `src/`, `tests/`, `scripts/`,
  `contrib/`, `cmake/`, `docs/` and the top-level build files. See
  [`LICENSE`](LICENSE).
- **The emulation core is not.** `vendor/nuked-sc55` is under the original
  (pre-2016) **MAME licence**, which is not an open-source licence. So are the
  patches in `patches/`, which quote core source as diff context. Full text in
  [`NOTICE`](NOTICE), reproduced there because that licence requires it.

**MIT does not make a built sc55d MIT-licensed.** Any binary from this
repository contains the core, so the core's terms apply to it — and they are
restrictive:

- **You may not sell it**, in any form, including a disk image or a device with
  it preinstalled.
- **You may not use it in a commercial product or activity** — not a paid
  service, not a commercial studio's pipeline, not part of anything you charge
  for. A personal build for your own music is exactly what is fine.
- **A modified build must ship its complete corresponding source.**
- **[`NOTICE`](NOTICE) must travel with any copy you pass on.**
- **This combination cannot be relicensed under the GPL**, which does not permit
  the added non-commercial restriction.

[`LICENSING.md`](LICENSING.md) spells all of that out file by file, including
what to do if you need commercial use (short version: the restriction is the
core's to lift, so the only real path is a different backend — see
[backend options](docs/backend-options.md)).

**No ROM data is included or downloaded.** The SC-55 ROMs are Roland's copyright,
no licence here grants you any right to them, and they have to come from
hardware you own.
