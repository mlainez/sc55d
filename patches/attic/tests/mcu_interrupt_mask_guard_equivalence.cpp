/*
 * Differential check for
 * patches/0003-mcu_interrupt-skip-fully-masked-scan.patch.
 *
 * The patch adds one early return to MCU_Interrupt_Handle(): when the CPU's
 * interrupt mask is 7, skip the scan over pending sources. The argument is that
 * every level the scan can compute is `(some IPR byte >> 0 or 4) & 7`, so it is
 * at most 7, and the scan dispatches only when `mask < level`.
 *
 * Rather than model that, this compiles mcu_interrupt.cpp twice -- once as
 * shipped and once with its externally visible names redefined by -D, so the
 * upstream version can sit in the same binary as the patched one -- and runs
 * both on identical machines. It compares the CPU registers, the status
 * register, the pending sets, the interrupt stack frame pushed into internal
 * RAM, and the log of outward calls.
 *
 * If skipping the scan ever lost a dispatch, the two would diverge: the
 * upstream handler would have pushed pc/cp/sr and jumped to a vector while the
 * patched one did nothing.
 *
 * The sweep runs every mask 0..7 -- not just 7, so a guard placed at the wrong
 * threshold is caught too -- crossed with every value 0..255 of the four
 * priority registers (in four rotations, so a level read out of the wrong
 * register shows up), every relevant P1CR gating combination, and pending sets
 * covering each source alone, all sources at once, and a spread of mixtures.
 *
 * Building it takes one wrinkle. The upstream copy of mcu_interrupt.cpp has to
 * see the *patched* mcu.h, or the two halves of the binary disagree about
 * mcu_t's layout -- and GCC resolves a quoted include from the directory of the
 * including file first, so compiling it in place would silently pick up the
 * pristine header next to it. Copy it somewhere neutral first. The runner in
 * this directory does that; by hand it is:
 *
 *   cp <pristine>/src/backend/mcu_interrupt.cpp /tmp/upstream_tu/
 *   g++ -O2 -std=c++23 -DNDEBUG -I<core>/src/backend -I<build>/backend \
 *       -c /tmp/upstream_tu/mcu_interrupt.cpp -o upstream_interrupt.o \
 *       -DMCU_Interrupt_Handle=Upstream_Interrupt_Handle \
 *       -DMCU_Interrupt_Start=Upstream_Interrupt_Start \
 *       -DMCU_Interrupt_StartVector=Upstream_Interrupt_StartVector \
 *       -DMCU_Interrupt_SetRequest=Upstream_Interrupt_SetRequest \
 *       -DMCU_Interrupt_Exception=Upstream_Interrupt_Exception \
 *       -DMCU_Interrupt_TRAPA=Upstream_Interrupt_TRAPA
 *
 *   g++ -O2 -std=c++23 -DNDEBUG -I. -I<core>/src/backend -I<build>/backend \
 *       -o mcu_interrupt_mask_guard_equivalence \
 *       mcu_interrupt_mask_guard_equivalence.cpp \
 *       <core>/src/backend/mcu.cpp <core>/src/backend/mcu_interrupt.cpp \
 *       upstream_interrupt.o
 *
 * Build with -DBREAK_GUARD=1 or 2 to check the test can fail.
 */
#define STUBS_REAL_INTERRUPT 1
#include "mcu_core_stubs.h"

#include <cstdio>

void Upstream_Interrupt_Handle(mcu_t& mcu);

