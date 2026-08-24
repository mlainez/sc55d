/*
 * Differential test for the closed-form / deferred TIMER_Clock().
 *
 * It builds the *real* mcu_timer.cpp twice -- upstream's in namespace `ref`,
 * the rewritten one in namespace `neu` -- against a stub mcu.h that supplies
 * only what the timer touches (the interrupt bitset and the DEV_TMR_* ids).
 * No ROMs, no audio, no emulator.
 *
 * What is compared is everything the rest of the emulator can actually see,
 * checked after every single operation:
 *
 *   - timer.cycles                       (read by nothing else, but it feeds
 *                                         the next call's loop bound, and the
 *                                         patch must land on it exactly)
 *   - mcu.interrupt_pending              (read by MCU_Interrupt_Handle() at the
 *                                         top of every MCU_Step, i.e. between
 *                                         any two TIMER_Clock calls)
 *   - the byte returned by every TIMER_Read()/TIMER_Read2()
 *
 * The rewritten version deliberately leaves frc/tcnt stale between events, so
 * comparing the structs field by field would be wrong -- it is *observational*
 * equivalence that is being claimed. To pin the hidden state down anyway, the
 * driver periodically reads every register of every timer through the public
 * accessors, on both sides, and compares the bytes; that forces a sync and
 * makes any divergence in a counter, a flag or a temp register visible.
 *
 * Drivers are randomised but deterministic, and mix:
 *   - the real call pattern (TIMER_Clock every 12 mcu cycles, forever)
 *   - long jumps, so a single call has to cross thousands of ticks and several
 *     compare-matches and overflows
 *   - register traffic at every timer address, which is what programs the
 *     dividers, the compare registers, CCLRA, the interrupt enables, and what
 *     clears flags and pending requests
 *   - romset switches (they swap the step tables under the timer) and resets
 */
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace ref {
#include "orig/mcu_timer.h"
#include "orig/mcu_timer.cpp"
} // namespace ref

namespace neu {
#include "newf/mcu_timer.h"
#include "newf/mcu_timer.cpp"
} // namespace neu

namespace {

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint32_t Next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 32);
    }
    uint32_t Below(uint32_t n) { return Next() % n; }
};

struct Pair
{
    ref::mcu_t       ref_mcu;
    ref::mcu_timer_t ref_timer;
    neu::mcu_t       neu_mcu;
    neu::mcu_timer_t neu_timer;

    uint64_t mcu_cycles = 0;

    Pair(bool is_mk1)
    {
        ref_mcu.is_mk1 = is_mk1;
        neu_mcu.is_mk1 = is_mk1;
        ref::TIMER_Init(ref_timer, ref_mcu);
        neu::TIMER_Init(neu_timer, neu_mcu);
        ref::TIMER_NotifyRomsetChange(ref_timer);
        neu::TIMER_NotifyRomsetChange(neu_timer);
        ref::TIMER_Reset(ref_timer);
        neu::TIMER_Reset(neu_timer);
    }
};

const char* g_what = "";
long        g_failures = 0;

bool Check(Pair& p, const char* what, int detail)
{
    if (p.ref_timer.cycles == p.neu_timer.cycles &&
        p.ref_mcu.interrupt_pending.bits == p.neu_mcu.interrupt_pending.bits)
        return true;

    if (g_failures++ < 8)
    {
        printf("MISMATCH after %s (%d), mcu_cycles=%llu\n", what, detail,
               (unsigned long long)p.mcu_cycles);
        printf("  timer.cycles      ref=%llu neu=%llu\n",
               (unsigned long long)p.ref_timer.cycles, (unsigned long long)p.neu_timer.cycles);
        printf("  interrupt_pending ref=%06llx neu=%06llx\n",
               (unsigned long long)p.ref_mcu.interrupt_pending.bits,
               (unsigned long long)p.neu_mcu.interrupt_pending.bits);
    }
    return false;
}

