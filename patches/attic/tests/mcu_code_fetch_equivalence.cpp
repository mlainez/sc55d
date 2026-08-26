/*
 * Differential check for
 * patches/0001-mcu-code-fetch-fast-path.patch.
 *
 * Upstream MCU_ReadCode() is exactly
 *
 *     MCU_Read(mcu, MCU_GetAddress(mcu.cp, mcu.pc))
 *
 * and the patch answers two of MCU_Read()'s cases in the header instead. So the
 * property to prove is: for every (cp, pc) the emulator can present, the patched
 * MCU_ReadCode() returns what MCU_Read() returns AND leaves the machine in the
 * state MCU_Read() would have left it in.
 *
 * The second half is the one that matters. Some addresses in page 0 latch a
 * button matrix, clear a gate-array interrupt or poke the LCD; short-circuiting
 * any of those would be silently wrong. So this links the *real* MCU_Read() out
 * of mcu.cpp against stubs that record every outward call, snapshots every
 * scalar in mcu_t either side of each call, and demands all three agree:
 * return value, recorded calls, and machine state.
 *
 * No ROMs: rom1/rom2/ram/sram/nvram/cardram are filled with a pseudo-random
 * pattern, so returning the right byte from the wrong offset is caught too.
 *
 * All 256 * 65536 (cp, pc) pairs are checked for all nine romsets, and the
 * pages the fast path can claim are checked again for four smaller values of
 * rom2_mask -- emu.cpp derives it from the size of the ROM2 file actually
 * loaded, so it is not a constant.
 *
 *   g++ -O2 -std=c++23 -I<core>/src/backend -I<build>/backend \
 *       -o mcu_code_fetch_equivalence mcu_code_fetch_equivalence.cpp \
 *       <core>/src/backend/mcu.cpp
 *   ./mcu_code_fetch_equivalence
 *
 * Build it with -DBREAK_FAST_PATH=1..3 to check the test can fail; each picks a
 * plausible-looking mistake in the patch.
 */
#include "mcu_core_stubs.h"

#include <cstdio>
#include <cstdlib>

namespace {

/* The patch's fast path, restated here so the test can be pointed at a
 * deliberately broken version of it. With BREAK_FAST_PATH unset this is a
 * literal copy of what the patch puts in mcu.h, and the test then also runs
 * the header's own MCU_ReadCode() and requires it to agree. */
uint8_t FetchUnderTest(mcu_t& mcu)
{
    const uint8_t  cp = mcu.cp;
    const uint16_t pc = mcu.pc;
#if BREAK_FAST_PATH == 1
    /* off-by-one on the page range: page 5 is SRAM on mk1, not ROM2 */
    if (cp == 0)
    {
        if (!(pc & 0x8000))
            return mcu.rom1[pc];
    }
    else if (cp <= 5)
    {
        return mcu.rom2[(((uint32_t)cp << 16) | pc) & 0x3ffffu & mcu.rom2_mask];
    }
#elif BREAK_FAST_PATH == 2
    /* forgets that page 0 above 0x8000 is I/O, RAM and SRAM */
    if (cp == 0)
        return mcu.rom1[pc & 0x7fff];
    else if (cp <= 4)
        return mcu.rom2[(((uint32_t)cp << 16) | pc) & 0x3ffffu & mcu.rom2_mask];
#elif BREAK_FAST_PATH == 3
    /* drops the rom2 mask */
    if (cp == 0)
    {
        if (!(pc & 0x8000))
            return mcu.rom1[pc];
    }
    else if (cp <= 4)
    {
        return mcu.rom2[(((uint32_t)cp << 16) | pc) & 0x3ffffu];
    }
#else
    if (cp == 0)
    {
        if (!(pc & 0x8000))
            return mcu.rom1[pc];
    }
    else if (cp <= 4)
    {
        return mcu.rom2[(((uint32_t)cp << 16) | pc) & 0x3ffffu & mcu.rom2_mask];
    }
#endif
    return MCU_Read(mcu, MCU_GetAddress(cp, pc));
}

uint32_t Rand(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void Fill(uint8_t* p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)Rand(seed);
}

const Romset kRomsets[] = {
    Romset::MK2, Romset::ST, Romset::MK1, Romset::CM300, Romset::JV880,
    Romset::SCB55, Romset::RLP3237, Romset::SC155, Romset::SC155MK2,
};

const char* Name(Romset r)
{
    switch (r)
    {
    case Romset::MK2:      return "mk2";
    case Romset::ST:       return "st";
    case Romset::MK1:      return "mk1";
    case Romset::CM300:    return "cm300";
    case Romset::JV880:    return "jv880";
    case Romset::SCB55:    return "scb55";
    case Romset::RLP3237:  return "rlp3237";
    case Romset::SC155:    return "sc155";
    case Romset::SC155MK2: return "sc155mk2";
    default:               return "?";
    }
}

mcu_t* g_mcu;

} // namespace

