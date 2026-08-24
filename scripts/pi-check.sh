#!/usr/bin/env bash
#
# SPDX-License-Identifier: MIT
#
# One-shot readiness check for running sc55d on a Raspberry Pi.
#
#   ./scripts/pi-check.sh [--roms <dir>] [--full]
#
# Reports the board and toolchain, picks the right -mcpu, builds, runs the
# patch equivalence tests, inspects the system tuning that decides whether
# audio will glitch, and finally runs the benchmark if ROMs are available.
# --full also runs sc55d's own thread-safety tests, which are slow but which
# this board is the right place to run.
# Nothing here changes the system; it only tells you what to change.

set -u

ROMS=""
FULL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --roms) ROMS="${2:-}"; shift 2 ;;
        --full) FULL=1; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "$(dirname "$0")/.." || exit 1
REPO="$PWD"
BUILD="$REPO/build"

warnings=0
blockers=0

say()   { printf '%s\n' "$*"; }
head2() { printf '\n== %s\n' "$*"; }
ok()    { printf '  ok      %s\n' "$*"; }
warn()  { printf '  WARN    %s\n' "$*"; warnings=$((warnings + 1)); }
bad()   { printf '  BLOCKED %s\n' "$*"; blockers=$((blockers + 1)); }
note()  { printf '          %s\n' "$*"; }

# ---------------------------------------------------------------- board ----
head2 "Board and OS"

model="unknown"
[ -r /proc/device-tree/model ] && model=$(tr -d '\0' < /proc/device-tree/model)
say "  model   $model"
say "  kernel  $(uname -r)"
say "  arch    $(uname -m)"

case "$(uname -m)" in
    aarch64)
        ok "64-bit userland"
        ;;
    armv7l|armv6l)
        warn "32-bit userland on a 64-bit capable board"
        note "The core counts cycles in uint64_t throughout, which costs real"
        note "time on 32-bit ARM. Use the 64-bit Raspberry Pi OS image."
        ;;
    *)
        note "not an ARM board; this script is meant to run on the Pi itself"
        ;;
esac

# Pick -mcpu from the part number rather than guessing from the model string.
part=$(awk '/CPU part/ {print $4; exit}' /proc/cpuinfo 2>/dev/null)
case "$part" in
    0xd03) CPU=cortex-a53 ;;   # Pi 3
    0xd08) CPU=cortex-a72 ;;   # Pi 4
    0xd0b) CPU=cortex-a76 ;;   # Pi 5
    *)     CPU="" ;;
esac
if [ -n "$CPU" ]; then
    ok "building for -mcpu=$CPU"
else
    warn "unrecognised CPU part '$part'; building without -mcpu"
    note "Pass -DSC55D_CPU=<cpu> yourself if you know the right one."
fi

# ------------------------------------------------------------ toolchain ----
head2 "Toolchain"

for tool in cmake g++ patch; do
    if command -v "$tool" > /dev/null 2>&1; then
        ok "$tool $("$tool" --version 2>/dev/null | head -1 | awk '{print $NF}')"
    else
        bad "$tool not installed"
    fi
done

if [ -e /usr/include/alsa/asoundlib.h ]; then
    ok "libasound2-dev headers present"
else
    bad "libasound2-dev not installed"
fi

[ $blockers -gt 0 ] && {
    say ""
    say "Install what is missing and re-run:"
    say "  sudo apt install build-essential cmake git libasound2-dev"
    exit 1
}

if [ ! -e vendor/nuked-sc55/src/backend/emu.cpp ]; then
    bad "the vendor/nuked-sc55 submodule is empty"
    note "git submodule update --init --recursive"
    exit 1
fi
ok "emulation core checked out"

# ---------------------------------------------------------------- build ----
head2 "Build"

cmake_args="-DCMAKE_BUILD_TYPE=Release"
[ -n "$CPU" ] && cmake_args="$cmake_args -DSC55D_CPU=$CPU"

