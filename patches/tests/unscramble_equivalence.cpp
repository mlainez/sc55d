/*
 * Exhaustive equivalence check for
 * patches/0002-rom_io-table-driven-unscramble.patch.
 *
 * The patch replaces a per-byte bit-shuffling loop with lookup tables. Both the
 * address permutation and the data permutation are fixed, so equivalence can be
 * proved outright rather than sampled -- no ROMs needed.
 *
 *   g++ -O2 -std=c++23 -o unscramble_equivalence unscramble_equivalence.cpp
 *   ./unscramble_equivalence
 *
 * Checks every one of the 256 data bytes, and every address in [0, 8 MiB) --
 * the largest buffer the core unscrambles is the 8 MiB JV-880 expansion rom.
 */
#include <array>
#include <cstdint>
#include <cstdio>

namespace original {

// Verbatim from the core's rom_io.cpp, factored into its two halves.
int Address(int i)
{
    int address = i & ~0xfffff;
    static const int aa[] = {2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14, 19};
    for (int j = 0; j < 20; j++)
    {
        if (i & (1 << j))
            address |= 1 << aa[j];
    }
    return address;
}

uint8_t Data(uint8_t srcdata)
{
    uint8_t data = 0;
    static const int dd[] = {2, 0, 4, 5, 7, 6, 3, 1};
    for (int j = 0; j < 8; j++)
    {
        if (srcdata & (1 << dd[j]))
            data |= 1 << j;
    }
    return data;
}

} // namespace original

namespace patched {

struct UnscrambleTables
{
    std::array<uint8_t, 256>   data{};
    std::array<uint32_t, 1024> addr_lo{};
    std::array<uint32_t, 1024> addr_hi{};
};

constexpr UnscrambleTables MakeUnscrambleTables()
{
    constexpr int aa[] = {2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14, 19};
    constexpr int dd[] = {2, 0, 4, 5, 7, 6, 3, 1};

    UnscrambleTables tables{};

    for (int value = 0; value < 256; value++)
    {
        uint8_t data = 0;
        for (int j = 0; j < 8; j++)
        {
            if (value & (1 << j))
                data |= (uint8_t)(1 << dd[j]);
        }
        tables.data[data] = (uint8_t)value;
    }

    for (int value = 0; value < 1024; value++)
    {
        uint32_t lo = 0;
        uint32_t hi = 0;
        for (int j = 0; j < 10; j++)
        {
            if (value & (1 << j))
            {
                lo |= 1u << aa[j];
                hi |= 1u << aa[j + 10];
            }
        }
        tables.addr_lo[value] = lo;
        tables.addr_hi[value] = hi;
    }

    return tables;
}

constexpr UnscrambleTables kUnscramble = MakeUnscrambleTables();

int Address(int i)
{
    return (i & ~0xfffff) | (int)kUnscramble.addr_lo[i & 0x3ff]
           | (int)kUnscramble.addr_hi[(i >> 10) & 0x3ff];
}

uint8_t Data(uint8_t srcdata)
{
    return kUnscramble.data[srcdata];
}

} // namespace patched

int main()
{
    int failures = 0;

    for (int value = 0; value < 256; value++)
    {
        const uint8_t want = original::Data((uint8_t)value);
        const uint8_t got  = patched::Data((uint8_t)value);
        if (want != got)
        {
            if (failures++ < 8)
                printf("data  %02x: want %02x got %02x\n", value, want, got);
        }
    }
    printf("data permutation: 256 values checked\n");

    const int kMax = 0x800000; // 8 MiB, the JV-880 expansion rom
    for (int i = 0; i < kMax; i++)
    {
        const int want = original::Address(i);
        const int got  = patched::Address(i);
        if (want != got)
        {
            if (failures++ < 8)
                printf("addr %07x: want %07x got %07x\n", i, want, got);
        }
    }
    printf("address permutation: %d addresses checked\n", kMax);

    if (failures)
    {
        printf("\nFAILED: %d mismatches\n", failures);
        return 1;
    }
    printf("\nPASS: the tables reproduce the original permutations exactly\n");
    return 0;
}
