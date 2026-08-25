/*
 * Differential check for
 * patches/0015-mcu-word-access-fast-path.patch.
 *
 * Upstream MCU_Read16() and MCU_Write16() are byte-wise: two full
 * MCU_Read()/MCU_Write() calls. The patch answers the side-effect-free
 * regions -- rom1, the battery SRAM window, and the rom2 pages -- with one
 * walk of the decode ladder. So the property to prove is: for every address,
 * every romset, both RAMCR states and every rom2_mask shape, the patched
 * word access returns what the byte-wise composition returns, makes exactly
 * the outward calls it makes, and leaves the machine -- scalars AND memory
 * arrays -- in the same state.
 *
 * Reads run against one machine with the scalar state restored around each
 * variant, exactly as mcu_code_fetch_equivalence.cpp does. Writes cannot be
 * checked that way -- a write's effect lives in the arrays the snapshot
 * deliberately excludes -- so writes drive two machines in lockstep, one
 * through the patched MCU_Write16() and one through the byte-wise
 * composition, with a unique value per address, and every byte of every
 * writable array is compared at the end of each pass. A fast path that wrote
 * the right value at the wrong offset ends the sweep with different arrays.
 *
 * The patch's fast path is also restated here behind BREAK_WORD, so the
 * mutants can prove the test is capable of failing:
 *
 *   -DBREAK_WORD=1  read: SRAM window off by one, claims 0xe000 (PCM space)
 *   -DBREAK_WORD=2  read: page range off by one, claims page 5 (mk1 SRAM)
 *   -DBREAK_WORD=3  read: rom1 arm forgets page 0's upper half is I/O
 *   -DBREAK_WORD=4  write: window swallows the RAMCR and device windows
 *
 *   g++ -O2 -std=c++23 -DNDEBUG -I. -I<core>/src/backend -I<build>/backend \
 *       -o mcu_word_access_equivalence mcu_word_access_equivalence.cpp \
 *       <core>/src/backend/mcu.cpp
 */
#include "mcu_core_stubs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/* The patch's read fast path, restated so a broken version can be swapped in
 * without touching the shipped file. With BREAK_WORD unset this is a literal
 * copy of what the patch puts in MCU_Read16(). */
uint16_t WordReadUnderTest(mcu_t& mcu, uint32_t address)
{
    address &= ~1u;
    const uint32_t page = (address >> 16) & 0xfu;
    const uint32_t lo = address & 0xffffu;
#if BREAK_WORD == 1
    if (page == 0)
    {
        if (!(lo & 0x8000u))
            return (uint16_t)((mcu.rom1[lo] << 8) + mcu.rom1[lo + 1]);
        if (lo <= 0xe000u)
        {
            const uint32_t off = lo & 0x7fffu;
            return (uint16_t)((mcu.sram[off] << 8) + mcu.sram[off + 1]);
        }
    }
    else if (page <= 4)
    {
        const uint32_t hi = mcu.rom2[(address & 0x3ffffu) & mcu.rom2_mask];
        const uint32_t lo8 = mcu.rom2[((address + 1) & 0x3ffffu) & mcu.rom2_mask];
        return (uint16_t)((hi << 8) + lo8);
    }
#elif BREAK_WORD == 2
    if (page == 0)
    {
        if (!(lo & 0x8000u))
            return (uint16_t)((mcu.rom1[lo] << 8) + mcu.rom1[lo + 1]);
        if (lo < 0xe000u)
        {
            const uint32_t off = lo & 0x7fffu;
            return (uint16_t)((mcu.sram[off] << 8) + mcu.sram[off + 1]);
        }
    }
    else if (page <= 5)
    {
        const uint32_t hi = mcu.rom2[(address & 0x3ffffu) & mcu.rom2_mask];
        const uint32_t lo8 = mcu.rom2[((address + 1) & 0x3ffffu) & mcu.rom2_mask];
        return (uint16_t)((hi << 8) + lo8);
    }
#elif BREAK_WORD == 3
    if (page == 0)
    {
        return (uint16_t)((mcu.rom1[lo & 0x7fffu] << 8) + mcu.rom1[(lo + 1) & 0x7fffu]);
    }
    else if (page <= 4)
    {
        const uint32_t hi = mcu.rom2[(address & 0x3ffffu) & mcu.rom2_mask];
        const uint32_t lo8 = mcu.rom2[((address + 1) & 0x3ffffu) & mcu.rom2_mask];
        return (uint16_t)((hi << 8) + lo8);
    }
#else
    if (page == 0)
    {
        if (!(lo & 0x8000u))
            return (uint16_t)((mcu.rom1[lo] << 8) + mcu.rom1[lo + 1]);
        if (lo < 0xe000u)
        {
            const uint32_t off = lo & 0x7fffu;
            return (uint16_t)((mcu.sram[off] << 8) + mcu.sram[off + 1]);
        }
    }
    else if (page <= 4)
    {
        const uint32_t hi = mcu.rom2[(address & 0x3ffffu) & mcu.rom2_mask];
        const uint32_t lo8 = mcu.rom2[((address + 1) & 0x3ffffu) & mcu.rom2_mask];
        return (uint16_t)((hi << 8) + lo8);
    }
#endif
    uint8_t b0 = MCU_Read(mcu, address);
    uint8_t b1 = MCU_Read(mcu, address + 1);
    return (uint16_t)((b0 << 8) + b1);
}