if cmake -S . -B "$BUILD" $cmake_args > "$BUILD.log" 2>&1 &&
   cmake --build "$BUILD" -j"$(nproc)" >> "$BUILD.log" 2>&1; then
    ok "built $BUILD/sc55d"
else
    bad "build failed; see $BUILD.log"
    tail -20 "$BUILD.log"
    exit 1
fi

# ---------------------------------------------------------------- tests ----
head2 "Patch equivalence tests"

for t in patches/tests/*.cpp; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .cpp)
    if g++ -O2 -std=c++23 -o "/tmp/$name" "$t" > /dev/null 2>&1 && "/tmp/$name" > "/tmp/$name.log" 2>&1; then
        ok "$name"
    else
        warn "$name FAILED — do not enable -DSC55D_PATCH_CORE=ON"
        tail -5 "/tmp/$name.log"
    fi
done

for t in patches/tests/*/run.sh; do
    [ -e "$t" ] || continue
    name=$(basename "$(dirname "$t")")
    if "$t" > "/tmp/$name.log" 2>&1; then
        ok "$name"
    else
        warn "$name FAILED — do not enable -DSC55D_PATCH_CORE=ON"
        tail -8 "/tmp/$name.log"
    fi
done
note "These prove the patch transformations, not the emulation."
note "Use scripts/validate-patches.sh with real ROMs before trusting them."

# --------------------------------------------------- sc55d's own threads ----
head2 "Thread-safety tests"

