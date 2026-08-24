#!/usr/bin/env bash
#
# Differential check for patches/0001-mcu_timer-closed-form-advancement.patch.
#
#   ./patches/tests/timer_closed_form/run.sh [runs]
#
# Builds the core's real mcu_timer.cpp twice in one program — upstream's in
# namespace `ref`, the patched one in namespace `neu` — against a stub mcu.h,
# and drives both through the same operations, comparing everything observable
# after each one: timer.cycles, the interrupt bitset, and every byte read back
# through the public register accessors.
#
# Field-by-field comparison would be wrong here: the patch deliberately leaves
# the counters stale between events and syncs them on demand, so what is
# claimed is observational equivalence, not identical internals.
#
# Then timer_mutants.sh breaks the patched version nine ways and checks the
# test notices each one. A test that cannot fail is not evidence.
#
# Needs no ROMs.
set -eu
cd "$(dirname "$0")"
REPO=$(cd ../../.. && pwd)
CORE="${NUKED_DIR:-$REPO/vendor/nuked-sc55}"

if [ ! -e "$CORE/src/backend/mcu_timer.cpp" ]; then
    echo "error: no core checkout at $CORE" >&2
    echo "       run: git submodule update --init --recursive" >&2
    exit 1
fi

rm -rf orig newf && mkdir -p orig newf

# Upstream, straight from the submodule.
cp "$CORE/src/backend/mcu_timer.h" "$CORE/src/backend/mcu_timer.cpp" orig/

# Patched: apply the patch to a scratch copy rather than to the submodule.
rm -rf .patched && mkdir -p .patched/src/backend
cp "$CORE/src/backend/mcu_timer.h" "$CORE/src/backend/mcu_timer.cpp" .patched/src/backend/
(cd .patched && patch -p1 --quiet -i "$REPO/patches/0001-mcu_timer-closed-form-advancement.patch")
cp .patched/src/backend/mcu_timer.h .patched/src/backend/mcu_timer.cpp newf/
rm -rf .patched

g++ -O2 -std=c++20 -Istub -o tt timer_closed_form_equivalence.cpp
./tt "${1:-480}"
./timer_mutants.sh
