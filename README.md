# sc55d

A headless Linux frontend for the [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55)
Roland SC-55 emulation core, aimed at a Raspberry Pi 4 (and, with tuning, a
Pi 3) running Raspberry Pi OS Lite. MIDI arrives over the ALSA sequencer, audio
leaves over ALSA, and there is no GUI, no SDL and no JACK — the only dependency
is `libasound`.

The core is built from the [jcmoyer fork](https://github.com/jcmoyer/Nuked-SC55)
in `vendor/nuked-sc55`, which packages the emulator as a library with no SDL
dependency. Nothing in the submodule is patched.

## Building

Raspberry Pi OS Lite:

```bash
sudo apt install build-essential cmake git libasound2-dev
git clone --recurse-submodules <this repo>
cd sc55d
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSC55D_CPU=cortex-a72
cmake --build build -j4
```

Use `-DSC55D_CPU=cortex-a53` on a Pi 3, `cortex-a76` on a Pi 5. The core needs a
C++23 compiler; GCC 12 (Bookworm) is enough.

If the tree was cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

`-DNUKED_DIR=/path/to/checkout` builds against a core checkout somewhere else
instead of the submodule.

Read [Performance](#performance) before deploying — the build flags are worth
more than anything else you can do, especially on a Pi 3.

## ROMs

sc55d ships no ROM data. Put the ROM files in a directory and point `--roms` at
it. By default they are identified by **SHA-256 content hash**, so file names do
not matter and a corrupt or wrong-revision dump is caught at startup rather than
becoming mysterious noise later. `--no-verify-roms` falls back to matching
upstream's file names.

`--list-models` prints every set the core recognises — `mk2`, `st`, `mk1`,
`cm300`, `jv880`, `scb55`, `rlp3237`, `sc155`, `sc155mk2`, plus specific
revisions such as `mk2-v1.01`. Without `--model` the loader autodetects.

With no usable ROMs, sc55d says what it found and exits non-zero.

## Running

```bash
./build/sc55d --roms /usr/share/sc55d/roms
```

The core renders at its native rate — 66207 Hz for the SC-55mk2 family, 64000 Hz
for the mk1 and JV-880, halved again when the emulated machine turns
oversampling off. sc55d opens the ALSA device at that rate through the plug
layer and lets ALSA resample; `--audio-device` picks something other than
`default` (`aplay -L` lists them).

Latency is `--period-frames` × `--periods`; the defaults (256 × 3) are about
11.6 ms at 66207 Hz. Raise `--periods` if the log shows xruns — each one is
reported as it happens and the total is printed at shutdown.

`--cpu <n>` pins the render thread to one core. sc55d also calls `mlockall()`
and asks for `SCHED_FIFO` (priority 70, `--priority` to change); both are best
effort and warn rather than fail without privileges. `--no-realtime` skips them.

The emulator's own log messages are capped at 100 (`--core-log-limit`, 0 for no
cap) and `--quiet-core` silences them. This is not cosmetic: a ROM the core is
unhappy with can emit millions of messages per second, and on a real device that
flood alone will cause xruns.

`SIGINT`/`SIGTERM` shut it down cleanly. Full options: `sc55d --help`.

### MIDI in

sc55d registers an ALSA sequencer client called `sc55d` with one writable port,
so anything can be routed to it with `aconnect`:

```bash
$ aconnect -l
client 14: 'Midi Through' [type=kernel]
    0 'Midi Through Port-0'
client 128: 'sc55d' [type=user]
    0 'midi in'
client 129: 'ttymidi' [type=user]
    0 'MIDI in'
    1 'MIDI out'
$ aconnect 129:0 128:0
```

`aconnect` takes client names too, which survive the numbers moving around:

```bash
aconnect 'ttymidi':0 'sc55d':0
```

Check it took with `aconnect -l` — the sc55d port should now list
`Connecting From: 129:0`.

#### Serial MIDI on ttyAMA0 at 31250 baud

A MIDI DIN input wired to the Pi's UART (through the usual 6N138-style
opto-isolator) needs three things: the UART freed from the console, the port
running at MIDI baud, and something bridging the tty to the ALSA sequencer.