int main()
{
    g_mcu = new mcu_t();
    mcu_t& mcu = *g_mcu;

    /* mcu.cpp dereferences these for the I/O cases; the stubs never look at
     * them, they only have to be valid addresses. */
    static submcu_t     sm{};
    static pcm_t*       pcm = new pcm_t();
    static mcu_timer_t  timer{};
    static lcd_t*       lcd = (lcd_t*)calloc(1, 1 << 20);
    mcu.sm = &sm;
    mcu.pcm = pcm;
    mcu.timer = &timer;
    mcu.lcd = lcd;

    Fill(mcu.rom1, sizeof(mcu.rom1), 0x12345678);
    Fill(mcu.rom2, sizeof(mcu.rom2), 0x2468ace0);
    Fill(mcu.ram, sizeof(mcu.ram), 0x0f0f0f0f);
    Fill(mcu.sram, sizeof(mcu.sram), 0xdeadbeef);
    Fill(mcu.nvram, sizeof(mcu.nvram), 0xfeedface);
    Fill(mcu.cardram, sizeof(mcu.cardram), 0xc0ffee11);

    Snapshot before, after_fast, after_slow;
    std::vector<StubCall> log_fast, log_slow;
    uint64_t checked = 0, fast = 0;
    int failures = 0;

    for (Romset romset : kRomsets)
    {
        MCU_SetRomset(mcu, romset);

        /* Exercise the paths that depend on more than the address: the
         * internal-RAM enable, the sub-MCU chip select and the port latches. */
        mcu.dev_register[DEV_RAMCR] = 0x80;
        mcu.dev_register[DEV_P1CR]  = 0x60;
        mcu.button_pressed = 0x12345678u;

        /* rom2_mask is (ROM2 file size - 1), so it is not always the full
         * 0x7ffff: emu.cpp derives it from whatever ROM2 was loaded. The full
         * cp sweep runs once; the smaller masks only need the pages the fast
         * path can claim. */
        static const uint32_t kMasks[] = { ROM2_SIZE - 1, 0x3ffff, 0x1ffff, 0xffff, 0x7fff };
        for (uint32_t mask : kMasks)
        {
        mcu.rom2_mask = mask;
        const uint32_t cp_limit = (mask == (uint32_t)(ROM2_SIZE - 1)) ? 256u : 16u;
        for (uint32_t cp = 0; cp < cp_limit; cp++)
        {
            for (uint32_t pc = 0; pc < 0x10000; pc++)
            {
                mcu.cp = (uint8_t)cp;
                mcu.pc = (uint16_t)pc;

                before.Take(mcu);

                StubLog().clear();
                const uint8_t got = FetchUnderTest(mcu);
                log_fast = StubLog();
                after_fast.Take(mcu);

                /* the shipped header, not just the copy above */
                before.Restore(mcu);
                StubLog().clear();
                const uint8_t got_hdr = MCU_ReadCode(mcu);
                const std::vector<StubCall> log_hdr = StubLog();
                Snapshot after_hdr;
                after_hdr.Take(mcu);

                before.Restore(mcu);
                StubLog().clear();
                const uint8_t want = MCU_Read(mcu, MCU_GetAddress((uint8_t)cp, (uint16_t)pc));
                log_slow = StubLog();
                after_slow.Take(mcu);

                if (log_slow.empty() && after_slow == before)
                    fast++;

                if (got != want || log_fast != log_slow || !(after_fast == after_slow)
                    || got_hdr != want || log_hdr != log_slow || !(after_hdr == after_slow))
                {
                    if (failures < 10)
                    {
                        printf("MISMATCH romset=%s cp=%02x pc=%04x: "
                               "fast=%02x slow=%02x calls %zu/%zu state %s\n",
                               Name(romset), cp, pc, got, want,
                               log_fast.size(), log_slow.size(),
                               (after_fast == after_slow) ? "same" : "DIFFER");
                    }
                    failures++;
                }

                before.Restore(mcu);
                checked++;
            }
        }
        }
    }

    printf("checked %llu (romset, rom2_mask, cp, pc) combinations over %zu romsets\n",
           (unsigned long long)checked, sizeof(kRomsets) / sizeof(kRomsets[0]));
    printf("%llu of them are side-effect-free reads the fast path may take\n",
           (unsigned long long)fast);

    if (failures)
    {
        printf("FAIL: %d mismatches\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