bool CheckByte(Pair& p, const char* what, int detail, uint8_t a, uint8_t b)
{
    if (a != b)
    {
        if (g_failures++ < 8)
            printf("MISMATCH byte from %s (%02x): ref=%02x neu=%02x, mcu_cycles=%llu\n", what, detail,
                   a, b, (unsigned long long)p.mcu_cycles);
        return false;
    }
    return Check(p, what, detail);
}

// Read every register of every timer through the public accessors on both
// sides and compare. This forces the deferred implementation to materialise
// everything it was holding back, so it is a full state comparison.
bool Observe(Pair& p)
{
    bool ok = true;
    for (uint32_t t = 1; t <= 3; t++)
    {
        for (uint32_t reg = 0; reg <= 9; reg++)
        {
            const uint32_t addr = (t << 4) | reg;
            const uint8_t  a    = ref::TIMER_Read(p.ref_timer, addr);
            const uint8_t  b    = neu::TIMER_Read(p.neu_timer, addr);
            ok &= CheckByte(p, "observe frt", (int)addr, a, b);
        }
    }
    for (uint32_t addr = ref::DEV_TMR_TCR; addr <= ref::DEV_TMR_TCNT; addr++)
    {
        const uint8_t a = ref::TIMER_Read2(p.ref_timer, addr);
        const uint8_t b = neu::TIMER_Read2(p.neu_timer, addr);
        ok &= CheckByte(p, "observe tmr", (int)addr, a, b);
    }
    return ok;
}

// One randomised run. `long_jumps` turns on multi-thousand-tick TIMER_Clock
// steps; `hot` biases the compare registers small so matches happen constantly.
bool Run(uint64_t seed, bool is_mk1, bool long_jumps, bool hot, int steps)
{
    Rng  rng(seed);
    Pair p(is_mk1);

    for (int i = 0; i < steps; i++)
    {
        const uint32_t roll = rng.Below(1000);

        if (roll < 700)
        {
            // The real call pattern: 12 mcu cycles per emulated instruction.
            p.mcu_cycles += 12;
            ref::TIMER_Clock(p.ref_timer, p.mcu_cycles);
            neu::TIMER_Clock(p.neu_timer, p.mcu_cycles);
            if (!Check(p, "clock", 12))
                return false;
        }
        else if (roll < 760 && long_jumps)
        {
            // A single call that has to cross many ticks and several events.
            p.mcu_cycles += 12 + rng.Below(1u << (12 + rng.Below(9)));
            ref::TIMER_Clock(p.ref_timer, p.mcu_cycles);
            neu::TIMER_Clock(p.neu_timer, p.mcu_cycles);
            if (!Check(p, "clock long", 0))
                return false;
        }
        else if (roll < 860)
        {
            const uint32_t t    = 1 + rng.Below(3);
            const uint32_t reg  = rng.Below(10);
            const uint32_t addr = (t << 4) | reg;
            uint8_t        data = (uint8_t)rng.Next();
            if (hot && (reg == 4 || reg == 6 || reg == 2)) // OCRAH / OCRBH / FRCH
                data = (uint8_t)(rng.Below(2) ? 0 : 0xff);
            ref::TIMER_Write(p.ref_timer, addr, data);
            neu::TIMER_Write(p.neu_timer, addr, data);
            if (!Check(p, "frt write", (int)addr))
                return false;
        }
        else if (roll < 900)
        {
            const uint32_t addr = ref::DEV_TMR_TCR + rng.Below(5);
            const uint8_t  data = (uint8_t)rng.Next();
            ref::TIMER2_Write(p.ref_timer, addr, data);
            neu::TIMER2_Write(p.neu_timer, addr, data);
            if (!Check(p, "tmr write", (int)addr))
                return false;
        }
        else if (roll < 950)
        {
            const uint32_t t    = 1 + rng.Below(3);
            const uint32_t addr = (t << 4) | rng.Below(10);
            const uint8_t  a    = ref::TIMER_Read(p.ref_timer, addr);
            const uint8_t  b    = neu::TIMER_Read(p.neu_timer, addr);
            if (!CheckByte(p, "frt read", (int)addr, a, b))
                return false;
        }
        else if (roll < 975)
        {
            const uint32_t addr = ref::DEV_TMR_TCR + rng.Below(5);
            const uint8_t  a    = ref::TIMER_Read2(p.ref_timer, addr);
            const uint8_t  b    = neu::TIMER_Read2(p.neu_timer, addr);
            if (!CheckByte(p, "tmr read", (int)addr, a, b))
                return false;
        }
        else if (roll < 995)
        {
            if (!Observe(p))
                return false;
        }
        else if (roll < 999)
        {
            // Romset switch: swaps the step tables under a running timer.
            const bool mk1     = rng.Below(2) != 0;
            p.ref_mcu.is_mk1   = mk1;
            p.neu_mcu.is_mk1   = mk1;
            ref::TIMER_NotifyRomsetChange(p.ref_timer);
            neu::TIMER_NotifyRomsetChange(p.neu_timer);
            if (!Check(p, "romset change", mk1))
                return false;
        }
        else
        {
            ref::TIMER_Reset(p.ref_timer);
            neu::TIMER_Reset(p.neu_timer);
            if (!Check(p, "reset", 0))
                return false;
        }
    }

    return Observe(p);
}

