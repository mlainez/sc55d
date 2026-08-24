# Testing on a Raspberry Pi

Development happened on an x86-64 machine with no sound card, so the Pi is where
the interesting checks live. This page covers the one-command readiness check,
proving the core patches are safe on your ROMs, and an honest list of what has
and has not been verified.

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

## What has been verified off-board

Short of a real Pi, the following were checked here, on x86-64 with a real
`mk2-v1.01` ROM set:

- **Emulation accuracy.** The patched core reproduces all 36 of upstream's own
  SC-55mk2 integration cases, byte for byte against their published SHA-256
  hashes. See [patches/README.md](../patches/README.md).
- **Cross-architecture agreement.** Cross-compiled for `-mcpu=cortex-a53`
  (`cmake/aarch64-linux-gnu.cmake`) and run on aarch64, the benchmark produces
  a digest identical to the x86-64 build on the same workload — with the
  threading changes in place.
- **Thread safety.** The period ring and the MIDI queue each have a test that
  drives them from two threads and verifies every period and every byte
  arrives exactly once, in order, uncorrupted — run under ThreadSanitizer and
  under ASan/UBSan, and each with a set of deliberate mutants the test is
  required to kill (9 and 14 respectively, all killed):

  ```bash
  ./tests/ring/run.sh
  ./tests/midi_queue/run.sh
  ```

  Neither needs ROMs, ALSA or an audio device, and neither writes inside the
  repository. `pi-check.sh --full` runs both on the board, which is the
  interesting place for them. The whole daemon also runs clean under
  ThreadSanitizer, both idle against ALSA's `null` device and with 2570 MIDI
  events crossing the queue during `--selftest`.
- **Patch equivalence tests** pass compiled for A53 and run on aarch64.
- **Builds with GCC 12**, which is what Raspberry Pi OS Bookworm ships — the
  core requires C++23, so that was worth confirming.

What has *not* been verified here, and what a Pi is needed for:

- **Speed on real hardware.** Every performance figure in [`performance.md`](performance.md) is
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

