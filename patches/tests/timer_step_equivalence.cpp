/*
 * Differential check for
 * patches/0001-mcu_timer-skip-uneventful-cycles.patch.
 *
 * TIMER_Clock() advances timer.cycles one at a time, and both clocking helpers
 * return immediately unless timer.cycles sits on their divider edge. The patch
 * skips the cycles where nothing can happen. This program models the loop's
 * visiting behaviour and proves the patched form:
 *
 *   1. clocks the timers on exactly the same set of cycle values, and
 *   2. leaves timer.cycles at exactly the same final value.
 *
 * Point 2 is not cosmetic. timer.cycles persists between calls, so a patched
 * loop that overshoots would consume cycles that a later, finer divider setting
 * would have fired on. An earlier draft of this patch had that bug.
 *
 * Every divider combination is checked (4*4*4*8 per step-table variant, both
 * variants), against a range of starting offsets and durations. No ROMs needed.
 *
 *   g++ -O2 -std=c++17 -o timer_step_equivalence timer_step_equivalence.cpp
 *   ./timer_step_equivalence
 */
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Config {
    uint64_t frt[3];  // divider masks
    uint64_t tmr;     // divider mask, 0 = never steps
};

// Cycles on which at least one timer would do something.
bool Fires(const Config &config, uint64_t cycles)
{
    for (int i = 0; i < 3; i++)
    {
        if ((cycles & config.frt[i]) == 0)
            return true;
    }
    return config.tmr != 0 && (cycles & config.tmr) == 0;
}

// The loop as it stands upstream.
uint64_t Original(const Config &config, uint64_t start, uint64_t target,
                  std::vector<uint64_t> &fired)
{
    uint64_t cycles = start;
    while (cycles * 2 < target)
    {
        if (Fires(config, cycles))
            fired.push_back(cycles);
        ++cycles;
    }
    return cycles;
}

// The loop as the patch leaves it.
uint64_t Patched(const Config &config, uint64_t start, uint64_t target,
                 std::vector<uint64_t> &fired)
{
    uint64_t period = ~(uint64_t)0;
    for (int i = 0; i < 3; i++)
    {
        if (config.frt[i] + 1 < period)
            period = config.frt[i] + 1;
    }
    if (config.tmr != 0 && config.tmr + 1 < period)
        period = config.tmr + 1;

    const uint64_t limit = (target + 1) / 2;

    uint64_t cycles = start;
    while (cycles < limit)
    {
        if (Fires(config, cycles))
            fired.push_back(cycles);
        const uint64_t next = cycles + period - (cycles & (period - 1));
        cycles = next < limit ? next : limit;
    }
    return cycles;
}

} // namespace

int main()
{
    // Both step tables from the core. Every entry is 2^k - 1, which is what
    // makes "smallest period divides all the others" hold.
    const uint64_t frt_generic[4] = {3, 7, 31, 1};
    const uint64_t frt_mk1[4]     = {3, 7, 31, 3};
    const uint64_t tmr_generic[8] = {0, 7, 63, 1023, 0, 1, 1, 1};
    const uint64_t tmr_mk1[8]     = {0, 7, 63, 1023, 0, 3, 3, 3};

    long configs = 0, cases = 0, failures = 0;

    for (int variant = 0; variant < 2; variant++)
    {
        const uint64_t *frt = variant ? frt_mk1 : frt_generic;
        const uint64_t *tmr = variant ? tmr_mk1 : tmr_generic;

        for (int a = 0; a < 4; a++)
        for (int b = 0; b < 4; b++)
        for (int c = 0; c < 4; c++)
        for (int t = 0; t < 8; t++)
        {
            const Config config{{frt[a], frt[b], frt[c]}, tmr[t]};
            configs++;

            // Start on and off every alignment boundary of interest, and run
            // for durations that both stop mid-period and land exactly on one.
            for (uint64_t start = 0; start < 70; start++)
            for (uint64_t span = 0; span < 130; span++)
            {
                const uint64_t target = start * 2 + span;

                std::vector<uint64_t> fired_original, fired_patched;
                const uint64_t end_original = Original(config, start, target, fired_original);
                const uint64_t end_patched  = Patched(config, start, target, fired_patched);
                cases++;

                if (fired_original != fired_patched || end_original != end_patched)
                {
                    if (failures++ < 5)
                    {
                        printf("MISMATCH masks=%llu/%llu/%llu tmr=%llu start=%llu target=%llu\n",
                               (unsigned long long)config.frt[0], (unsigned long long)config.frt[1],
                               (unsigned long long)config.frt[2], (unsigned long long)config.tmr,
                               (unsigned long long)start, (unsigned long long)target);
                        printf("  fired: %zu vs %zu, end: %llu vs %llu\n",
                               fired_original.size(), fired_patched.size(),
                               (unsigned long long)end_original, (unsigned long long)end_patched);
                    }
                }
            }
        }
    }

    printf("%ld divider configurations, %ld loop cases\n", configs, cases);
    if (failures)
    {
        printf("\nFAILED: %ld mismatches\n", failures);
        return 1;
    }
    printf("\nPASS: identical firing cycles and identical final timer.cycles\n");
    return 0;
}
