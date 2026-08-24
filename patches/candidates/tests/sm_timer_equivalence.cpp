/*
 * Differential check for the SM_UpdateTimer rewrite
 * (0001-submcu-collapse-the-sub-MCU-timer-loop.patch).
 *
 * SM_UpdateTimer() walks sm.timer_cycles up to sm.cycles in steps of 16,
 * re-reading the enable bit, the sleep flag and both reload registers on every
 * step, and running a two-level prescaler/counter countdown. The patch reads
 * the invariants once, closes the two uneventful cases in constant time, and
 * raises the timer interrupt once at the end instead of once per firing tick.
 *
 * This program proves, on the model of that loop:
 *
 *   1. the final (timer_cycles, timer_prescaler, timer_counter, INT_REQUEST)
 *      is bit-identical;
 *   2. timer_cycles lands on exactly the original's exit point -- never past
 *      it.  timer_cycles persists across calls, so an overshoot would silently
 *      eat ticks that a later prescaler setting would have made live.  This is
 *      the bug an earlier draft of patches/0001 had;
 *   3. the *index of the first tick that raises the interrupt* agrees.  The
 *      patch collapses N `|= 0x8` stores into one, which is only sound if the
 *      flag is idempotent AND no tick in between observes it.  Idempotency of
 *      `|= 0x8` is checked here; the "no observer" half is a property of the
 *      source: the loop body makes no calls, never reads INT_REQUEST back, and
 *      the three registers it does read (PRESCALER 0x1d, TIMER 0x1e,
 *      TIMER_CTRL 0x1f) are all distinct from INT_REQUEST (0x1c).  Checking
 *      the first-fire index means a hypothetical observer placed after the
 *      loop cannot tell the two apart either.
 *
 * Coverage: exhaustive over small prescaler/counter/reload/tick combinations,
 * exhaustive over the enable/sleep cross product, a large pseudo-random sweep
 * over the full 8-bit register ranges with tick counts up to 4096 and a few
 * enormous ones, and unaligned timer_cycles starts.  No ROMs needed.
 *
 *   g++ -O2 -std=c++17 -o sm_timer_equivalence sm_timer_equivalence.cpp
 *   ./sm_timer_equivalence                    # the shipped rewrite; must pass
 *   ./sm_timer_equivalence --variant=hoist    # rejected alternative; must pass
 *   ./sm_timer_equivalence --variant=closed   # rejected alternative; must pass
 *   ./sm_timer_equivalence --broken=1         # must FAIL
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

// Only the fields SM_UpdateTimer can see.  device_mode is modelled as the four
// registers the function touches, so an accidental write to the wrong one shows
// up as a mismatch.
struct State {
    uint64_t cycles = 0;         // sm.cycles          (read only)
    uint64_t timer_cycles = 0;   // sm.timer_cycles
    uint8_t  prescaler = 0;      // sm.timer_prescaler
    uint8_t  counter = 0;        // sm.timer_counter
    uint8_t  sleep = 0;          // sm.sleep           (read only)
    uint8_t  int_request = 0;    // device_mode[0x1c]
    uint8_t  presc_reload = 0;   // device_mode[0x1d]  (read only)
    uint8_t  timer_reload = 0;   // device_mode[0x1e]  (read only)
    uint8_t  timer_ctrl = 0;     // device_mode[0x1f]  (read only)

    bool operator==(const State &o) const
    {
        return cycles == o.cycles && timer_cycles == o.timer_cycles
            && prescaler == o.prescaler && counter == o.counter
            && sleep == o.sleep && int_request == o.int_request
            && presc_reload == o.presc_reload && timer_reload == o.timer_reload
            && timer_ctrl == o.timer_ctrl;
    }
};

// Which broken variant to build, 0 = the real patch.  See Patched().
int g_broken = 0;
// 1 = the shipped rewrite (tick count + prescaler shortcut) and the default.
// 0 and 2 are the two rewrites that were measured and rejected; they are kept
// here because the report quotes their numbers and they must be proved too.
int g_variant = 1;

// -------------------------------------------------------------------------
// The loop exactly as it stands in the core.  first_fire is the 1-based index
// of the tick that first set INT_REQUEST bit 3, or 0 if it never did.
// -------------------------------------------------------------------------
void Original(State &sm, uint64_t &first_fire)
{
    uint64_t tick = 0;
    first_fire = 0;

    while (sm.timer_cycles < sm.cycles)
    {
        ++tick;
        if ((sm.timer_ctrl & 0x20) == 0 && !sm.sleep)
        {
            if (sm.prescaler == 0)
            {
                sm.prescaler = sm.presc_reload;

                if (sm.counter == 0)
                {
                    sm.counter = sm.timer_reload;
                    sm.int_request |= 0x8;
                    if (first_fire == 0)
                        first_fire = tick;
                }
                else
                    sm.counter--;
            }
            else
                sm.prescaler--;
        }
        sm.timer_cycles += 16;
    }
}

// -------------------------------------------------------------------------
// The loop as the patch leaves it.
// -------------------------------------------------------------------------
void Patched(State &sm, uint64_t &first_fire)
{
    first_fire = 0;

    const uint64_t target = sm.cycles;
    const uint64_t start = sm.timer_cycles;

    if (start >= target)
        return;

    // The original adds 16 until timer_cycles >= target, so it runs
    // ceil((target - start) / 16) times and stops exactly there.
    uint64_t ticks = (target - start + 15) >> 4;
    if (g_broken == 1)
        ticks = (target - start) >> 4;            // loses the partial tick
    sm.timer_cycles = start + (ticks << 4);
    if (g_broken == 2)
        sm.timer_cycles = target + 16;            // overshoots the exit point

    if ((sm.timer_ctrl & 0x20) != 0 || sm.sleep)
        return;

    uint32_t prescaler = sm.prescaler;

    // Fewer ticks than the prescaler has left: it never reaches zero, so the
    // counter cannot move and no interrupt can be raised.
    bool shortcut = (ticks <= prescaler);
    if (g_broken == 3)
        shortcut = (ticks <= prescaler + 1u);     // off-by-one on the shortcut
    if (shortcut)
    {
        sm.prescaler = (uint8_t)(prescaler - ticks);
        return;
    }

    const uint32_t presc_reload = sm.presc_reload;
    const uint32_t timer_reload = sm.timer_reload;
    uint32_t counter = sm.counter;
    uint64_t fired = 0;
    uint64_t remaining = ticks;
    uint64_t tick = 0;

    do
    {
        ++tick;
        if (prescaler == 0)
        {
            prescaler = presc_reload;

            if (counter == 0)
            {
                counter = timer_reload;
                if (fired == 0)
                    fired = tick;
            }
            else
                counter--;
        }
        else
            prescaler--;
    } while (--remaining);

    sm.prescaler = (uint8_t)prescaler;
    if (g_broken != 5)                            // 5: forgets the counter write-back
        sm.counter = (uint8_t)counter;
    if (g_broken == 4)
    {
        sm.int_request |= 0x8;                    // raises even when it never fired
        first_fire = fired;
        return;
    }
    if (fired)
    {
        sm.int_request |= 0x8;
        first_fire = fired;
    }
}


// -------------------------------------------------------------------------
// Alternative rewrite: full closed form, two divisions, no loop at all.
// Measured but not shipped -- see the report.  --variant=closed checks it.
// -------------------------------------------------------------------------
void PatchedClosed(State &sm, uint64_t &first_fire)
{
    first_fire = 0;

    const uint64_t target = sm.cycles;
    const uint64_t start = sm.timer_cycles;

    if (start >= target)
        return;

    const uint64_t ticks = (target - start + 15) >> 4;
    sm.timer_cycles = start + (ticks << 4);

    if ((sm.timer_ctrl & 0x20) != 0 || sm.sleep)
        return;

    const uint64_t prescaler = sm.prescaler;
    if (ticks <= prescaler)
    {
        sm.prescaler = (uint8_t)(prescaler - ticks);
        return;
    }

    const uint64_t presc_reload = sm.presc_reload;
    const uint64_t p_period = presc_reload + 1;
    const uint64_t p_rem = ticks - prescaler;
    const uint64_t hits = (p_rem + presc_reload) / p_period;
    sm.prescaler = (uint8_t)(presc_reload - (p_rem - ((hits - 1) * p_period + 1)));

    const uint64_t counter = sm.counter;
    if (hits <= counter)
    {
        sm.counter = (uint8_t)(counter - hits);
        return;
    }

    const uint64_t timer_reload = sm.timer_reload;
    const uint64_t c_period = timer_reload + 1;
    const uint64_t c_rem = hits - counter;
    const uint64_t fires = (c_rem + timer_reload) / c_period;
    sm.counter = (uint8_t)(timer_reload - (c_rem - ((fires - 1) * c_period + 1)));
    sm.int_request |= 0x8;
    // The tick that first fires is the (counter+1)-th prescaler hit, i.e.
    // tick prescaler + counter * p_period + 1.
    first_fire = prescaler + counter * p_period + 1;
}

// -------------------------------------------------------------------------


// -------------------------------------------------------------------------
// The shipped rewrite: read the invariants once, run the countdown in
// registers, store back once.  --variant=hoist (the default) checks this one.
// -------------------------------------------------------------------------
void PatchedHoist(State &sm, uint64_t &first_fire)
{
    first_fire = 0;

    uint64_t timer_cycles = sm.timer_cycles;
    const uint64_t target = sm.cycles;

    if (timer_cycles >= target)
        return;

    if ((sm.timer_ctrl & 0x20) != 0 || sm.sleep)
    {
        sm.timer_cycles = timer_cycles + ((target - timer_cycles + 15) & ~(uint64_t)15);
        if (g_broken == 6)
            sm.timer_cycles = target;          // lands short of the exit point
        return;
    }

    const uint32_t presc_reload = sm.presc_reload;
    const uint32_t timer_reload = sm.timer_reload;
    uint32_t prescaler = sm.prescaler;
    uint32_t counter = sm.counter;
    uint32_t fired = 0;
    uint64_t tick = 0;

    do
    {
        ++tick;
        if (prescaler == 0)
        {
            prescaler = presc_reload;

            if (counter == 0)
            {
                counter = timer_reload;
                if (!fired)
                    fired = (uint32_t)tick;
            }
            else
                counter--;
        }
        else
            prescaler--;

        timer_cycles += 16;
    } while (g_broken == 7 ? timer_cycles <= target   // runs one tick too many
                           : timer_cycles < target);

    sm.timer_cycles = timer_cycles;
    sm.prescaler = (uint8_t)prescaler;
    if (g_broken != 5)
        sm.counter = (uint8_t)counter;
    if (g_broken == 4)
    {
        sm.int_request |= 0x8;
        first_fire = fired;
        return;
    }
    if (fired)
    {
        sm.int_request |= 0x8;
        first_fire = fired;
    }
}

unsigned long long g_cases = 0;
unsigned long long g_fail = 0;

void Check(const State &in)
{
    State a = in, b = in;
    uint64_t fa = 0, fb = 0;
    Original(a, fa);
    if (g_variant == 2) PatchedClosed(b, fb);
    else if (g_variant == 1) Patched(b, fb);
    else PatchedHoist(b, fb);

    if (a == b && fa == fb)
    {
        ++g_cases;
        return;
    }

    if (g_fail < 10)
    {
        printf("MISMATCH cycles=%llu timer_cycles=%llu presc=%u cnt=%u "
               "sleep=%u ireq=0x%02x preload=%u treload=%u ctrl=0x%02x\n",
               (unsigned long long)in.cycles, (unsigned long long)in.timer_cycles,
               in.prescaler, in.counter, in.sleep, in.int_request,
               in.presc_reload, in.timer_reload, in.timer_ctrl);
        printf("   orig: tc=%llu p=%u c=%u ireq=0x%02x first_fire=%llu\n",
               (unsigned long long)a.timer_cycles, a.prescaler, a.counter,
               a.int_request, (unsigned long long)fa);
        printf("   patch:tc=%llu p=%u c=%u ireq=0x%02x first_fire=%llu\n",
               (unsigned long long)b.timer_cycles, b.prescaler, b.counter,
               b.int_request, (unsigned long long)fb);
    }
    ++g_fail;
    ++g_cases;
}

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint64_t Rand()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--broken=", 9) == 0)
            g_broken = atoi(argv[i] + 9);
        else if (strcmp(argv[i], "--variant=closed") == 0)
            g_variant = 2;
        else if (strcmp(argv[i], "--variant=ticks") == 0)
            g_variant = 1;
        else if (strcmp(argv[i], "--variant=hoist") == 0)
            g_variant = 0;
    }
    if (g_broken)
        printf("running BROKEN variant %d -- this run is expected to FAIL\n", g_broken);

    // 1. Exhaustive over small state.  Ticks 0..12 covers the three ticks per
    //    call that SM_Update actually produces plus plenty of slack, and the
    //    unaligned starts cover a timer_cycles that is not a multiple of 16.
    for (unsigned presc = 0; presc <= 8; presc++)
    for (unsigned cnt = 0; cnt <= 8; cnt++)
    for (unsigned pre = 0; pre <= 8; pre++)
    for (unsigned tre = 0; tre <= 8; tre++)
    for (unsigned delta = 0; delta <= 12 * 16 + 3; delta++)
    for (unsigned off = 0; off < 2; off++)
    {
        State s;
        s.timer_cycles = off ? 7 : 0;
        s.cycles = s.timer_cycles + delta;
        s.prescaler = (uint8_t)presc;
        s.counter = (uint8_t)cnt;
        s.presc_reload = (uint8_t)pre;
        s.timer_reload = (uint8_t)tre;
        Check(s);
    }

    // 2. Enable bit / sleep flag / pre-set interrupt bits cross product.
    for (unsigned ctrl = 0; ctrl < 256; ctrl++)
    for (unsigned sleep = 0; sleep < 2; sleep++)
    for (unsigned ireq = 0; ireq < 256; ireq++)
    for (unsigned delta = 0; delta <= 5 * 16; delta += 16)
    {
        State s;
        s.cycles = delta;
        s.prescaler = 0;
        s.counter = 0;
        s.presc_reload = 3;
        s.timer_reload = 2;
        s.timer_ctrl = (uint8_t)ctrl;
        s.sleep = (uint8_t)sleep;
        s.int_request = (uint8_t)ireq;
        Check(s);
    }

    // 3. Random sweep over the full 8-bit ranges, tick counts up to 4096, and
    //    starting points far from zero.
    for (unsigned long long i = 0; i < 3000000ull; i++)
    {
        State s;
        uint64_t r = Rand();
        s.timer_cycles = (r & 0xffffff) * 16 + (r >> 60);
        s.cycles = s.timer_cycles + (Rand() % (4096ull * 16));
        s.prescaler = (uint8_t)Rand();
        s.counter = (uint8_t)Rand();
        s.presc_reload = (uint8_t)Rand();
        s.timer_reload = (uint8_t)Rand();
        s.int_request = (uint8_t)Rand();
        s.timer_ctrl = (uint8_t)((Rand() % 4) == 0 ? 0x20 : 0x00);
        s.sleep = (uint8_t)((Rand() % 8) == 0);
        Check(s);
    }

    // 4. A handful of very long catch-ups, which is what a closed form has to
    //    get right and where an off-by-one in the tick count shows up.
    for (unsigned presc = 0; presc < 256; presc += 37)
    for (unsigned cnt = 0; cnt < 256; cnt += 41)
    for (unsigned pre = 0; pre < 256; pre += 43)
    for (unsigned tre = 0; tre < 256; tre += 47)
    for (unsigned long long n : {1ull, 2ull, 3ull, 255ull, 256ull, 257ull,
                                 65535ull, 65536ull, 1000003ull})
    {
        State s;
        s.timer_cycles = 16 * 12345;
        s.cycles = s.timer_cycles + n * 16;
        s.prescaler = (uint8_t)presc;
        s.counter = (uint8_t)cnt;
        s.presc_reload = (uint8_t)pre;
        s.timer_reload = (uint8_t)tre;
        Check(s);
    }

    printf("%llu cases, %llu mismatches\n", g_cases, g_fail);
    if (g_fail)
    {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
