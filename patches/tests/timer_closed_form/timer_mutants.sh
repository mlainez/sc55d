#!/bin/bash
# Deliberately break the closed-form implementation in one specific way each
# time and confirm the differential test notices. A test that cannot fail is
# not evidence.
set -u
cd "$(dirname "$0")"
mkdir -p mut
run_mutant() {
    name="$1"; shift
    rm -rf mut/newf && mkdir -p mut/newf
    cp newf/mcu_timer.h mut/newf/
    cp newf/mcu_timer.cpp mut/newf/
    "$@" || { echo "  [$name] mutation script failed"; return 1; }
    if ! cmp -s newf/mcu_timer.cpp mut/newf/mcu_timer.cpp; then :; else
        echo "  [$name] MUTATION DID NOT APPLY"; return 1; fi
    g++ -O2 -std=c++20 -Istub -Imut -DMUTANT -o mut/tt \
        <(sed 's#"newf/mcu_timer#"mut/newf/mcu_timer#' timer_closed_form_equivalence.cpp) \
        -x c++ /dev/null 2>/dev/null
    # the process-substitution trick loses the include dir, so use a real file
    sed 's#"newf/mcu_timer#"mut/newf/mcu_timer#' timer_closed_form_equivalence.cpp > mut/driver.cpp
    g++ -O2 -std=c++20 -Istub -I. -o mut/tt mut/driver.cpp 2>mut/build.log || {
        echo "  [$name] BUILD FAILED"; head -5 mut/build.log; return 1; }
    if ./mut/tt 24 > mut/out.log 2>&1; then
        echo "  [$name] NOT DETECTED  <-- test is too weak"
        return 1
    else
        echo "  [$name] detected: $(grep -m1 MISMATCH mut/out.log)"
        return 0
    fi
}

fail=0
echo "mutation testing the closed-form timer:"

run_mutant "overshoot the loop bound" \
  sed -i 's#const uint64_t limit = (cycles + 1) / 2;#const uint64_t limit = (cycles + 2) / 2;#' mut/newf/mcu_timer.cpp || fail=1

run_mutant "defer one tick too far" \
  sed -i 's#timer.next_event = next == ~(uint64_t)0 ? next : next + 1;#timer.next_event = next == ~(uint64_t)0 ? next : next + 2;#' mut/newf/mcu_timer.cpp || fail=1

run_mutant "ignore CCLRA when scheduling" \
  sed -i 's#^    if (!has_clear)$#    if (true)#' mut/newf/mcu_timer.cpp || fail=1

run_mutant "never re-raise an already-set flag" \
  sed -i 's#^        return (tcr \& enable) != 0 \&\& (tcsr \& flag) != 0 \&\&$#        return false \&\& (tcr \& enable) != 0 \&\& (tcsr \& flag) != 0 \&\&#' mut/newf/mcu_timer.cpp || fail=1

run_mutant "step the 8-bit timer even when its mask says never" \
  sed -i 's#^        if (tmr_mask != 0)$#        if (true)#' mut/newf/mcu_timer.cpp || fail=1

run_mutant "forget to invalidate the schedule on a register write" \
  perl -0pi -e 's/    \/\/ Registers changed under us.*?\n    timer\.next_event = 0;\n/    \/\/ MUTANT: schedule left stale\n/s' mut/newf/mcu_timer.cpp || fail=1

run_mutant "do not sync before a register read" \
  python3 mut_drop_read_sync.py mut/newf/mcu_timer.cpp || fail=1

run_mutant "miss compare-match A inside a long run" \
  perl -0pi -e 's/        const uint32_t da  = \(ocra - frc\) & 0xffffu;/        const uint32_t da  = 0xffffu;/' mut/newf/mcu_timer.cpp || fail=1

run_mutant "drop the overflow event" \
  sed -i 's#^    return maxv - cur;$#    return TIMER_NEVER;#' mut/newf/mcu_timer.cpp || fail=1

if [ $fail -ne 0 ]; then echo "SOME MUTANTS SURVIVED"; exit 1; fi
echo "all mutants detected"
