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

Buffer latency is `--period-frames` × `--periods`; the defaults (256 × 3) are
about 11.6 ms at 66207 Hz. `--render-ahead <n>` adds another `n` periods on top
of that, because the renderer is working that far in front of the speaker.
Raise `--periods` if the log shows xruns — each one is reported as it happens
and the total is printed at shutdown.

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

`--cpu <n>` pins the render thread to one core and `--output-cpu <n>` the output
thread. sc55d also calls `mlockall()` and asks for `SCHED_FIFO` (renderer at
priority 70, `--priority` to change); both are best effort and warn rather than
fail without privileges. `--no-realtime` skips them.

The emulator's own log messages are capped at 100 (`--core-log-limit`, 0 for no
cap) and `--quiet-core` silences them. This is not cosmetic: a ROM the core is
unhappy with can emit millions of messages per second, and on a real device that
flood alone will cause xruns.

`--selftest <seconds>` plays the benchmark's sixteen-part stress sequence to the
audio device in realtime, through the same MIDI queue the sequencer feeds. It is
the quickest way to answer the first question anyone has on a fresh box — *does
this thing make a sound?* — without setting up `aconnect` and a MIDI file
player:

```bash
sc55d --roms /path/to/roms --gs --selftest 20
```

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

A one-second warm-up is not enough with real ROMs — the firmware has not booted
and the run measures silence, which the digest line flags as `(SILENT)`. The
default is 4 s (`--bench-warmup`).

Run it the way the daemon will run: same `--cpu`, same `--model`, same
privileges. And compare like with like — the ratio moves several-fold with build
flags, so re-benchmark after changing them.

