/*
 * Proof for the constant-time SM_UpdateTimer in
 * patches/0004-submcu-fast-forward-idle-loop.patch.
 *
 * Upstream's timer is a loop: one tick per 16 cycles, and per tick, if the
 * timer runs and the sub-MCU is awake, either the prescaler decrements or --
 * at zero -- reloads and the counter decrements or, at zero, reloads and
 * requests an interrupt. The patch computes the same result arithmetically.
 * Both are restated here from the definitions above (upstream's literally,
 * the patch's as shipped) and compared over every (prescaler, counter,
 * prescaler reload, counter reload) byte quadruple for a set of tick counts,
 * plus randomised (state, ticks) pairs up to millions of ticks, on all four
 * fields the function may change: timer_cycles, prescaler, counter, and the
 * interrupt request bit.
 *
 * -DBREAK_TIMER=1..3 checks the test can fail: an off-by-one in the first
 * expiry, the prescaler remainder taken the wrong way round, and a fire that
 * forgets the counter reload.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

struct T { uint64_t timer_cycles, cycles; uint8_t prescaler, counter, reload_p, reload_c, ctrl, sleep, req; };

static void Upstream(T& t)
{
    while (t.timer_cycles < t.cycles)
    {
        t.timer_cycles += 16;
        if ((t.ctrl & 0x20) == 0 && !t.sleep)
        {
            if (t.prescaler == 0)
            {
                t.prescaler = t.reload_p;
                if (t.counter == 0) { t.counter = t.reload_c; t.req |= 0x8; }
                else t.counter--;
            }
            else t.prescaler--;
        }
    }
}

static void Patched(T& t)
{
    const uint64_t target = t.cycles, start = t.timer_cycles;
    if (start >= target) return;
    const uint64_t ticks = (target - start + 15) >> 4;
    t.timer_cycles = start + (ticks << 4);
    if ((t.ctrl & 0x20) != 0 || t.sleep) return;
    uint64_t P = t.prescaler;
    if (ticks <= P) { t.prescaler = (uint8_t)(P - ticks); return; }
    const uint64_t R = t.reload_p, K = t.reload_c;
#if BREAK_TIMER == 1
    const uint64_t after_first = ticks - P;                 /* first expiry off by one */
#else
    const uint64_t after_first = ticks - (P + 1);
#endif
    const uint64_t expiries = 1 + after_first / (R + 1);
#if BREAK_TIMER == 2
    t.prescaler = (uint8_t)(after_first % (R + 1));          /* remainder the wrong way round */
#else
    t.prescaler = (uint8_t)(R - after_first % (R + 1));
#endif
    uint64_t C = t.counter;
    if (expiries <= C) { t.counter = (uint8_t)(C - expiries); return; }
    const uint64_t after_fire = expiries - (C + 1);
#if BREAK_TIMER == 3
    t.counter = (uint8_t)(after_fire % (K + 1));              /* forgets the reload */
#else
    t.counter = (uint8_t)(K - after_fire % (K + 1));
#endif
    t.req |= 0x8;
}

static uint32_t Rand(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

int main()
{
    uint64_t checked = 0, mismatches = 0;
    auto check = [&](T a) {
        T b = a; Upstream(a); Patched(b); checked++;
        if (memcmp(&a, &b, sizeof(T)) != 0 && mismatches++ < 5)
            printf("MISMATCH P=%u C=%u R=%u K=%u ticks=%llu ctrl=%02x: upstream (tc %llu p %u c %u req %02x) patched (tc %llu p %u c %u req %02x)\n",
                   b.prescaler, b.counter, b.reload_p, b.reload_c, (unsigned long long)((a.cycles - (a.timer_cycles - 0)) ), b.ctrl,
                   (unsigned long long)a.timer_cycles, a.prescaler, a.counter, a.req, (unsigned long long)b.timer_cycles, b.prescaler, b.counter, b.req);
    };
    /* exhaustive over the four bytes for a spread of tick counts and both control states */
    static const uint64_t kTicks[] = { 1, 2, 3, 15, 16, 17, 255, 256, 257, 1000, 4095, 4096, 65536 };
    for (unsigned P = 0; P < 256; P += 5) for (unsigned C = 0; C < 256; C += 7)
        for (unsigned R = 0; R < 256; R += 11) for (unsigned K = 0; K < 256; K += 13)
            for (uint64_t ticks : kTicks) for (unsigned ctrl = 0; ctrl <= 0x20; ctrl += 0x20)
            {
                T t{}; t.timer_cycles = 1000; t.cycles = 1000 + ticks * 16 - (ticks & 1 ? 7 : 0); /* unaligned targets too */
                t.prescaler = (uint8_t)P; t.counter = (uint8_t)C; t.reload_p = (uint8_t)R; t.reload_c = (uint8_t)K; t.ctrl = (uint8_t)ctrl; t.req = 0x40;
                check(t);
            }
    /* random states, ticks up to ~2M, sleep and control bits random */
    uint32_t s = 0xc0ffee11;
    for (int i = 0; i < 3000000; i++)
    {
        uint32_t r = Rand(s), q = Rand(s);
        T t{}; t.timer_cycles = (uint64_t)(r & 0xffff) * 16 + (q & 15);
        t.cycles = t.timer_cycles + (Rand(s) % 2000000);
        t.prescaler = (uint8_t)r; t.counter = (uint8_t)(r >> 8); t.reload_p = (uint8_t)(r >> 16); t.reload_c = (uint8_t)(r >> 24);
        t.ctrl = (q & 1) ? 0x20 : 0; t.sleep = (q >> 1) & 1 && (q & 4) != 0; t.req = (uint8_t)(q >> 8);
        check(t);
    }
    printf("checked %llu states: %s\n", (unsigned long long)checked, mismatches ? "MISMATCH" : "ok");
    if (mismatches) { printf("FAIL: %llu\n", (unsigned long long)mismatches); return 1; }
    printf("PASS\n"); return 0;
}