if [ "$FULL" -eq 1 ]; then
    for t in tests/*/run.sh; do
        [ -e "$t" ] || continue
        name=$(basename "$(dirname "$t")")
        if "$t" > "/tmp/sc55d-$name.log" 2>&1; then
            ok "$name"
        else
            warn "$name FAILED"
            tail -12 "/tmp/sc55d-$name.log"
        fi
    done
else
    note "Skipped; re-run with --full to include them. They take a few minutes"
    note "(ThreadSanitizer builds), but this board is the interesting place to"
    note "run them: the ordering bugs they look for cannot happen on x86."
fi

# --------------------------------------------------------------- tuning ----
head2 "Realtime tuning"

gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
case "$gov" in
    performance) ok "cpufreq governor is performance" ;;
    "")          warn "cannot read the cpufreq governor" ;;
    *)           warn "cpufreq governor is '$gov'; ramping causes xruns"
                 note "echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor" ;;
esac

if command -v vcgencmd > /dev/null 2>&1; then
    thr=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)
    if [ "$thr" = "0x0" ]; then
        ok "no throttling recorded"
    else
        warn "throttling flags $thr — check power supply and cooling"
        note "A Pi 3B+ without a heatsink will throttle under this load."
    fi
fi

cores=$(nproc 2>/dev/null || echo 1)
if [ "$cores" -ge 2 ]; then
    ok "$cores cores: --render-ahead can put the renderer and the audio write"
    note "on different cores, which is what absorbs scheduling jitter."
else
    warn "single core: --render-ahead cannot help here"
    note "Run with --render-ahead 0 so the renderer is not paying for a ring"
    note "and two threads it has no second core to use."
fi

cmdline=$(cat /proc/cmdline 2>/dev/null)
if printf '%s' "$cmdline" | grep -q isolcpus; then
    ok "isolcpus set: $(printf '%s' "$cmdline" | tr ' ' '\n' | grep isolcpus)"
    note "Remember to run sc55d with --cpu <that core>, and --output-cpu on a"
    note "different one -- both threads on the isolated core gets you the"
    note "serial behaviour back with extra latency."
else
    warn "no isolated CPU"
    note "In /boot/firmware/cmdline.txt add: isolcpus=3 nohz_full=3 irqaffinity=0-2"
    note "then run sc55d with --cpu 3 --output-cpu 2."
fi

# The single most likely reason a correctly-configured Pi still glitches.
rt_runtime=$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null)
rt_period=$(cat /proc/sys/kernel/sched_rt_period_us 2>/dev/null || echo 1000000)
if [ "${rt_runtime:--1}" = "-1" ]; then
    ok "kernel RT throttle disabled"
elif [ -n "$rt_runtime" ] && [ "$rt_runtime" -lt "$rt_period" ] 2>/dev/null; then
    bad "kernel RT throttle is on: SCHED_FIFO threads are stopped for"
    note "$(( (rt_period - rt_runtime) / 1000 )) ms out of every $(( rt_period / 1000 )) ms once they saturate a core."
    note "That is far longer than any audio buffer, so it is an xrun every"
    note "second, and it bites hardest exactly when the board is struggling."
    note "  sudo sysctl -w kernel.sched_rt_runtime_us=-1"
    note "  echo 'kernel.sched_rt_runtime_us=-1' | sudo tee /etc/sysctl.d/99-sc55d.conf"
    note "Or run with --no-realtime, which avoids SCHED_FIFO entirely."
fi

rtprio=$(ulimit -Hr 2>/dev/null)
if [ "${rtprio:-0}" = "unlimited" ] || [ "${rtprio:-0}" -gt 0 ] 2>/dev/null; then
    ok "RTPRIO limit is $rtprio (SCHED_FIFO available)"
else
    warn "RTPRIO limit is 0; sc55d cannot get SCHED_FIFO as this user"
    note "Run as root, or use the systemd unit in contrib/, or add a limits.d rule."
fi

if [ "$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)" -gt 0 ]; then
    note "swap is enabled; sc55d calls mlockall() so this should not bite"
fi

cards=$(aplay -l 2>/dev/null | grep -c '^card' || true)
if [ "${cards:-0}" -gt 0 ]; then
    ok "$cards ALSA playback device(s); 'aplay -L' lists names for --audio-device"
else
    warn "no ALSA playback device found"
fi

# ------------------------------------------------------------ benchmark ----
head2 "Benchmark"

if [ -z "$ROMS" ]; then
    for guess in "$REPO/roms" /usr/share/sc55d/roms; do
        [ -d "$guess" ] && { ROMS="$guess"; break; }
    done
fi

if [ -z "$ROMS" ] || [ ! -d "$ROMS" ]; then
    warn "no ROM directory found; skipping the benchmark"
    note "Re-run with --roms <dir> once your ROMs are in place. That benchmark"
    note "is the go/no-go number for this board."
else
    say "  using ROMs from $ROMS"
    say ""
    cpu_arg=""
    printf '%s' "$cmdline" | grep -q 'isolcpus=3' && cpu_arg="--cpu 3"
    "$BUILD/sc55d" --roms "$ROMS" --bench $cpu_arg 2>&1 | sed 's/^/  /'
    bench_status=${PIPESTATUS[0]}
    say ""
    if [ "$bench_status" -eq 0 ]; then
        ok "holds realtime"
    else
        warn "does NOT hold realtime on this board as configured"
        note "Try: --model scb55 (no sub-MCU, ~12% cheaper), larger"
        note "--period-frames/--periods, the tuning above, and PGO"
        note "(see docs/performance.md)."
    fi
fi

# --------------------------------------------------------------- sound ----
if [ "$FULL" -eq 1 ] && [ -n "$ROMS" ] && [ -d "$ROMS" ] && [ "${cards:-0}" -gt 0 ]; then
    head2 "Selftest"
    say "  Playing 20 s of the stress sequence to the default device."
    say "  If you hear nothing, the problem is the audio path, not the emulator."
    say ""
    "$BUILD/sc55d" --roms "$ROMS" --gs --selftest 20 2>&1 | sed 's/^/  /'
fi

# -------------------------------------------------------------- summary ----
head2 "Summary"
say "  $warnings warning(s), $blockers blocker(s)"
[ $warnings -gt 0 ] && say "  Address the warnings above before judging the benchmark."
say ""
exit 0
