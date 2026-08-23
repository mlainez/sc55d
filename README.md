# sc55d

A headless Linux frontend for the [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55)
Roland SC-55 emulation core, aimed at a Raspberry Pi 4 running Raspberry Pi OS
Lite (aarch64). MIDI arrives over the ALSA sequencer, audio leaves over ALSA,
and there is no GUI, no SDL and no JACK — the only dependency is `libasound`.

The emulation core is built straight from the `vendor/nuked-sc55` submodule and
is never patched; everything sc55d needs on top of it lives in `src/`.

## Building

Raspberry Pi OS Lite:

```bash
sudo apt install build-essential cmake git libasound2-dev
git clone --recurse-submodules <this repo>
cd sc55d
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

If the tree was cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

On a Pi 4 it is worth telling the compiler what it is building for:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-mcpu=cortex-a72 -mtune=cortex-a72"
```

## ROMs

sc55d ships no ROM data. Put the ROM files in a directory of your own and point
`--roms` at it; the file names are upstream's:

| Model (`--model`) | Files |
|---|---|
| `mk2` (default)   | `rom1.bin` `rom2.bin` `waverom1.bin` `waverom2.bin` `rom_sm.bin` |
| `st`              | `rom1.bin` `rom2_st.bin` `waverom1.bin` `waverom2.bin` `rom_sm.bin` |
| `mk1`             | `sc55_rom1.bin` `sc55_rom2.bin` `sc55_waverom1.bin` `sc55_waverom2.bin` `sc55_waverom3.bin` |
| `cm300`           | `cm300_rom1.bin` `cm300_rom2.bin` `cm300_waverom1.bin` `cm300_waverom2.bin` `cm300_waverom3.bin` |
| `jv880`           | `jv880_rom1.bin` `jv880_rom2.bin` `jv880_waverom1.bin` `jv880_waverom2.bin` (+ optional expansion/PCM card) |
| `scb55`           | `scb55_rom1.bin` `scb55_rom2.bin` `scb55_waverom1.bin` `scb55_waverom2.bin` |
| `rlp3237`         | `rlp3237_rom1.bin` `rlp3237_rom2.bin` `rlp3237_waverom1.bin` |
| `sc155`           | `sc155_rom1.bin` `sc155_rom2.bin` `sc155_waverom1.bin` `sc155_waverom2.bin` `sc155_waverom3.bin` |
| `sc155mk2`        | same names as `mk2` |

Without `--model`, sc55d runs upstream's autodetect: the first complete set in
that order wins, so an SC-55mk2 set is picked up on its own. With no ROMs at all
it prints the expected file names and exits.

## Running

```bash
./build/sc55d --roms /usr/share/sc55d/roms
```

The core renders at its native rate — 66207 Hz for the SC-55mk2 family, 64000 Hz
for the mk1 and JV-880. sc55d opens the ALSA device at that rate through the
plug layer and lets ALSA resample to whatever the hardware does; use
`--audio-device` to pick something other than `default` (`aplay -L` lists them).

Latency is `--period-frames` × `--periods`; the defaults (256 × 3) are about
11.6 ms at 66207 Hz. Raise `--periods` if the log shows xruns — every xrun is
reported as it happens and the total is printed at shutdown.

`--cpu <n>` pins the render thread to one core, which on a four-core Pi 4 keeps
it off whatever else is running. sc55d also calls `mlockall()` and asks for
`SCHED_FIFO` (priority 70 by default, `--priority` to change). Both are best
effort: without the privileges it warns and carries on. `--no-realtime` skips
them entirely.

`SIGINT` and `SIGTERM` shut it down cleanly.

Full option list: `sc55d --help`.

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

`--bench` is the go/no-go number for a given board: it loads the ROMs, feeds a
dense sixteen-part sequence generated in code (no external file), renders 30
seconds of audio as fast as the machine allows, throws the samples away, and
reports how many seconds of audio it produced per wall-clock second.

```bash
$ ./build/sc55d --roms /usr/share/sc55d/roms --bench
sc55d: benchmark: 30 s of audio at 66207 Hz (after 1 s of warm-up), output discarded

  rendered      30.00 s of audio (1986210 frames at 66207 Hz)
  wall clock    ...
  realtime      ...x  (rendered seconds per wall-clock second)
  worst second  ...x

  verdict       ...
```

A one-second warm-up (firmware boot plus a GS reset) runs before the clock
starts. `worst second` is the lowest ratio over any one second of the run and is
the number that matters — the average can hide a stall that would be an xrun in
real use. Anything at or above 1.0x holds realtime; the exit status is 0 when it
does and 1 when it does not. `--bench-seconds` changes the length.

Run it the way the daemon will run: same `--cpu`, same privileges.

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
`mlockall()`. Drop the `User=`/`Group=` lines to run it as root instead, and
edit `ExecStart` for your ROM path, audio device and CPU pinning.

Routing MIDI to a service is the same `aconnect` call; a udev rule or a small
`ExecStartPost=` is the usual way to make it automatic once the serial client
appears.

## How it fits together

```
ALSA sequencer ──► midi_in.cpp ──► MCU_PostUART() ──► emulated serial port
                                                            │
                   main.cpp render loop ──► Core::Step() ──►┤ Nuked-SC55 core
                                                            │
ALSA pcm  ◄── audio_out.cpp ◄── Core::PullPage() ◄──────────┘
```

One thread renders and writes: it steps the core until a period of frames is
ready, hands that period to `snd_pcm_writei()`, and repeats. The blocking write
is what paces the emulation — no timers, no drift. MIDI decoding runs on its own
thread and posts bytes into the core's UART FIFO, exactly as upstream's RtMidi
callback does.

### What we had to do to the core, and why

Upstream's SDL frontend is not a separate file: `main()`, the SDL audio setup
and the SDL work thread all live in `src/mcu.cpp` alongside the MCU itself, and
`src/mcu.h` includes `SDL_atomic.h`. Since the submodule is never patched, the
build works around that from our side:

* **`src/sdl_compat/`** — a small stand-in for the slice of SDL2 the core names.
  Atomics, mutexes and threads are real (pthreads). The audio device is virtual:
  `MCU_OpenAudio()` still allocates the core's sample ring and registers its
  callback, and sc55d pumps that callback by hand from the render loop.
* **`main` is renamed at compile time.** `mcu.cpp` is compiled with
  `-Dmain=nuked_sc55_sdl_main_unused`, so upstream's SDL `main()` becomes dead
  code in the same object file as the MCU, and `src/main.cpp` provides the real
  entry point.
* **`src/stubs.cpp`** — no-op `LCD_*` and `MIDI_*` functions, replacing
  `lcd.cpp` (SDL video) and `midi_rtmidi.cpp`, which are left out of the build.
  One piece is not a no-op: on the SC-55mk1 the gate array raises an interrupt
  shortly after each LCD write and the mk1 firmware waits for it, so the stub
  keeps that timer running.
* **`src/romset.cpp`** — ROM selection and loading. Upstream's copy is inside
  its `main()`, so it is re-done here against the core's own file-name table,
  with the same autodetect order and the same model flags.
* **`src/core.cpp`** — the body of upstream's `work_thread()`, minus its SDL
  ring bookkeeping, plus a frame counter so the render loop knows when a period
  is ready.

Nothing in `vendor/nuked-sc55` is modified. Updating the submodule is a
`git submodule update --remote` plus a rebuild.

## Layout

```
CMakeLists.txt          one executable target, sc55d
contrib/sc55d.service   systemd unit
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
