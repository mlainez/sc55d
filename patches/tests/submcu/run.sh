#!/usr/bin/env bash
# Proofs for patches/0004-submcu-fast-forward-idle-loop.patch.
#
#   ./patches/tests/submcu/run.sh <patched-core-dir> <pristine-core-dir> <build-dir-with-config.h>
#
# Two programs, both against upstream's real submcu.cpp compiled with every
# global renamed Ref_* (the list comes from nm, so it cannot go stale):
#
#   sm_fastforward_equivalence   the patched sub-MCU and upstream's, driven by the
#                                same simulated main MCU over synthetic programs
#                                and -- if SM_ROM names the mk2 sub-MCU ROM -- the
#                                real firmware; every observable and the full
#                                state at every synchronisation point compared.
#   sm_timer_closed_form         the constant-time timer advance against upstream's
#                                tick loop, 27M states.
#
# Then each is rebuilt with deliberate mistakes that must be detected. Both TUs
# are compiled from NEUTRAL copies: a quoted #include "mcu.h" resolves from the
# source file's own directory first, and upstream's mcu.h has a different mcu_t
# layout from the patched one -- two layouts in one program once produced a very
# convincing false failure. FF_STEPS scales the fast-forward run (default 1M;
# the firmware gets 4x).
set -e
CORE=${1:?patched core dir}
PRISTINE=${2:?pristine core dir}
BUILD=${3:?build dir containing backend/config.h}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/_out}
FF_STEPS=${FF_STEPS:-1000000}
mkdir -p "$OUT/tu"
CXX=${CXX:-g++}
FLAGS="-O2 -std=c++23 -DNDEBUG -I$CORE/src/backend -I$BUILD/backend"

fail=0
run() { # name expect-exit binary [args]
    printf '%-46s ' "$1"
    if "$3" ${4:-} > "$OUT/$1.log" 2>&1; then got=0; else got=1; fi
    tail -1 "$OUT/$1.log" | tr -d '\n'
    if [ "$got" = "$2" ]; then echo "   [ok]"; else echo "   [UNEXPECTED exit $got]"; fail=1; fi
}

# The test constructs an mcu_t; on the decoder2 sources that carries the
# instruction cache, whose constructor lives in its own translation unit.
DECODER_TU=""
if [ -e "$CORE/src/backend/decoder2/cache.cpp" ]; then
    $CXX $FLAGS -c "$CORE/src/backend/decoder2/cache.cpp" -o "$OUT/cache.o"
    DECODER_TU="$OUT/cache.o"
fi
cp "$CORE/src/backend/submcu.cpp" "$OUT/tu/patched_submcu.cpp"
cp "$PRISTINE/src/backend/submcu.cpp" "$OUT/tu/upstream_submcu.cpp"
$CXX $FLAGS -c "$OUT/tu/patched_submcu.cpp" -o "$OUT/probe.o"
RENAMES=$(nm -C --defined-only "$OUT/probe.o" | awk '$2=="T"||$2=="D"{sub(/\(.*/,"",$3); print $3}' | sort -u | awk '{printf "-D%s=Ref_%s ", $1, $1}')
# shellcheck disable=SC2086
$CXX $FLAGS $RENAMES -c "$OUT/tu/upstream_submcu.cpp" -o "$OUT/ref_submcu.o"

$CXX $FLAGS -o "$OUT/ff" "$HERE/sm_fastforward_equivalence.cpp" "$OUT/tu/patched_submcu.cpp" "$OUT/ref_submcu.o" $DECODER_TU
run sm_fastforward_equivalence 0 "$OUT/ff" "$FF_STEPS"
python3 - "$OUT/tu/patched_submcu.cpp" "$OUT/tu" <<'PYEOF'
import sys
src=open(sys.argv[1]).read(); out=sys.argv[2]
muts={1:("limit -= 1;","limit += 47;"),                                          # timer-fire bound off by one: overshoots the fire
      2:("(now - sm.ff_c0 + 47) / 48","(now - sm.ff_c0) / 48"),                    # rewind lands one instruction early
      3:("    sm.timer_cycles = sm.ff_timer_cycles0;\n    sm.timer_prescaler = sm.ff_timer_prescaler0;\n    sm.timer_counter = sm.ff_timer_counter0;\n    SM_UpdateTimer(sm);\n",""),  # rewind forgets the timer
      4:("        sm.ram[address] = data;\n        return;\n    }\n    sm.ff_impure = 1;","        sm.ram[address] = data;\n        return;\n    }\n"),  # shared-RAM writes counted as pure
      5:("                    if (mcu.uart_rx_delay <= sm.cycles)\n                        limit = sm.cycles;\n                    else if (mcu.uart_rx_delay - 1 < limit)\n                        limit = mcu.uart_rx_delay - 1;\n","")}  # pending UART byte ignored
for k,(a,b) in muts.items():
    assert src.count(a)==1, ("mutant anchor", k, src.count(a))
    open(f"{out}/ff_mutant{k}.cpp","w").write(src.replace(a,b,1))
PYEOF
for b in 1 2 3 4 5; do
    $CXX $FLAGS -o "$OUT/ff.b$b" "$HERE/sm_fastforward_equivalence.cpp" "$OUT/tu/ff_mutant$b.cpp" "$OUT/ref_submcu.o" $DECODER_TU
    run "sm_fastforward (broken $b, must fail)" 1 "$OUT/ff.b$b" "$FF_STEPS"
done

$CXX -O2 -std=c++23 -o "$OUT/timer" "$HERE/sm_timer_closed_form_equivalence.cpp"
run sm_timer_closed_form_equivalence 0 "$OUT/timer"
for b in 1 2 3; do
    $CXX -O2 -std=c++23 -DBREAK_TIMER=$b -o "$OUT/timer.b$b" "$HERE/sm_timer_closed_form_equivalence.cpp"
    run "sm_timer_closed_form (broken $b, must fail)" 1 "$OUT/timer.b$b"
done

[ "$fail" = 0 ] && echo "all tests behaved as expected" || echo "SOMETHING BEHAVED UNEXPECTEDLY"
exit $fail