**Run it with `--no-realtime`.** Under `SCHED_FIFO` the benchmark saturates its
core, which is exactly what the kernel RT throttle punishes: the tail of the
distribution then measures the throttle rather than the emulator. See
[System tuning](#system-tuning).

#### Where the time goes within a period

`--bench-histogram` times every individual period and reports the distribution
against that period's realtime budget:

```
./build/sc55d --roms /path/to/roms --bench --bench-histogram --no-realtime
```

It prints mean, median, p90, p99, p99.9 and max as multiples of the budget, how
many periods went over it, the worst run of consecutive over-budget periods, and
the **peak cumulative deficit** — the running sum of how far behind realtime the
renderer fell, floored at zero. That last one is the number that says how deep
`--render-ahead` has to be, and it is the only one that does: the
consecutive-run figure reads 1 almost always, because a single 14 ms stall lands
inside one period rather than spreading across several, and sizing a ring from
it would give you 2 and be badly wrong.

`--bench-ring` pushes every period through a real `PeriodRing` to a consumer
thread that discards it, so the cost of the hand-off can be priced against the
same run without it. Measured here: **10–20 µs per period**, about 0.5% of the
budget at 256 frames — a 1 KiB memcpy, a mutex pair and one futex wake. It is a
fixed per-period cost, so it hurts proportionally more at smaller periods
(0.74% at 128), and on a board running near its limit it eats a larger share of
what slack is left.

## Performance

**Measured with a real SC-55mk2 ROM set** (`mk2-v1.01`), rendering the
benchmark's dense sixteen-part sequence for 30 seconds on an Intel Xeon at
2.8 GHz:

| | realtime ratio |
|---|---|
| unpatched core | 2.70x |
| **with `patches/`** (default) | **3.87x**, worst second 3.44x |

That is **+43.5%**, and it corroborates the −45.1% drop in retired instructions
measured separately. The patches are validated bit-identical on this romset —
see [`patches/README.md`](patches/README.md).

Where the time goes with real firmware, unpatched: `PCM_Update` 31.4%,
`TIMER_Clock` 26.0%, `SM_Update` 11.9%, `unscramble` 4.2% (startup),
`PCM_ReadROM` 3.7%, `calc_tv` 3.5%.


The core is an interpreter running a cycle-level model of two CPUs and a PCM
chip. It is one serial dependency chain, so it lives or dies on single-core
throughput and memory latency. In rough order of how much they buy you:

### Build flags

All measured, in retired instructions, on the 32-slot workload. The defaults
are the defaults because of this table, not by assumption.

| Configuration | vs default | Verdict |
|---|---|---|
| **gcc `-O3` + LTO** (default) | — | keep |
| LTO off | **+8.9%** | LTO earns its default |
| `-O2` + LTO | **+11.5%** | `-O3` earns its default |
| `-O2`, no LTO | +43.3% | — |
| clang `-O3` + LTO | **+21.6%** | stay on gcc |
| **+ PGO** | **−1.3%** | not worth enabling by default — see below |
| `-fno-plt`, `-fno-semantic-interposition`, `-fvisibility=hidden` | **0.0%** | no effect, skip them |
| `-funroll-loops` | −0.8% on x86 | **do not use on ARM** — see below |

`-DSC55D_CPU=cortex-a53` barely changes *which* instructions are generated
(228 vs 225 in `SM_Update` against generic aarch64). Its real value is
instruction *scheduling* for an in-order pipeline, which no tool available here
can score. Harmless, keep it, but do not expect a number.

**PGO is a disappointment, and earlier advice here was wrong.** This document
previously suggested PGO might be the biggest single win, on the reasoning that
upstream PR #51's main safe technique was hand-reordering branches by hit
frequency and PGO automates exactly that. Measured, the full cycle yields
**−1.3%**, nowhere near PR #51's reported ~35%. Worse, a binary trained on one
romset ran **0.14% slower** on another — the gain is profile-specific and
inverts under a workload change.

The training run here used zero-filled ROMs, where both CPUs execute mostly
invalid opcodes, so precisely the opcode-dispatch frequencies PR #51 targeted
are the ones the profile gets wrong. That neither confirms nor refutes the 35%
claim; it does establish that PGO is not a free substitute for it, and that
training on a fake workload is worse than not training at all. **If you want
PGO, train it on the Pi, on real ROMs, playing music you care about.**

`-funroll-loops` is the clearest case of a result that does not travel: −0.8%
on x86-64, but on aarch64 it adds 64 instructions to `SM_Update` and 4.5 KiB to
`.text`. An A53 has a 32 KiB L1I and no out-of-order engine to hide the extra
code.

The cheap flags measure as *exactly* zero — single-digit instructions out of
7.86 billion, not "too small to see". LTO has already merged the hot code into
one unit, and none of it crosses a shared-object boundary, so there is nothing
for them to act on.

### Pick a cheaper machine

Not every SC-55 costs the same to emulate, and the differences are large:

- **`--model scb55` (or `rlp3237`) has no sub-MCU — measured at −15.5% of all
  retired instructions, the single largest lever in this document.** On an mk2 the core emulates
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
# The kernel RT throttle. This one is not optional.
sudo sysctl -w kernel.sched_rt_runtime_us=-1
echo 'kernel.sched_rt_runtime_us=-1' | sudo tee /etc/sysctl.d/99-sc55d.conf

# CPU governor: ondemand ramps, and the ramp is an xrun
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# /boot/firmware/cmdline.txt — hand core 3 to sc55d alone, keep IRQs off it
isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2
# then run with --cpu 3 --output-cpu 2
```

- **Turn off the kernel RT throttle.** By default the kernel reserves 50 ms of
  every second for non-realtime work and simply stops any `SCHED_FIFO` thread
  that is still runnable when the budget is gone. sc55d's render thread
  saturates its core precisely when the board is struggling, so the throttle
  fires exactly when you can least afford it — and 50 ms is far longer than any
  sensible audio buffer, so every occurrence is an xrun. This is not
  speculation: a CPU-saturating `SCHED_FIFO` loop containing no emulator at
  all, measured here, stalls up to 54 ms roughly once a second; the same loop
  at normal priority never exceeds 1.6 ms. sc55d warns at startup if the
  throttle is on.

- **Give the renderer a core and the output thread a different one.** That is
  what `--render-ahead` exists to exploit; putting both on the isolated core
  gets you the serial behaviour back with extra latency. The output thread is
  cheap enough to share core 2 with the rest of the system.

- **Pick the rate converter.** 66207 Hz is not a rate any card supports, so the
  plug layer converts every sample — and it does that work *inside
  `snd_pcm_writei()`*, on the output thread, which is the other reason that
  thread wants its own core. Measured here, one 256-frame period against a
  3867 µs budget:

  | `defaults.pcm.rate_converter` | per period | of one core |
  |---|---|---|
  | `linear`          |   2.4 µs |  0.06% |
  | `lavrate`         |   5.9 µs |  0.15% |
  | `speexrate`       |  37.4 µs |  0.97% |
  | (unset)           |  38.0 µs |  0.98% |
  | `samplerate`      |  60.2 µs |  1.56% |
  | `speexrate_best`  | 255.9 µs |  6.62% |
  | `samplerate_best` | 469.9 µs | 12.15% |

  ```
  # /etc/asound.conf
  defaults.pcm.rate_converter "lavrate"
  ```

  `lavrate` is the one to use: 6x cheaper than the default and far better than
  `linear`, which is cheapest but audibly so — plain linear interpolation of a
  66 kHz source aliases. Avoid both `_best` variants; on a Pi 3 they would cost
  more than the whole rest of the output path. These are x86 numbers, so read
  the *ratios*, not the absolute microseconds.

- **Avoid the 3.5 mm jack.** It is PWM-driven, sounds poor, and adds work. An
  I²S DAC HAT or a USB DAC is better on both counts.
- **Trade latency for safety on a Pi 3.** `--period-frames 512 --periods 4` is
  about 31 ms and far harder to starve than the 11.6 ms default.
- **Watch thermals.** A Pi 3B+ throttles without a heatsink; check
  `vcgencmd get_throttled`.
- Bluetooth is already disabled if you followed the serial MIDI setup; also turn
  off Wi-Fi power saving if the box is headless and wired.

### Core patches

`patches/` holds thirteen performance patches applied at build time to a copy of
the core; the submodule itself is never modified. They are **on by default** and
**pass all 36 of upstream's own SC-55mk2 integration cases** — real MIDI files
with published expected SHA-256 hashes of the rendered audio, an absolute
reference rather than a comparison against ourselves. Each patch also has a
ROM-free equivalence test with deliberate mutants. `-DSC55D_PATCH_CORE=OFF`
disables them.

Romsets other than the mk2 family are **not** validated — no ROMs for them were
available. If you run one, especially the JV-880, run
`scripts/validate-patches.sh` first. `patches/README.md` has the details, plus
what was measured and rejected.

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

The thing render-ahead cannot do is make the core faster. It converts spare
cores into *tolerance of jitter*, nothing else: the render thread runs
continuously on one core while the output thread blocks in ALSA on another, and
the queued periods absorb a hiccup that would otherwise have been an xrun.

So there are two different failure modes, and sc55d now tells them apart. If
the shutdown line reports **starves**, the ring ran dry: the core is not
sustaining realtime on this board and no amount of buffering will fix it —
that is a `--bench` problem, and the answers are the patches, the build flags,
a cheaper romset, or a faster board. If there are **xruns but no starves**, the
audio path was late while the renderer was keeping up, and more buffer
(`--periods`, `--render-ahead`) or better isolation is the answer.

**A ring recovers more slowly the closer the board is to its limit**, which is
the part that does not show up on a fast machine. The ring refills at
`1/r − 1` periods per period, where `r` is how much of its budget a period
costs. On this x86 host at `r = 0.25` it refills at 0.75 periods per period; at
`r = 0.90` it refills at 0.10. The *same* stall therefore takes about seven
times longer to pay back on a board near 1.0x, and a second stall arriving
inside that window stacks on the first. So a deeper ring is worth less on a
struggling board than the arithmetic suggests, and headroom in the core is
worth more.

That is also why the RT throttle cannot be buffered away. A 50 ms hole at 256
frames needs 13 periods of ring to survive; the 262 ms stall recorded here
needs 68. No sane depth covers that — the `sysctl` does.

Beyond that: a single instance is strictly serial. The MCU, sub-MCU and PCM
chip are cycle-coupled per instruction, so the emulation itself cannot be split
across cores, and running several instances does not help a board that is short
— each one has to hit realtime independently, so N copies at 0.6x are still
0.6x. What is left is making the core cheaper, and `patches/README.md` records
what has been tried.

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
throttling flags, core count, `isolcpus`, the RTPRIO limit, whether an ALSA
device exists — and finishes with the benchmark. It changes nothing; it only
tells you what to change. Without `--roms` it does everything except the
benchmark.

`--full` adds two things: sc55d's own thread-safety tests, and — if ROMs and an
audio device are both present — 20 seconds of `--selftest` played to the default
device, so the run ends with the board actually making a noise. The tests take a
few minutes because of the ThreadSanitizer builds, but the Pi is the interesting
place to run them: they exist to catch memory-ordering bugs that x86 is too
strongly ordered to exhibit.

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

Short of a real Pi, the following were checked here, on x86-64 with a real
`mk2-v1.01` ROM set:

- **Emulation accuracy.** The patched core reproduces all 36 of upstream's own
  SC-55mk2 integration cases, byte for byte against their published SHA-256
  hashes. See `patches/README.md`.
- **Cross-architecture agreement.** Cross-compiled for `-mcpu=cortex-a53`
  (`cmake/aarch64-linux-gnu.cmake`) and run on aarch64, the benchmark produces
  a digest identical to the x86-64 build on the same workload — with the
  threading changes in place.
- **Thread safety.** The period ring and the MIDI queue each have a test that
  drives them from two threads and verifies every period and every byte
  arrives exactly once, in order, uncorrupted — run under ThreadSanitizer and
  under ASan/UBSan, each with a set of deliberate mutants that the test is
  required to kill. `tests/`. The whole daemon also runs clean under
  ThreadSanitizer against ALSA's `null` device.
- **Patch equivalence tests** pass compiled for A53 and run on aarch64.
- **Builds with GCC 12**, which is what Raspberry Pi OS Bookworm ships — the
  core requires C++23, so that was worth confirming.

What has *not* been verified here, and what a Pi is needed for:

- **Speed on real hardware.** Every performance figure in this README is
  x86-64. The ratios transfer; the absolute headroom does not. `--bench` on
  your Pi is the only number that settles whether this works.
- **The audio path to a real card** — there is no sound device on the machine
  this was developed on, so xruns, the plug layer against real hardware and
  `snd_pcm_writei()` pacing are all untested outside the `null` and `file`
  plugins.
- **The MIDI path from a real sequencer client.** There is no `/dev/snd/seq`
  here, so the ALSA sequencer decoding in `midi_in.cpp` is reviewed, not
  executed. Everything downstream of it *is* exercised: `--selftest` injects
  into the same queue from a non-render thread, and a 15-second run with 2570
  events produces real audio (peak 0.59 FS) and reports no ThreadSanitizer
  findings.
- **Romsets other than the mk2 family** — no ROMs were available for them.

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
always written as: single-threaded. `patches/README.md` has the measurement
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
[System tuning](#system-tuning) below. MIDI is idle until an event arrives and
only adds latency if made to wait. The renderer wants every cycle it can get, so
it goes last. `--priority` sets the renderer's; the other two sit one and two
steps above it.

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
tests/                  tests for sc55d's own code, each with its mutants
vendor/nuked-sc55/      the emulation core, unmodified (submodule)
```

## Licence

**Read [`NOTICE`](NOTICE) before redistributing anything.** It reproduces the
emulation core's licence in full, as that licence requires.

The short version: the core in `vendor/nuked-sc55` is under the original
(pre-2016) **MAME licence**, which is not an open-source licence. Redistribution
is allowed, but **not selling, and not use in a commercial product or
activity**; a modified build must ship complete source; and the notice must
travel with any redistribution. Those terms cover any binary built here, and
they cover the patches in `patches/`, which contain core source as diff context.
They are also GPL-incompatible, so this combination cannot be relicensed under
the GPL.

sc55d's own code — everything in `src/`, `scripts/`, `contrib/` and `cmake/` —
is original work. It contains no code from the core and reaches it only through
its public API. **No licence has been chosen for it yet**, which means default
copyright: fine for a personal build, but it needs deciding before publishing.
A permissive licence (MIT or BSD-2-Clause) is the sensible default — it creates
no conflict with the core's terms, and it keeps sc55d reusable should the
backend ever change (see [`docs/backend-options.md`](docs/backend-options.md),
where libEmuSC is LGPL and carries no such restriction).

No ROM data is included or downloaded. The SC-55 ROMs are Roland's copyright
and have to come from your own hardware.
