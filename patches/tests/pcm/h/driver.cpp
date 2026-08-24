// ROM-free differential driver for PCM_Update.
//
// Builds a pcm_t with pseudorandom wave ROM and register state, drives
// PCM_Update for a large number of ticks while poking PCM_Write, and prints a
// running digest of the *entire* mutable PCM state (ram1, ram2, eram, all
// scalars) plus every sample handed to MCU_PostSample and every interrupt
// raised.  Two builds of pcm.cpp that behave identically print identical
// output, byte for byte.
//
// Usage: driver <mode> [seed]
//   mode 0 : mixed activity, mk2 filter path (is_mk1 = false)
//   mode 1 : mixed activity, mk1 filter path (is_mk1 = true)
//   mode 2 : all 32 voices permanently keyed on  (worst case for realtime)
//   mode 3 : all voices permanently off          (idle case)
//   mode 4 : mixed activity, jv880
//   mode 5 : mixed activity, reg_slots = 12  (short voice loop)
//   mode 6 : mixed activity, reg_slots = 1   (the fork's default)
//   mode 7 : mixed activity, reg_slots = 28  (fast-path boundary)
//   mode 8 : all voices on, phase increments spread so sub_phase_of covers 0..4
//   mode 9 : like mode 0 but with that same wide spread of phase increments
#include "harness.h"
#include <cstdlib>

struct Rng
{
    uint64_t s;
    Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint32_t next()
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint32_t)(s >> 16);
    }
};

static uint64_t g_state_hash = 1469598103934665603ull;
static void hash_bytes(uint64_t& h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
}

static void hash_pcm(uint64_t& h, const pcm_t& pcm)
{
    hash_bytes(h, pcm.ram1, sizeof(pcm.ram1));
    hash_bytes(h, pcm.ram2, sizeof(pcm.ram2));
    hash_bytes(h, pcm.eram, sizeof(pcm.eram));
    hash_bytes(h, &pcm.cycles, sizeof(pcm.cycles));
    hash_bytes(h, &pcm.voice_mask, sizeof(pcm.voice_mask));
    hash_bytes(h, &pcm.voice_mask_pending, sizeof(pcm.voice_mask_pending));
    hash_bytes(h, &pcm.write_latch, sizeof(pcm.write_latch));
    hash_bytes(h, &pcm.read_latch, sizeof(pcm.read_latch));
    hash_bytes(h, &pcm.wave_read_address, sizeof(pcm.wave_read_address));
    hash_bytes(h, &pcm.tv_counter, sizeof(pcm.tv_counter));
    hash_bytes(h, &pcm.wave_byte_latch, sizeof(pcm.wave_byte_latch));
    hash_bytes(h, &pcm.select_channel, sizeof(pcm.select_channel));
    hash_bytes(h, &pcm.config_reg_3c, sizeof(pcm.config_reg_3c));
    hash_bytes(h, &pcm.config_reg_3d, sizeof(pcm.config_reg_3d));
    hash_bytes(h, &pcm.irq_channel, sizeof(pcm.irq_channel));
    hash_bytes(h, &pcm.irq_assert, sizeof(pcm.irq_assert));
    hash_bytes(h, &pcm.voice_mask_updating, sizeof(pcm.voice_mask_updating));
    hash_bytes(h, &pcm.nfs, sizeof(pcm.nfs));
    hash_bytes(h, &pcm.accum_l, sizeof(pcm.accum_l));
    hash_bytes(h, &pcm.accum_r, sizeof(pcm.accum_r));
    hash_bytes(h, pcm.rcsum, sizeof(pcm.rcsum));
    hash_bytes(h, &pcm.config, sizeof(pcm.config));
}