/* The patch's write fast path, same idea. */
void WordWriteUnderTest(mcu_t& mcu, uint32_t address, uint16_t value)
{
    address &= ~1u;
    if (((address >> 16) & 0xfu) == 0)
    {
        const uint32_t lo = address & 0xffffu;
#if BREAK_WORD == 4
        if (lo >= 0x8000u && lo < 0xff80u)
#else
        if (lo >= 0x8000u && lo < 0xe000u)
#endif
        {
            const uint32_t off = lo & 0x7fffu;
            mcu.sram[off] = (uint8_t)(value >> 8);
            mcu.sram[off + 1] = (uint8_t)(value & 0xff);
            return;
        }
    }
    MCU_Write(mcu, address, (uint8_t)(value >> 8));
    MCU_Write(mcu, address + 1, (uint8_t)(value & 0xff));
}

uint32_t Rand(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

void Fill(uint8_t* p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)Rand(seed);
}

const Romset kRomsets[] = {
    Romset::MK2, Romset::ST, Romset::MK1, Romset::CM300, Romset::JV880,
    Romset::SCB55, Romset::RLP3237, Romset::SC155, Romset::SC155MK2,
};

void Setup(mcu_t& mcu, submcu_t* sm, pcm_t* pcm, mcu_timer_t* timer, lcd_t* lcd)
{
    mcu.sm = sm;
    mcu.pcm = pcm;
    mcu.timer = timer;
    mcu.lcd = lcd;
    Fill(mcu.rom1, sizeof(mcu.rom1), 0x12345678);
    Fill(mcu.rom2, sizeof(mcu.rom2), 0x2468ace0);
    Fill(mcu.ram, sizeof(mcu.ram), 0x0f0f0f0f);
    Fill(mcu.sram, sizeof(mcu.sram), 0xdeadbeef);
    Fill(mcu.nvram, sizeof(mcu.nvram), 0xfeedface);
    Fill(mcu.cardram, sizeof(mcu.cardram), 0xc0ffee11);
}

int failures = 0;

void Complain(const char* what, const char* romset, uint32_t ramcr,
              uint32_t mask, uint32_t address)
{
    if (failures < 10)
        printf("MISMATCH %s romset=%s ramcr=%02x mask=%05x address=%05x\n",
               what, romset, ramcr, mask, address);
    failures++;
}

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

} // namespace