// The specific sequence that a naive "flags are sticky so nothing can happen"
// implementation gets wrong: a compare-match flag is raised while its interrupt
// is disabled, and the enable is turned on afterwards. The hardware re-raises
// the request on the very next tick even though no flag changes.
bool RunEnableAfterFlag()
{
    for (int frt_id = 0; frt_id < 3; frt_id++)
    {
        Pair p(false);
        // ocra = 4, no interrupts enabled yet, free running with the /4 divider.
        const uint32_t base = (uint32_t)(frt_id + 1) << 4;
        ref::TIMER_Write(p.ref_timer, base + 4, 0x00);
        neu::TIMER_Write(p.neu_timer, base + 4, 0x00);
        ref::TIMER_Write(p.ref_timer, base + 5, 0x04);
        neu::TIMER_Write(p.neu_timer, base + 5, 0x04);

        // Run past the match so OCFA is set with the interrupt still masked.
        p.mcu_cycles += 12 * 64;
        ref::TIMER_Clock(p.ref_timer, p.mcu_cycles);
        neu::TIMER_Clock(p.neu_timer, p.mcu_cycles);
        if (!Check(p, "enable-after-flag: settle", frt_id))
            return false;

        // Now enable OCIA. Upstream raises the request on the next tick.
        ref::TIMER_Write(p.ref_timer, base + 0, 0x20);
        neu::TIMER_Write(p.neu_timer, base + 0, 0x20);
        for (int i = 0; i < 8; i++)
        {
            p.mcu_cycles += 12;
            ref::TIMER_Clock(p.ref_timer, p.mcu_cycles);
            neu::TIMER_Clock(p.neu_timer, p.mcu_cycles);
            if (!Check(p, "enable-after-flag: tick", frt_id * 100 + i))
                return false;
        }
        if (!Observe(p))
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    int runs = argc > 1 ? atoi(argv[1]) : 240;

    if (!RunEnableAfterFlag())
        printf("(enable-after-flag scenario failed)\n");

    long ok = 0;
    for (int r = 0; r < runs; r++)
    {
        const bool is_mk1     = (r & 1) != 0;
        const bool long_jumps = (r & 2) != 0;
        const bool hot        = (r & 4) != 0;
        if (Run((uint64_t)r + 1, is_mk1, long_jumps, hot, 20000))
            ok++;
        if (g_failures > 8)
            break;
    }

    printf("%ld/%d randomised runs clean, %ld mismatches\n", ok, runs, g_failures);
    if (g_failures)
    {
        printf("\nFAILED\n");
        return 1;
    }
    printf("\nPASS: identical timer.cycles, identical interrupt_pending and identical\n"
           "      register reads at every observation point\n");
    return 0;
}
