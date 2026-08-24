/*
 * Differential check for
 * patches/0002-mcu_step-hoist-fixed-work.patch.
 *
 * The patch makes two claims about work MCU_Step() does on every emulated
 * instruction, and this proves both against the real code in mcu.cpp.
 *
 * 1. has_submcu, computed once by MCU_SetRomset(), equals the predicate it
 *    replaces: !is_mk1 && !is_jv880 && !is_scb55. Checked for every value of
 *    the Romset enumeration, including any added later -- the loop walks ROMSET_COUNT rather
 *    range rather than a hand-written list.
 *
 * 2. Guarding the call as
 *
 *        if (dev_register[DEV_ADCSR] & 0x20) MCU_UpdateAnalog(mcu, cycles);
 *        else                                analog_end_time = 0;
 *
 *    is indistinguishable from calling MCU_UpdateAnalog(mcu, cycles)
 *    unconditionally. Checked over all 256 ADCSR values crossed with the
 *    states that steer the function: analog_end_time before, on and after the
 *    conversion deadline, several cycle counts, and both settings of the
 *    interrupt-enable and scan-mode bits (which live in ADCSR, so the 256
 *    covers them). Every scalar in mcu_t is compared afterwards, along with
 *    the log of outward calls, so a difference in the analog sample written,
 *    the ADCSR flags left behind or the interrupt raised would all show.
 *
 *   g++ -O2 -std=c++23 -I<core>/src/backend -I<build>/backend \
 *       -o mcu_step_fixed_work_equivalence mcu_step_fixed_work_equivalence.cpp \
 *       <core>/src/backend/mcu.cpp
 *   ./mcu_step_fixed_work_equivalence
 *
 * Build with -DBREAK_GATE=1 or 2 to check the test can fail.
 */
#include "mcu_core_stubs.h"

#include <cstdio>

/* mcu.cpp defines this but no header declares it; MCU_Step() is its only
 * caller upstream. */
void MCU_UpdateAnalog(mcu_t& mcu, uint64_t cycles);

namespace {

/* The guard as the patch writes it, restated so it can be broken on purpose. */
void GatedUpdate(mcu_t& mcu, uint64_t cycles)
{
#if BREAK_GATE == 1
    /* gates on the wrong bit: 0x10 is scan mode, 0x20 is start */
    if (mcu.dev_register[DEV_ADCSR] & 0x10)
        MCU_UpdateAnalog(mcu, cycles);
    else
        mcu.analog_end_time = 0;
#elif BREAK_GATE == 2
    /* forgets that the untaken branch still has to clear analog_end_time */
    if (mcu.dev_register[DEV_ADCSR] & 0x20)
        MCU_UpdateAnalog(mcu, cycles);
#else
    if (mcu.dev_register[DEV_ADCSR] & 0x20)
        MCU_UpdateAnalog(mcu, cycles);
    else
        mcu.analog_end_time = 0;
#endif
}

mcu_t* g_mcu;

} // namespace

int main()
{
    g_mcu = new mcu_t();
    mcu_t& mcu = *g_mcu;

    static submcu_t    sm{};
    static pcm_t*      pcm = new pcm_t();
    static mcu_timer_t timer{};
    static lcd_t*      lcd = (lcd_t*)calloc(1, 1 << 20);
    mcu.sm = &sm;
    mcu.pcm = pcm;
    mcu.timer = &timer;
    mcu.lcd = lcd;

    int failures = 0;

    /* --- claim 1: has_submcu is the predicate it replaced ----------------- */
    int romsets = 0;
    for (int r = 0; r < (int)ROMSET_COUNT; r++)
    {
        MCU_SetRomset(mcu, (Romset)r);
        const bool want = !mcu.is_mk1 && !mcu.is_jv880 && !mcu.is_scb55;
        if (mcu.has_submcu != want)
        {
            printf("MISMATCH romset=%d has_submcu=%d predicate=%d\n",
                   r, (int)mcu.has_submcu, (int)want);
            failures++;
        }
        romsets++;
    }
    printf("has_submcu matches the predicate for all %d romsets\n", romsets);

    /* --- claim 2: the ADCSR gate is invisible ----------------------------- */
    const uint64_t kCycles[]  = { 0, 1, 199, 200, 201, 1000, 1u << 20 };
    const uint64_t kDeadlines[] = { 0, 1, 199, 200, 201, 999, 1000, 1001, 1u << 20, ~0ull };
    const uint8_t  kSwPos[]   = { 0, 1, 2, 3 };

    Snapshot before, after_gated, after_plain;
    uint64_t checked = 0;

    for (int r = 0; r < (int)ROMSET_COUNT; r++)
    {
        MCU_SetRomset(mcu, (Romset)r);
        for (uint8_t sw : kSwPos)
        {
            mcu.sw_pos = sw;
            for (uint64_t cycles : kCycles)
            {
                for (uint64_t deadline : kDeadlines)
                {
                    for (uint32_t adcsr = 0; adcsr < 256; adcsr++)
                    {
                        mcu.dev_register[DEV_ADCSR] = (uint8_t)adcsr;
                        mcu.analog_end_time = deadline;
                        mcu.ad_val[0] = 0x1111; mcu.ad_val[1] = 0x2222;
                        mcu.ad_val[2] = 0x3333; mcu.ad_val[3] = 0x4444;
                        mcu.ad_nibble = 0;
                        mcu.interrupt_pending = {};

                        before.Take(mcu);

                        StubLog().clear();
                        GatedUpdate(mcu, cycles);
                        const std::vector<StubCall> log_gated = StubLog();
                        after_gated.Take(mcu);

                        before.Restore(mcu);
                        StubLog().clear();
                        MCU_UpdateAnalog(mcu, cycles);
                        const std::vector<StubCall> log_plain = StubLog();
                        after_plain.Take(mcu);

                        if (!(after_gated == after_plain) || log_gated != log_plain)
                        {
                            if (failures < 10)
                            {
                                printf("MISMATCH romset=%d adcsr=%02x cycles=%llu "
                                       "deadline=%llu: state %s, calls %zu/%zu\n",
                                       r, adcsr, (unsigned long long)cycles,
                                       (unsigned long long)deadline,
                                       (after_gated == after_plain) ? "same" : "DIFFER",
                                       log_gated.size(), log_plain.size());
                            }
                            failures++;
                        }

                        before.Restore(mcu);
                        checked++;
                    }
                }
            }
        }
    }

    printf("checked %llu analog-gate states\n", (unsigned long long)checked);

    if (failures)
    {
        printf("FAIL: %d mismatches\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