int main(int argc, char** argv)
{
    int      mode = argc > 1 ? atoi(argv[1]) : 0;
    const bool quiet = getenv("PCM_QUIET") != nullptr;
    extern bool g_hash_off; g_hash_off = quiet;
    uint64_t seed = argc > 2 ? strtoull(argv[2], nullptr, 0) : 12345;

    static mcu_t mcu;
    static pcm_t pcm;

    mcu.is_mk1   = (mode == 1);
    mcu.is_jv880 = (mode == 4);

    PCM_Init(pcm, mcu);

    Rng rng(seed);

    // Wave ROM: pseudorandom, so the DPCM/interpolation paths see real data.
    for (size_t i = 0; i < sizeof(pcm.waverom1); i++) pcm.waverom1[i] = (uint8_t)rng.next();
    for (size_t i = 0; i < sizeof(pcm.waverom2); i++) pcm.waverom2[i] = (uint8_t)rng.next();
    for (size_t i = 0; i < sizeof(pcm.waverom3); i++) pcm.waverom3[i] = (uint8_t)rng.next();

    // Register file: pseudorandom but in plausible widths.
    for (int s = 0; s < 32; s++)
    {
        for (int i = 0; i < 8; i++)  pcm.ram1[s][i] = rng.next() & 0xfffff;
        for (int i = 0; i < 16; i++) pcm.ram2[s][i] = (uint16_t)rng.next();
        // keep the phase increments modest so voices sweep rather than alias
        pcm.ram2[s][0] = (uint16_t)((mode == 8 || mode == 9) ? (rng.next() >> (s % 5))
                                                             : (rng.next() & 0x3fff));
    }
    for (int i = 0; i < 0x4000; i++) pcm.eram[i] = (uint16_t)rng.next();

    // Config, via the real register write path.
    int slots_minus_1 = 31;                   // reg_slots = 32
    if (mode == 5) slots_minus_1 = 11;
    if (mode == 6) slots_minus_1 = 0;
    if (mode == 7) slots_minus_1 = 27;
    PCM_Write(pcm, 0x3d, (uint8_t)slots_minus_1);
    PCM_Write(pcm, 0x3c, (mode == 4) ? 0xc0 : 0xc3);

    if (mode == 2 || mode == 8) { pcm.voice_mask = 0xffffffff; pcm.voice_mask_pending = 0xffffffff; }
    else if (mode == 3) { pcm.voice_mask = 0;          pcm.voice_mask_pending = 0; }
    else                { pcm.voice_mask = rng.next(); pcm.voice_mask_pending = rng.next(); }

    int ticks = (mode == 2 || mode == 3 || mode == 8) ? 4000 : 20000;
    if (const char* e = getenv("PCM_TICKS")) ticks = atoi(e);

    uint64_t target = 0;
    for (int t = 0; t < ticks; t++)
    {
        // Poke registers the way the MCU would, in the mixed modes.
        if (mode != 2 && mode != 3 && mode != 8 && (t % 7) == 0)
        {
            uint32_t r = rng.next();
            uint32_t addr = r & 0x3f;
            // leave 0x3d alone so reg_slots stays 32
            if (addr != 0x3d) PCM_Write(pcm, addr, (uint8_t)(r >> 8));
        }
        if (mode != 2 && mode != 3 && mode != 8 && (t % 53) == 0)
        {
            pcm.voice_mask         = rng.next();
            pcm.voice_mask_pending = rng.next();
        }
        // Clear a pending IRQ from time to time so the IRQ path can re-fire.
        if ((t % 11) == 0) pcm.irq_assert = false;

        target += 825;
        PCM_Update(pcm, target);

        if (quiet) continue;
        g_state_hash = 1469598103934665603ull;
        hash_pcm(g_state_hash, pcm);
        printf("%6d %016llx %016llx %016llx\n", t,
               (unsigned long long)g_state_hash,
               (unsigned long long)g_post_hash,
               (unsigned long long)g_int_hash);
    }

    if (quiet) { printf("quiet %llu\n", (unsigned long long)pcm.cycles); return 0; }
    if (getenv("PCM_TERSE")) return 0;   // per-tick digests only, skip the raw dump
    // Final full dump: raw state, not just a digest.
    printf("--- ram1\n");
    for (int s = 0; s < 32; s++)
    {
        printf("%2d:", s);
        for (int i = 0; i < 8; i++) printf(" %08x", pcm.ram1[s][i]);
        printf("\n");
    }
    printf("--- ram2\n");
    for (int s = 0; s < 32; s++)
    {
        printf("%2d:", s);
        for (int i = 0; i < 16; i++) printf(" %04x", pcm.ram2[s][i]);
        printf("\n");
    }
    printf("--- eram\n");
    for (int i = 0; i < 0x4000; i += 8)
    {
        printf("%04x:", i);
        for (int j = 0; j < 8; j++) printf(" %04x", pcm.eram[i + j]);
        printf("\n");
    }
    printf("--- scalars %llu %08x %08x %08x %08x %04x %02x %02x %02x %02x %d %d %d %d %d %d %d\n",
           (unsigned long long)pcm.cycles, pcm.voice_mask, pcm.voice_mask_pending,
           pcm.write_latch, pcm.read_latch, pcm.tv_counter, pcm.wave_byte_latch,
           pcm.select_channel, pcm.config_reg_3c, pcm.config_reg_3d,
           (int)pcm.irq_channel, (int)pcm.irq_assert, (int)pcm.voice_mask_updating,
           (int)pcm.nfs, pcm.accum_l, pcm.accum_r, pcm.rcsum[0]);
    return 0;
}