namespace {

/* The patched handler, reached through a wrapper so a broken guard can be
 * substituted without touching the shipped file. */
void Handler_Under_Test(mcu_t& mcu)
{
#if BREAK_GUARD == 1
    /* guard at the wrong threshold: a mask of 6 can still be outranked by 7 */
    if (((mcu.sr >> 8) & 7) >= 6)
        return;
    MCU_Interrupt_Handle(mcu);
#elif BREAK_GUARD == 2
    /* guard placed before the exception and trapa dispatch, not after */
    if (((mcu.sr >> 8) & 7) >= 7 && mcu.exception_pending >= 0)
        return;
    MCU_Interrupt_Handle(mcu);
#else
    MCU_Interrupt_Handle(mcu);
#endif
}

uint32_t Rand(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

mcu_t* g_a;
mcu_t* g_b;

} // namespace

int main()
{
    g_a = new mcu_t();
    g_b = new mcu_t();

    static submcu_t    sm{};
    static pcm_t*      pcm = new pcm_t();
    static mcu_timer_t timer{};
    static lcd_t*      lcd = (lcd_t*)calloc(1, 1 << 20);

    uint32_t seed = 0x1337c0de;
    static uint8_t rom1_pattern[ROM1_SIZE];
    for (size_t i = 0; i < sizeof(rom1_pattern); i++)
        rom1_pattern[i] = (uint8_t)Rand(seed);

    /* Pending sets: each source alone, all of them, none, and a spread of
     * mixtures so more than one candidate is in the scan at a time. */
    std::vector<uint32_t> pending_sets;
    pending_sets.push_back(0);
    for (int i = 0; i < INTERRUPT_SOURCE_MAX; i++)
        pending_sets.push_back(1u << i);
    pending_sets.push_back((1u << INTERRUPT_SOURCE_MAX) - 1);
    for (int i = 0; i < 24; i++)
        pending_sets.push_back(Rand(seed) & ((1u << INTERRUPT_SOURCE_MAX) - 1));

    const uint8_t kP1CR[] = { 0x00, 0x20, 0x40, 0x60, 0xff };
    const int kRotations = 4;

    int failures = 0;
    uint64_t checked = 0;

    for (uint32_t mask = 0; mask < 8; mask++)
    {
        for (uint8_t p1cr : kP1CR)
        {
            for (uint32_t v = 0; v < 256; v++)
            {
                for (int rot = 0; rot < kRotations; rot++)
                {
                    const uint8_t ipr[4] = {
                        (uint8_t)(v + rot * 0x11), (uint8_t)(v ^ 0x5a),
                        (uint8_t)(~v), (uint8_t)(v + 0x37),
                    };
                    for (uint32_t pend : pending_sets)
                    {
                        for (int exc = -1; exc <= 2; exc++)
                        {
                            mcu_t& a = *g_a;
                            mcu_t& b = *g_b;

                            /* one machine, built twice */
                            for (mcu_t* m : { g_a, g_b })
                            {
                                mcu_t& x = *m;
                                x.sm = &sm; x.pcm = pcm; x.timer = &timer; x.lcd = lcd;
                                memcpy(x.rom1, rom1_pattern, sizeof(rom1_pattern));
                                memset(x.ram, 0, sizeof(x.ram));
                                for (int i = 0; i < 8; i++)
                                    x.r[i] = (uint16_t)(0x1000 + i);
                                x.r[7] = 0xfe00;      /* stack in internal RAM */
                                x.pc = 0x4321;
                                x.cp = 0x02;
                                x.sr = (uint16_t)(mask << 8);
                                x.sleep = 0;
                                x.exception_pending = (MCU_Exception_Source)exc;
                                x.interrupt_pending  = {};
                                x.trapa_pending      = {};
                                for (int i = 0; i < INTERRUPT_SOURCE_MAX; i++)
                                    if (pend & (1u << i))
                                        x.interrupt_pending.Include((MCU_Interrupt_Source)i);
                                memset(x.dev_register, 0, sizeof(x.dev_register));
                                x.dev_register[DEV_IPRA]  = ipr[0];
                                x.dev_register[DEV_IPRB]  = ipr[1];
                                x.dev_register[DEV_IPRC]  = ipr[2];
                                x.dev_register[DEV_IPRD]  = ipr[3];
                                x.dev_register[DEV_P1CR]  = p1cr;
                                x.dev_register[DEV_RAMCR] = 0x80;
                            }

                            StubLog().clear();
                            Upstream_Interrupt_Handle(a);
                            const std::vector<StubCall> log_a = StubLog();

                            StubLog().clear();
                            Handler_Under_Test(b);
                            const std::vector<StubCall> log_b = StubLog();

                            const bool same =
                                memcmp(a.r, b.r, sizeof(a.r)) == 0 &&
                                a.pc == b.pc && a.sr == b.sr && a.cp == b.cp &&
                                a.sleep == b.sleep &&
                                a.exception_pending == b.exception_pending &&
                                memcmp(&a.interrupt_pending, &b.interrupt_pending,
                                       sizeof(a.interrupt_pending)) == 0 &&
                                memcmp(&a.trapa_pending, &b.trapa_pending,
                                       sizeof(a.trapa_pending)) == 0 &&
                                memcmp(a.ram, b.ram, sizeof(a.ram)) == 0 &&
                                log_a == log_b;

                            if (!same)
                            {
                                if (failures < 10)
                                {
                                    printf("MISMATCH mask=%u p1cr=%02x ipr=%02x%02x%02x%02x "
                                           "pend=%06x exc=%d: pc %04x/%04x sr %04x/%04x "
                                           "calls %zu/%zu\n",
                                           mask, p1cr, ipr[0], ipr[1], ipr[2], ipr[3],
                                           pend, exc, a.pc, b.pc, a.sr, b.sr,
                                           log_a.size(), log_b.size());
                                }
                                failures++;
                            }
                            checked++;
                        }
                    }
                }
            }
        }
    }

    printf("checked %llu interrupt-handler states\n", (unsigned long long)checked);
    if (failures)
    {
        printf("FAIL: %d mismatches\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
