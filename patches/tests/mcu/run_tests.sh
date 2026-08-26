#!/bin/sh
# Builds and runs the three differential tests for the MCU patch series, then
# rebuilds each with a deliberate mistake and checks it is rejected.
#
#   run_tests.sh <patched-core-dir> <pristine-core-dir> <build-dir-with-config.h>
set -e
CORE=${1:?patched core dir}
PRISTINE=${2:?pristine core dir}
BUILD=${3:?build dir containing backend/config.h}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/_out}
mkdir -p "$OUT" "$OUT/upstream_tu"
CXX=${CXX:-g++}
FLAGS="-O2 -std=c++23 -DNDEBUG -I$HERE -I$CORE/src/backend -I$BUILD/backend"

fail=0
run() { # name expect-exit binary
    printf '%-46s ' "$1"
    if "$3" > "$OUT/$1.log" 2>&1; then got=0; else got=1; fi
    tail -1 "$OUT/$1.log" | tr -d '\n'
    if [ "$got" = "$2" ]; then echo "   [ok]"; else echo "   [UNEXPECTED exit $got]"; fail=1; fi
}

# --- 0001: code fetch -------------------------------------------------------
$CXX $FLAGS -o "$OUT/fetch" "$HERE/mcu_code_fetch_equivalence.cpp" "$CORE/src/backend/mcu.cpp"
run mcu_code_fetch_equivalence 0 "$OUT/fetch"
for b in 1 2 3; do
    $CXX $FLAGS -DBREAK_FAST_PATH=$b -o "$OUT/fetch.b$b" \
        "$HERE/mcu_code_fetch_equivalence.cpp" "$CORE/src/backend/mcu.cpp"
    run "mcu_code_fetch (broken $b, must fail)" 1 "$OUT/fetch.b$b"
done

# --- 0002: MCU_Step fixed work ---------------------------------------------
$CXX $FLAGS -o "$OUT/step" "$HERE/mcu_step_fixed_work_equivalence.cpp" "$CORE/src/backend/mcu.cpp"
run mcu_step_fixed_work_equivalence 0 "$OUT/step"
for b in 1 2; do
    $CXX $FLAGS -DBREAK_GATE=$b -o "$OUT/step.b$b" \
        "$HERE/mcu_step_fixed_work_equivalence.cpp" "$CORE/src/backend/mcu.cpp"
    run "mcu_step_fixed_work (broken $b, must fail)" 1 "$OUT/step.b$b"
done

# --- 0003: interrupt mask guard --------------------------------------------
# The upstream TU has to see the patched mcu.h, and a quoted include resolves
# from the source file's own directory first, so copy it somewhere neutral.
cp "$PRISTINE/src/backend/mcu_interrupt.cpp" "$OUT/upstream_tu/"
$CXX $FLAGS -c "$OUT/upstream_tu/mcu_interrupt.cpp" -o "$OUT/upstream_interrupt.o" \
    -DMCU_Interrupt_Handle=Upstream_Interrupt_Handle \
    -DMCU_Interrupt_Start=Upstream_Interrupt_Start \
    -DMCU_Interrupt_StartVector=Upstream_Interrupt_StartVector \
    -DMCU_Interrupt_SetRequest=Upstream_Interrupt_SetRequest \
    -DMCU_Interrupt_Exception=Upstream_Interrupt_Exception \
    -DMCU_Interrupt_TRAPA=Upstream_Interrupt_TRAPA
IRQ_SRC="$HERE/mcu_interrupt_mask_guard_equivalence.cpp $CORE/src/backend/mcu.cpp $CORE/src/backend/mcu_interrupt.cpp $OUT/upstream_interrupt.o"
$CXX $FLAGS -o "$OUT/irq" $IRQ_SRC
run mcu_interrupt_mask_guard_equivalence 0 "$OUT/irq"
for b in 1 2; do
    $CXX $FLAGS -DBREAK_GUARD=$b -o "$OUT/irq.b$b" $IRQ_SRC
    run "mcu_interrupt_mask_guard (broken $b, must fail)" 1 "$OUT/irq.b$b"
done


# --- 0014: sub-MCU interrupt scan gate -------------------------------------
# Upstream submcu.cpp is compiled with every global renamed Ref_*, so the
# patched and pristine handlers can be linked into one program and compared.
$CXX $FLAGS -c "$CORE/src/backend/submcu.cpp" -o "$OUT/submcu_probe.o"
SM_RENAMES=$(nm -C --defined-only "$OUT/submcu_probe.o" \
    | awk '$2=="T"||$2=="D"{sub(/\(.*/,"",$3); print $3}' | sort -u \
    | awk '{printf "-D%s=Ref_%s ", $1, $1}')