int main()
{
    static submcu_t    sm{};
    static pcm_t*      pcm = new pcm_t();
    static mcu_timer_t timer{};
    static lcd_t*      lcd = (lcd_t*)calloc(1, 1 << 20);

    /* One machine for reads (scalars restored around each variant), and a
     * lockstep pair plus the restated wrapper's own machine for writes. */
    mcu_t& mr = *new mcu_t();
    mcu_t& w_fast = *new mcu_t();
    mcu_t& w_slow = *new mcu_t();
    mcu_t& w_test = *new mcu_t();
    Setup(mr, &sm, pcm, &timer, lcd);
    Setup(w_fast, &sm, pcm, &timer, lcd);
    Setup(w_slow, &sm, pcm, &timer, lcd);
    Setup(w_test, &sm, pcm, &timer, lcd);

    Snapshot before, after_fast, after_slow, after_test;
    uint64_t checked = 0, fast_reads = 0;

    for (Romset romset : kRomsets)
    {
        MCU_SetRomset(mr, romset);
        MCU_SetRomset(w_fast, romset);
        MCU_SetRomset(w_slow, romset);
        MCU_SetRomset(w_test, romset);

        /* Passes: the full mask over the whole space in both RAMCR states,
         * then the smaller mask shapes over the pages the read fast path can
         * claim. rom2_mask is (ROM2 file size - 1) in emu.cpp, so it is not
         * always the full 0x7ffff -- and 0x2ffff checks a shape that is not
         * even a power of two minus one. */
        struct Pass { uint32_t mask, ramcr, limit; };
        const Pass passes[] = {
            { ROM2_SIZE - 1, 0x80, 0x100000 },
            { ROM2_SIZE - 1, 0x00, 0x100000 },
            { 0x3ffff,       0x80, 0x50000 },
            { 0x2ffff,       0x80, 0x50000 },
            { 0xffff,        0x80, 0x50000 },
            { 0x7fff,        0x80, 0x50000 },
        };

        for (const Pass& pass : passes)
        {
            mr.rom2_mask = pass.mask;
            w_fast.rom2_mask = w_slow.rom2_mask = w_test.rom2_mask = pass.mask;
            mr.dev_register[DEV_RAMCR] = (uint8_t)pass.ramcr;
            w_fast.dev_register[DEV_RAMCR] = (uint8_t)pass.ramcr;
            w_slow.dev_register[DEV_RAMCR] = (uint8_t)pass.ramcr;
            w_test.dev_register[DEV_RAMCR] = (uint8_t)pass.ramcr;

            for (uint32_t address = 0; address < pass.limit; address += 2)
            {
                /* --- read --- */
                before.Take(mr);

                StubLog().clear();
                const uint16_t got = MCU_Read16(mr, address);
                std::vector<StubCall> log_got = StubLog();
                after_fast.Take(mr);

                before.Restore(mr);
                StubLog().clear();
                const uint16_t got_test = WordReadUnderTest(mr, address);
                std::vector<StubCall> log_test = StubLog();
                after_test.Take(mr);

                before.Restore(mr);
                StubLog().clear();
                uint8_t b0 = MCU_Read(mr, address);
                uint8_t b1 = MCU_Read(mr, address + 1);
                const uint16_t want = (uint16_t)((b0 << 8) + b1);
                std::vector<StubCall> log_want = StubLog();
                after_slow.Take(mr);

                if (log_want.empty() && after_slow == before)
                    fast_reads++;

                if (got != want || log_got != log_want || !(after_fast == after_slow)
                    || got_test != want || log_test != log_want
                    || !(after_test == after_slow))
                    Complain("read", Name(romset), pass.ramcr, pass.mask, address);

                before.Restore(mr);

                /* --- write, lockstep machines, unique value per address --- */
                const uint16_t value =
                    (uint16_t)((address * 2654435761u) >> 8 ^ (uint32_t)romset);

                StubLog().clear();
                MCU_Write16(w_fast, address, value);
                log_got = StubLog();
                after_fast.Take(w_fast);

                StubLog().clear();
                MCU_Write(w_slow, address, (uint8_t)(value >> 8));
                MCU_Write(w_slow, address + 1, (uint8_t)(value & 0xff));
                log_want = StubLog();
                after_slow.Take(w_slow);

                StubLog().clear();
                WordWriteUnderTest(w_test, address, value);
                log_test = StubLog();
                after_test.Take(w_test);

                if (log_got != log_want || !(after_fast == after_slow)
                    || log_test != log_want || !(after_test == after_slow))
                    Complain("write", Name(romset), pass.ramcr, pass.mask, address);

                checked++;
            }

            /* every byte the write sweep could have landed */
            if (memcmp(w_fast.ram, w_slow.ram, sizeof(w_fast.ram)) != 0
                || memcmp(w_fast.sram, w_slow.sram, sizeof(w_fast.sram)) != 0
                || memcmp(w_fast.nvram, w_slow.nvram, sizeof(w_fast.nvram)) != 0
                || memcmp(w_fast.cardram, w_slow.cardram, sizeof(w_fast.cardram)) != 0)
                Complain("write-arrays", Name(romset), pass.ramcr, pass.mask, 0);
            if (memcmp(w_test.ram, w_slow.ram, sizeof(w_test.ram)) != 0
                || memcmp(w_test.sram, w_slow.sram, sizeof(w_test.sram)) != 0
                || memcmp(w_test.nvram, w_slow.nvram, sizeof(w_test.nvram)) != 0
                || memcmp(w_test.cardram, w_slow.cardram, sizeof(w_test.cardram)) != 0)
                Complain("write-arrays-test", Name(romset), pass.ramcr, pass.mask, 0);
        }
    }

    printf("checked %llu (romset, ramcr, rom2_mask, address) word accesses, "
           "read and write each\n", (unsigned long long)checked);
    printf("%llu of the reads are side-effect-free and eligible for the fast path\n",
           (unsigned long long)fast_reads);

    if (failures)
    {
        printf("FAIL: %d mismatches\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