1. Free `ttyAMA0`. In `/boot/firmware/cmdline.txt` remove
   `console=serial0,115200`, and in `/boot/firmware/config.txt` add:

   ```
   enable_uart=1
   dtoverlay=disable-bt
   dtoverlay=midi-uart0
   ```

   `disable-bt` moves the PL011 off Bluetooth and onto the GPIO header pins.
   `midi-uart0` retunes the UART base clock so that asking for 38400 baud puts
   exactly 31250 baud on the wire — the Pi cannot divide down to 31250 from the
   stock clock, which is why the overlay exists. Reboot afterwards.

2. Bridge the tty to the sequencer. [ttymidi](https://github.com/cjbarnes18/ttymidi)
   is the usual choice and creates an ALSA sequencer client of its own:

   ```bash
   ttymidi -s /dev/ttyAMA0 -b 38400 &
   ```

   38400 here is what you ask the driver for; the overlay makes it 31250 on the
   wire. A bridge that can set a non-standard baud rate directly (`BOTHER`
   termios) can skip the overlay and use `-b 31250`.

3. Connect it, as above:

   ```bash
   aconnect 'ttymidi':0 'sc55d':0
   ```


### Benchmark

`--bench` is the go/no-go number for a board: it loads the ROMs, feeds a dense
sixteen-part sequence generated in code (no external file), renders 30 seconds
of audio as fast as the machine allows, discards the samples, and reports how
many seconds of audio it produced per wall-clock second.

```bash
./build/sc55d --roms /usr/share/sc55d/roms --bench
```

```
  rendered      30.00 s of audio (1986210 frames at 66207 Hz)
  wall clock    ...
  realtime      ...x  (rendered seconds per wall-clock second)
  worst second  ...x

  verdict       ...
```

A one-second warm-up (firmware boot plus a GS reset) runs before the clock
starts. **`worst second` is the number that matters** — the lowest ratio over
any one second of the run. The average can hide a stall that would be an xrun in
real use. At or above 1.0x holds realtime; the exit status is 0 when it does and
1 when it does not.

Run it the way the daemon will run: same `--cpu`, same `--model`, same
privileges. And compare like with like — the ratio moves several-fold with build
flags, so re-benchmark after changing them.

## Performance

The core is an interpreter running a cycle-level model of two CPUs and a PCM
chip. It is one serial dependency chain, so it lives or dies on single-core
throughput and memory latency. In rough order of how much they buy you:

### Build flags

| Flag | Why |
|---|---|
| `-DSC55D_CPU=cortex-a53` / `cortex-a72` | Gives the compiler the real pipeline model. Worth most on the **Pi 3**, whose Cortex-A53 is *in-order* — it cannot hide a badly scheduled load the way the Pi 4's out-of-order A72 does. |
| `-DSC55D_LTO=ON` (default) | Lets the PCM, MCU and sc55d translation units inline into each other. The render loop calls across all three per emulated instruction. |
| `-DSC55D_PGO=generate` → `use` | Biggest single win for an interpreter. The opcode dispatch is one enormous indirect branch; profile data lets the compiler lay out the hot handlers together and predict the branches. |
| 64-bit OS | The core counts cycles in `uint64_t` everywhere. A 32-bit armhf userland pays for every one of those. The Pi 3 is aarch64-capable — use the 64-bit image. |

The PGO cycle, using the benchmark as the training run:

```bash
cmake -S . -B build-pgo -DCMAKE_BUILD_TYPE=Release \
      -DSC55D_CPU=cortex-a53 -DSC55D_PGO=generate
cmake --build build-pgo -j4
./build-pgo/sc55d --roms /usr/share/sc55d/roms --bench --bench-seconds 30

cmake -S . -B build-pgo -DSC55D_PGO=use
cmake --build build-pgo -j4
```

Train on the model you will actually run — the profile bakes in which code paths
matter, and mk2 and mk1 take different ones.

### Pick a cheaper machine

Not every SC-55 costs the same to emulate, and the differences are large:

- **`--model scb55` (or `rlp3237`) has no sub-MCU.** On an mk2 the core emulates
  a *second* CPU, stepping it after every instruction of the first. The SCB-55
  is the same sound engine on a card with no front panel, so that whole second
  interpreter collapses into cheap UART polling. On a Pi 3 this is likely the
  difference between working and not.
- **`--model mk1` and `jv880` run at 64000 Hz** rather than 66207 Hz — about
  3.3% fewer samples to produce, and the mk1 also has no sub-MCU.
- sc55d installs **no LCD backend**, so the core skips LCD emulation entirely.
  Nothing to configure; it is simply work the desktop frontend does and we do not.

### System tuning

```bash
# CPU governor: ondemand ramps, and the ramp is an xrun
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# /boot/firmware/cmdline.txt — hand core 3 to sc55d alone, keep IRQs off it
isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2
# then run with --cpu 3
```

- **ALSA resampling is not free.** 66207 Hz is not a rate any card supports, so
  the plug layer converts every sample. The default converter can be one of the
  Speex ones; `linear` costs a fraction of that:

  ```
  # /etc/asound.conf
  defaults.pcm.rate_converter "linear"
  ```

- **Avoid the 3.5 mm jack.** It is PWM-driven, sounds poor, and adds work. An
  I²S DAC HAT or a USB DAC is better on both counts.
- **Trade latency for safety on a Pi 3.** `--period-frames 512 --periods 4` is
  about 31 ms and far harder to starve than the 11.6 ms default.
- **Watch thermals.** A Pi 3B+ throttles without a heatsink; check
  `vcgencmd get_throttled`.
- Bluetooth is already disabled if you followed the serial MIDI setup; also turn
  off Wi-Fi power saving if the box is headless and wired.

### Core patches

`patches/` holds performance patches applied at build time to a copy of the
core; the submodule itself is never modified. They are **off by default** —
`-DSC55D_PATCH_CORE=ON` enables them — because they change emulator behaviour in
principle and have not been checked against real ROM audio yet.
`patches/README.md` has the two-minute digest procedure for validating them on
your ROMs, and lists what was measured and rejected.

`--bench` prints an FNV-1a digest of the audio it renders precisely so this
check is easy. A digest marked `(SILENT)` means the run produced no audio and
the comparison is meaningless.

`docs/arm-optimization.md` covers the questions that come up next: whether NEON,
the Pi's GPU, or an off-the-shelf ARM-optimised library can be pointed at any of
this. Summary: not at the emulator, which is bit-exact hardware modelling rather
than DSP — but yes at the resampler, which is a one-line `/etc/asound.conf`
change to a NEON-backed converter.

### Where the time actually goes

Callgrind, on a synthetic 32-slot PCM workload (no real ROMs — see the caveat
below):

| | share of retired instructions |
|---|---|
| `TIMER_Clock` | 29% |
| `PCM_Update` | 28% |
| `SM_Update` (sub-MCU) | 12% |
| `MCU_Interrupt_Handle` | 4% |
| `PCM_ReadROM` | 3% |

Two things worth knowing from that. `TIMER_Clock` being the largest is a
surprise, and it is what `patches/0001` targets. And `SM_Update` is 12% that
simply disappears if you can use the `scb55` romset, which has no sub-MCU.

Caveat: this profile used placeholder ROMs, so the MCU interpreter itself is
under-represented — real firmware executes varied instructions instead of
trapping. `TIMER_Clock` and `PCM_Update` are driven by cycle count rather than
ROM content, so their absolute cost per second of audio is right; the MCU's
share on top of them is not. Re-profile on the target with real ROMs before
optimising anything else.

### Known ceiling

sc55d renders and writes on one thread, so the ALSA buffer is the only slack: a
scheduling hiccup longer than the buffer is an xrun even when the average ratio
is comfortable. If `--bench` says a board is near 1.0x, more buffer is the first
answer; a render-ahead design (non-blocking writes, rendering in sub-period
chunks between `snd_pcm_avail()` checks) would absorb jitter at the cost of MIDI
latency, and is the obvious next change if buffering is not enough.

## Testing on a Raspberry Pi

One command does the whole readiness check on the board:

```bash
git clone --recurse-submodules <this repo>
cd sc55d
./scripts/pi-check.sh --roms /path/to/roms
```

It reports the model and whether the userland is 64-bit, picks `-mcpu` from the
CPU part number, builds, runs the patch equivalence tests, then checks the
things that actually decide whether audio glitches — cpufreq governor,
throttling flags, `isolcpus`, the RTPRIO limit, whether an ALSA device exists —
and finishes with the benchmark. It changes nothing; it only tells you what to
change. Without `--roms` it does everything except the benchmark.

**A Pi 3 needs the 64-bit image.** The core counts cycles in `uint64_t`
throughout and a 32-bit armhf userland pays for every one of them. The script
warns if it finds `armv7l`.

Before enabling the core patches, prove they are safe on your ROMs:

```bash
./scripts/validate-patches.sh --roms /path/to/roms
```

That builds the core patched and unpatched, renders the same sequence with
each across every romset, and compares audio digests. Only then:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSC55D_CPU=cortex-a53 \
      -DSC55D_PATCH_CORE=ON
```

### What has been verified off-board

Short of a real Pi, the following were checked here:

- Builds and runs correctly cross-compiled for **`-mcpu=cortex-a53`** and
  executed on aarch64 (`cmake/aarch64-linux-gnu.cmake` is the toolchain file).
- Both patch equivalence tests pass compiled for A53 and run on aarch64.
- Builds with **GCC 12**, which is what Raspberry Pi OS Bookworm ships — the
  core requires C++23, so that was worth confirming.

What has *not* been verified is anything about speed on real hardware, or the
audio path, since there are no ROMs and no sound device here. The benchmark on
your Pi is the only number that settles whether this works.

## Installing as a service

`contrib/sc55d.service` is a systemd unit (`After=sound.target`,
`Restart=always`):

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

The unit runs as a dedicated `sc55d` user in the `audio` group; `LimitRTPRIO`
and `LimitMEMLOCK` are what let an unprivileged process get `SCHED_FIFO` and
`mlockall()`. Drop the `User=`/`Group=` lines to run as root instead, and edit
`ExecStart` for your ROM path, audio device and CPU pinning.

## How it fits together

```
ALSA sequencer ──► midi_in.cpp ──► Emulator::PostMIDI() ──► emulated serial port
                                                                   │
                   main.cpp render loop ──► Emulator::Step() ──────┤ Nuked-SC55
                                                                   │
ALSA pcm  ◄── audio_out.cpp ◄── Core::Frames() ◄── sample callback ┘
```

One thread renders and writes: it steps the core until a period of frames is
ready, hands that period to `snd_pcm_writei()`, and repeats. The blocking write
paces the emulation — no timers, no drift. MIDI decoding runs on its own thread
and posts raw bytes into the core's UART FIFO.

The core hands finished frames to a sample callback, which clamps them to 16-bit
and appends to a small linear buffer. Because the loop only steps until one
period is ready, and one instruction yields at most two frames, that buffer never
holds much more than a period — so there is no ring, no wrap handling, and the
bytes go to ALSA without another copy.

### Why this fork

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
vendor/nuked-sc55/      the emulation core, unmodified (submodule)
```

## Licence

The emulation core in `vendor/nuked-sc55` is under the **original (pre-2016)
MAME licence**, which is not an open-source licence: redistribution is allowed,
but **not selling it and not using it in a commercial product or activity**, any
modified binary must ship complete source, and the copyright notice and
conditions must be reproduced with the distribution. See
`vendor/nuked-sc55/LICENSE`.

Those terms cover anything built from this repository, because sc55d is useless
without the core. That also makes the combination GPL-incompatible — the GPL
does not allow the extra non-commercial restriction — so sc55d cannot be merged
into a GPL project such as mt32-pi.

No ROM data is included or downloaded. The SC-55 ROMs are Roland's copyright and
have to come from your own hardware.