# From a neutral copy, so the TU sees the patched headers (see the 0016 section).
cp "$PRISTINE/src/backend/submcu.cpp" "$OUT/upstream_tu/submcu_scan.cpp"
# shellcheck disable=SC2086
$CXX $FLAGS $SM_RENAMES -c "$OUT/upstream_tu/submcu_scan.cpp" -o "$OUT/ref_submcu.o"
SM_SRC="$HERE/sm_interrupt_scan_equivalence.cpp $CORE/src/backend/submcu.cpp $OUT/ref_submcu.o"
$CXX $FLAGS -o "$OUT/smscan" $SM_SRC
run sm_interrupt_scan_equivalence 0 "$OUT/smscan"
for b in 1 2 3; do
    $CXX $FLAGS -DBREAK_SCAN=$b -o "$OUT/smscan.b$b" $SM_SRC
    run "sm_interrupt_scan (broken $b, must fail)" 1 "$OUT/smscan.b$b"
done

# --- 0015: word access fast path --------------------------------------------
$CXX $FLAGS -o "$OUT/word" "$HERE/mcu_word_access_equivalence.cpp" "$CORE/src/backend/mcu.cpp"
run mcu_word_access_equivalence 0 "$OUT/word"
for b in 1 2 3 4; do
    $CXX $FLAGS -DBREAK_WORD=$b -o "$OUT/word.b$b" \
        "$HERE/mcu_word_access_equivalence.cpp" "$CORE/src/backend/mcu.cpp"
    run "mcu_word_access (broken $b, must fail)" 1 "$OUT/word.b$b"
done

# --- 0016: sub-MCU idle-loop fast-forward ------------------------------------
# Both TUs are compiled from NEUTRAL copies: a quoted #include "mcu.h" resolves
# from the source file's own directory first, and upstream's mcu.h has a
# different mcu_t layout from the patched one (0006). Two layouts in one
# program is a silent, catastrophic mismatch -- it was, once.
cp "$PRISTINE/src/backend/submcu.cpp" "$OUT/upstream_tu/submcu_ff.cpp"
cp "$CORE/src/backend/submcu.cpp" "$OUT/upstream_tu/patched_submcu_ff.cpp"
# shellcheck disable=SC2086
$CXX $FLAGS $SM_RENAMES -c "$OUT/upstream_tu/submcu_ff.cpp" -o "$OUT/ref_submcu_ff.o"
FF_SRC="$HERE/sm_fastforward_equivalence.cpp $OUT/upstream_tu/patched_submcu_ff.cpp $OUT/ref_submcu_ff.o"
$CXX $FLAGS -o "$OUT/ff" $FF_SRC
# SM_ROM (path to the mk2 sub-MCU ROM image) adds the real firmware to the run; without it the synthetic programs still prove the property.
# FF_STEPS scales the run (synthetic programs; the firmware gets 4x): 1M keeps the
# whole harness under a few minutes and still crosses ~250k synchronisation points.
FF_STEPS=${FF_STEPS:-1000000}
run() { # name expect-exit binary [args]
    printf '%-46s ' "$1"
    if "$3" ${4:-} > "$OUT/$1.log" 2>&1; then got=0; else got=1; fi
    tail -1 "$OUT/$1.log" | tr -d '\n'
    if [ "$got" = "$2" ]; then echo "   [ok]"; else echo "   [UNEXPECTED exit $got]"; fail=1; fi
}
run sm_fastforward_equivalence 0 "$OUT/ff" "$FF_STEPS"
# Mutants are produced from the patched source: each removes or breaks one guard the proof rests on.
python3 - "$OUT/upstream_tu/patched_submcu_ff.cpp" "$OUT" <<'PYEOF'
import sys
src=open(sys.argv[1]).read(); out=sys.argv[2]
muts={1:("limit -= 1;","limit += 47;"),                                  # timer fire bound off by one: overshoots the fire
      2:("(now - sm.ff_c0 + 47) / 48","(now - sm.ff_c0) / 48"),            # rewind lands one instruction early
      3:("    sm.timer_cycles = sm.ff_timer_cycles0;\n    sm.timer_prescaler = sm.ff_timer_prescaler0;\n    sm.timer_counter = sm.ff_timer_counter0;\n    SM_UpdateTimer(sm);\n",""),  # rewind forgets the timer
      4:("        sm.ram[address] = data;\n        return;\n    }\n    sm.ff_impure = 1;","        sm.ram[address] = data;\n        return;\n    }\n"),  # shared-RAM writes counted as pure
      5:("                    if (mcu.uart_rx_delay <= sm.cycles)\n                        limit = sm.cycles;\n                    else if (mcu.uart_rx_delay - 1 < limit)\n                        limit = mcu.uart_rx_delay - 1;\n","")}  # pending UART byte ignored
for k,(a,b) in muts.items():
    assert src.count(a)==1, ("mutant anchor", k, src.count(a))
    open(f"{out}/upstream_tu/ff_mutant{k}.cpp","w").write(src.replace(a,b,1))
PYEOF
for b in 1 2 3 4 5; do
    $CXX $FLAGS -o "$OUT/ff.b$b" "$HERE/sm_fastforward_equivalence.cpp" "$OUT/upstream_tu/ff_mutant$b.cpp" "$OUT/ref_submcu_ff.o"
    run "sm_fastforward (broken $b, must fail)" 1 "$OUT/ff.b$b" "$FF_STEPS"
done

[ "$fail" = 0 ] && echo "all tests behaved as expected" || echo "SOMETHING BEHAVED UNEXPECTEDLY"
exit $fail
