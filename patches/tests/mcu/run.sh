#!/usr/bin/env bash
#
# Differential checks for the MCU patches (0003-0007).
#
#   ./patches/tests/mcu/run.sh
#
# Builds each test against both the pristine core and a patched copy, runs it,
# then rebuilds it with a deliberate mistake and checks it fails. A test that
# cannot fail is not evidence.
#
# Covers:
#   mcu_code_fetch_equivalence      the ROM fast path against the real MCU_Read,
#                                   over 188,743,680 (cp, pc) x romset x
#                                   rom2_mask combinations, comparing the return
#                                   value, the log of outward calls and every
#                                   scalar in mcu_t -- so short-circuiting an
#                                   address with a side effect would be caught
#   mcu_step_fixed_work_equivalence has_submcu for all 9 romsets, and the ADCSR
#                                   gate against the unguarded MCU_UpdateAnalog
#   mcu_interrupt_mask_guard_...    mcu_interrupt.cpp compiled twice, upstream's
#                                   names redefined, both handlers run on
#                                   identical machines over 7,700,480 states
#
# Needs no ROMs.
set -eu
cd "$(dirname "$0")"
REPO=$(cd ../../.. && pwd)
CORE="${NUKED_DIR:-$REPO/vendor/nuked-sc55}"

if [ ! -e "$CORE/src/backend/mcu.cpp" ]; then
    echo "error: no core checkout at $CORE" >&2
    echo "       run: git submodule update --init --recursive" >&2
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp -a "$CORE/src" "$WORK/pristine-src"
mkdir -p "$WORK/pristine" && mv "$WORK/pristine-src" "$WORK/pristine/src"

cp -a "$CORE/src" "$WORK/patched-src"
mkdir -p "$WORK/patched" && mv "$WORK/patched-src" "$WORK/patched/src"
for p in "$REPO"/patches/000[3-7]-*.patch; do
    (cd "$WORK/patched" && patch -p1 --quiet -i "$p")
done

./run_tests.sh "$WORK/patched" "$WORK/pristine" "$WORK/build"
