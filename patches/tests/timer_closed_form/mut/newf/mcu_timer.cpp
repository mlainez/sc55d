/*
 * Copyright (C) 2021, 2024 nukeykt
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */
#include "mcu_timer.h"
#include "mcu.h"
#include <bit>
#include <cstdint>

enum TMR_TCR_Bits : uint8_t
{
    TMR_TCR_CKS0  = 1 << 0, // Clock Select 0
    TMR_TCR_CKS1  = 1 << 1, // Clock Select 1
    TMR_TCR_CKS2  = 1 << 2, // Clock Select 2
    TMR_TCR_CCLR0 = 1 << 3, // Counter Clear 0
    TMR_TCR_CCLR1 = 1 << 4, // Counter Clear 1
    TMR_TCR_OVIE  = 1 << 5, // Timer Overflow Interrupt Enable
    TMR_TCR_CMIEA = 1 << 6, // Compare-match Interrupt Enable A
    TMR_TCR_CMIEB = 1 << 7, // Compare-match Interrupt Enable B
};

enum TMR_TCSR_Bits : uint8_t
{
    TMR_TCSR_OS0  = 1 << 0, // Output Select 0
    TMR_TCSR_OS1  = 1 << 1, // Output Select 1
    TMR_TCSR_OS2  = 1 << 2, // Output Select 2
    TMR_TCSR_OS3  = 1 << 3, // Output Select 3
    TMR_TCSR_BIT4 = 1 << 4, // Reserved
    TMR_TCSR_OVF  = 1 << 5, // Timer Overflow Flag
    TMR_TCSR_CMFA = 1 << 6, // Compare-Match Flag A
    TMR_TCSR_CMFB = 1 << 7, // Compare-Match Flag B
};

enum FRT_TCR_Bits : uint8_t
{
    FRT_TCR_CKS0  = 1 << 0, // Clock Select 0
    FRT_TCR_CKS1  = 1 << 1, // Clock Select 1
    FRT_TCR_OEA   = 1 << 2, // Output Enable A
    FRT_TCR_OEB   = 1 << 3, // Output Enable B
    FRT_TCR_OVIE  = 1 << 4, // Timer overflow Interrupt Enable
    FRT_TCR_OCIEA = 1 << 5, // Output Compare Interrupt Enable A
    FRT_TCR_OCIEB = 1 << 6, // Output Compare Interrupt Enable B
    FRT_TCR_ICIE  = 1 << 7, // Input Capture Interrupt Enable
};

enum FRT_TCSR_Bits : uint8_t
{
    FRT_TCSR_CCLRA = 1 << 0, // Counter Clear A
    FRT_TCSR_IEDG  = 1 << 1, // Input Edge Select
    FRT_TCSR_OLVLA = 1 << 2, // Output Level A
    FRT_TCSR_OLVLB = 1 << 3, // Output Level B
    FRT_TCSR_OVF   = 1 << 4, // Timer Overflow Flag
    FRT_TCSR_OCFA  = 1 << 5, // Output Compare Flag A
    FRT_TCSR_OCFB  = 1 << 6, // Output Compare Flag B
    FRT_TCSR_ICF   = 1 << 7, // Input Capture Flag
};

// Values are byte offsets from start of FRTs in memory (ffa0, ffb0, ffc0)
enum FRT_Field_Offset : uint8_t
{
    REG_TCR   = 0x00,
    REG_TCSR  = 0x01,
    REG_FRCH  = 0x02,
    REG_FRCL  = 0x03,
    REG_OCRAH = 0x04,
    REG_OCRAL = 0x05,
    REG_OCRBH = 0x06,
    REG_OCRBL = 0x07,
    REG_ICRH  = 0x08,
    REG_ICRL  = 0x09,
};

// Bring every timer up to date with cycle `limit` (exclusive) and work out
// when the next observable tick is. Defined further down, next to the rest of
// the clocking code; declared here because every entry point that can observe
// timer state has to call it first.
static void TIMER_Sync(mcu_timer_t& timer, uint64_t limit);

void TIMER_Init(mcu_timer_t& timer, mcu_t& mcu)
{
    timer.mcu = &mcu;
}

void TIMER_Reset(mcu_timer_t& timer)
{
    // Anything the deferred ticks would have done still has to happen: they are
    // in the past, and the original had already applied them by now.
    if (timer.synced < timer.cycles)
        TIMER_Sync(timer, timer.cycles);

    for (int i = 0; i < 3; ++i)
    {
        timer.frt[i] = {
            .tcr       = 0,
            .tcsr      = 0,
            .frc       = 0,
            .ocra      = 0xffff,
            .ocrb      = 0xffff,
            .icr       = 0,
            .status_rd = 0,
        };
    }
    timer.tmr = {
        .tcr       = 0,
        .tcsr      = TMR_TCSR_BIT4,
        .tcora     = 0xff,
        .tcorb     = 0xff,
        .tcnt      = 0,
        .status_rd = 0,
    };
    // timer.cycles deliberately survives a reset, as it always has.
    timer.synced     = timer.cycles;
    timer.next_event = 0;
}

void TIMER_Write(mcu_timer_t& timer, uint32_t address, uint8_t data)
{
    uint32_t t = (address >> 4) - 1;
    if (t > 2)
        return;
    // The MCU is about to observe and change timer state, so every tick that
    // has already happened must be applied before it does.
    TIMER_Sync(timer, timer.cycles);
    frt_t& frt = timer.frt[t];

    address &= 0x0f;
    switch (address)
    {
    case REG_TCR:
        frt.tcr = data;
        break;
    case REG_TCSR:
        frt.tcsr &= ~0xf;
        frt.tcsr |= data & 0xf;
        if ((data & FRT_TCSR_OVF) == 0 && (frt.status_rd & FRT_TCSR_OVF) != 0)
        {
            frt.tcsr      &= ~FRT_TCSR_OVF;
            frt.status_rd &= ~FRT_TCSR_OVF;
            MCU_Interrupt_SetRequest(*timer.mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_FOVI + t * 4), 0);
        }
        if ((data & FRT_TCSR_OCFA) == 0 && (frt.status_rd & FRT_TCSR_OCFA) != 0)
        {
            frt.tcsr      &= ~FRT_TCSR_OCFA;
            frt.status_rd &= ~FRT_TCSR_OCFA;
            MCU_Interrupt_SetRequest(*timer.mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_OCIA + t * 4), 0);
        }
        if ((data & FRT_TCSR_OCFB) == 0 && (frt.status_rd & FRT_TCSR_OCFB) != 0)
        {
            frt.tcsr      &= ~FRT_TCSR_OCFB;
            frt.status_rd &= ~FRT_TCSR_OCFB;
            MCU_Interrupt_SetRequest(*timer.mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_OCIB + t * 4), 0);
        }
        break;
    case REG_FRCH:
    case REG_OCRAH:
    case REG_OCRBH:
    case REG_ICRH:
        timer.tempreg = data;
        break;
    case REG_FRCL:
        frt.frc = (uint16_t)((timer.tempreg << 8) | data);
        break;
    case REG_OCRAL:
        frt.ocra = (uint16_t)((timer.tempreg << 8) | data);
        break;
    case REG_OCRBL:
        frt.ocrb = (uint16_t)((timer.tempreg << 8) | data);
        break;
    case REG_ICRL:
        frt.icr = (uint16_t)((timer.tempreg << 8) | data);
        break;
    }
    // Registers changed under us; the schedule computed at the top of this
    // function is stale. 0 forces the next TIMER_Clock() to recompute it.
    timer.next_event = 0;
}

uint8_t TIMER_Read(mcu_timer_t& timer, uint32_t address)
{
    uint32_t t = (address >> 4) - 1;
    if (t > 2)
        return 0xff;
    TIMER_Sync(timer, timer.cycles);
    frt_t& frt = timer.frt[t];

    address &= 0x0f;
    switch (address)
    {
    case REG_TCR:
        return frt.tcr;
    case REG_TCSR: {
        uint8_t ret    = frt.tcsr;
        frt.status_rd |= frt.tcsr & 0xf0;
        // frt.status_rd |= 0xf0;
        return ret;
    }
    case REG_FRCH:
        timer.tempreg = (uint8_t)frt.frc;
        return (uint8_t)(frt.frc >> 8);
    case REG_OCRAH:
        timer.tempreg = (uint8_t)frt.ocra;
        return (uint8_t)(frt.ocra >> 8);
    case REG_OCRBH:
        timer.tempreg = (uint8_t)frt.ocrb;
        return (uint8_t)(frt.ocrb >> 8);
    case REG_ICRH:
        timer.tempreg = (uint8_t)frt.icr;
        return (uint8_t)(frt.icr >> 8);
    case REG_FRCL:
    case REG_OCRAL:
    case REG_OCRBL:
    case REG_ICRL:
        return timer.tempreg;
    }
    return 0xff;
}

void TIMER2_Write(mcu_timer_t& timer, uint32_t address, uint8_t data)
{
    TIMER_Sync(timer, timer.cycles);
    tmr_t& tmr = timer.tmr;

    switch (address)
    {
    case DEV_TMR_TCR:
        tmr.tcr = data;
        break;
    case DEV_TMR_TCSR:
        tmr.tcsr &= ~0xf;
        tmr.tcsr |= data & 0xf;
        if ((data & TMR_TCSR_OVF) == 0 && (tmr.status_rd & TMR_TCSR_OVF) != 0)
        {
            tmr.tcsr      &= ~TMR_TCSR_OVF;
            tmr.status_rd &= ~TMR_TCSR_OVF;
            MCU_Interrupt_SetRequest(*timer.mcu, INTERRUPT_SOURCE_TIMER_OVI, 0);
        }
        if ((data & TMR_TCSR_CMFA) == 0 && (tmr.status_rd & TMR_TCSR_CMFA) != 0)
        {
            tmr.tcsr      &= ~TMR_TCSR_CMFA;
            tmr.status_rd &= ~TMR_TCSR_CMFA;
            MCU_Interrupt_SetRequest(*timer.mcu, INTERRUPT_SOURCE_TIMER_CMIA, 0);
        }
        if ((data & TMR_TCSR_CMFB) == 0 && (tmr.status_rd & TMR_TCSR_CMFB) != 0)
        {
            tmr.tcsr      &= ~TMR_TCSR_CMFB;
            tmr.status_rd &= ~TMR_TCSR_CMFB;
            MCU_Interrupt_SetRequest(*timer.mcu, INTERRUPT_SOURCE_TIMER_CMIB, 0);
        }
        break;
    case DEV_TMR_TCORA:
        tmr.tcora = data;
        break;
    case DEV_TMR_TCORB:
        tmr.tcorb = data;
        break;
    case DEV_TMR_TCNT:
        tmr.tcnt = data;
        break;
    }
    timer.next_event = 0;
}

uint8_t TIMER_Read2(mcu_timer_t& timer, uint32_t address)
{
    TIMER_Sync(timer, timer.cycles);
    tmr_t& tmr = timer.tmr;

    switch (address)
    {
    case DEV_TMR_TCR:
        return tmr.tcr;
    case DEV_TMR_TCSR: {
        uint8_t ret    = tmr.tcsr;
        tmr.status_rd |= tmr.tcsr & (TMR_TCSR_OVF | TMR_TCSR_CMFA | TMR_TCSR_CMFB);
        return ret;
    }
    case DEV_TMR_TCORA:
        return tmr.tcora;
    case DEV_TMR_TCORB:
        return tmr.tcorb;
    case DEV_TMR_TCNT:
        return tmr.tcnt;
    }
    return 0xff;
}


// ---------------------------------------------------------------------------
// Closed-form advancement
//
// TIMER_Clock() is called once per emulated instruction and is only ever asked
// to advance six cycles, so with the usual /4 divider the three FRTs really do
// tick one or two times per call. Stepping them is not wasted work in the way
// the empty cycles were -- it is just work that produces nothing the MCU can
// see. Between events an FRT counter only counts, and nothing outside this file
// can look at it.
//
// So the timers are advanced lazily. `timer.cycles` still lands exactly where
// the original loop left it on every call, but the counters are only brought
// forward when a tick could produce something observable. `timer.synced` is how
// far they have actually been advanced, and `timer.next_event` is the first
// `cycles` value at which deferring is no longer allowed.
//
// What counts as observable, given that this function's only outputs are the
// timer registers and mcu.interrupt_pending:
//
//   * A tcsr flag going 0 -> 1. Flags are sticky; only TIMER_Write() and
//     TIMER2_Write() clear them, and those sync first.
//   * MCU_Interrupt_SetRequest(..., 1) on a source that is not already
//     requested. The call is an idempotent bit set (mcu_interrupt.cpp), and the
//     only code that clears a timer request is TIMER_Write()/TIMER2_Write(),
//     which sync first and clear the matching flag as they do it. So a request
//     that is already pending cannot be observed being set again -- but one that
//     has been cleared while its flag stayed set will be raised again on the
//     very next tick, and that is an event.
//   * A counter value, but only via TIMER_Read()/TIMER_Read2(), which sync.
//
// Nothing else reaches timer state: mcu_timer_t is touched by this file alone.
// ---------------------------------------------------------------------------

// Returned by the distance helpers when a value is never reached again.
static constexpr uint32_t TIMER_NEVER = 0xffffffffu;

// How many times a timer with this divider mask is clocked over [begin, end).
// Every step-table entry is 2^k - 1, so the clocked cycles are exactly the
// multiples of 2^k.
static inline uint64_t TIMER_TickCount(uint64_t begin, uint64_t end, uint64_t mask)
{
    // countr_zero(mask + 1), not popcount(mask): both give k for a 2^k - 1
    // mask, but gcc turns popcount into a call to __popcountdi2 unless the
    // target has the instruction, and neither a Cortex-A53 nor a baseline
    // x86-64 does. Counting trailing zeros is bsf / rbit+clz everywhere.
    const int shift = std::countr_zero(mask + 1);
    return ((end + mask) >> shift) - ((begin + mask) >> shift);
}

// First cycle at or after `from` on which such a timer is clocked.
static inline uint64_t TIMER_FirstTick(uint64_t from, uint64_t mask)
{
    return (from + mask) & ~mask;
}

// Ticks until an up-counter reading `cur` next reads `target`, where the counter
// runs 0..maxv and, if has_clear, is reset to 0 by a compare-match against
// `clear` instead of incrementing past it.
static uint32_t TIMER_DistToValue(uint32_t cur, uint32_t target, uint32_t maxv, bool has_clear, uint32_t clear)
{
    if (!has_clear)
        return (target - cur) & maxv;

    if (cur <= clear)
    {
        // Cycles within [0, clear] forever; nothing above clear is reachable.
        if (target > clear)
            return TIMER_NEVER;
        return target >= cur ? target - cur : target + clear + 1 - cur;
    }

    // Started above the clear point: counts up to maxv, wraps to 0, and only
    // then starts cycling within [0, clear].
    if (target >= cur)
        return target - cur;
    if (target <= clear)
        return maxv + 1 - cur + target;
    return TIMER_NEVER;
}

// Ticks until such a counter next wraps maxv -> 0, which is what sets OVF.
static uint32_t TIMER_DistToOverflow(uint32_t cur, uint32_t maxv, bool has_clear, uint32_t clear)
{
    // A counter cycling within [0, clear] never reaches the wrap; if clear is
    // maxv itself the compare-match clears it instead of overflowing.
    if (has_clear && cur <= clear)
        return TIMER_NEVER;
    return TIMER_NEVER;
}

// Advance one FRT by `ticks` clocks, exactly as `ticks` iterations of the
// original per-cycle body would have.
static void TIMER_AdvanceFrt(mcu_timer_t& timer, int frt_id, uint64_t ticks)
{
    frt_t& frt = timer.frt[frt_id];

    if (ticks == 0)
        return;

    const uint32_t ocra  = frt.ocra;
    const uint32_t ocrb  = frt.ocrb;
    const bool     cclra = (frt.tcsr & FRT_TCSR_CCLRA) != 0;

    uint32_t frc  = frt.frc;
    uint8_t  tcsr = frt.tcsr;

    while (ticks != 0)
    {
        // On a tick where frc is none of ocra, ocrb or 0xffff the body does
        // nothing but increment, so run all of those in one addition. Bounding
        // the run by 0xffff - frc is also what makes the addition safe.
        uint32_t       run = 0xffffu - frc;
        const uint32_t da  = (ocra - frc) & 0xffffu;
        const uint32_t db  = (ocrb - frc) & 0xffffu;
        if (da < run)
            run = da;
        if (db < run)
            run = db;

        if (run >= ticks)
        {
            frc += (uint32_t)ticks;
            break;
        }

        frc   += run;
        ticks -= run;

        // One tick of the original body, minus the interrupt requests.
        const bool matcha = frc == ocra;
        const bool matchb = frc == ocrb;
        if (cclra && matcha)
        {
            frc = 0;
        }
        else
        {
            frc = (frc + 1) & 0xffffu;
            if (frc == 0)
                tcsr |= FRT_TCSR_OVF;
        }
        if (matcha)
            tcsr |= FRT_TCSR_OCFA;
        if (matchb)
            tcsr |= FRT_TCSR_OCFB;

        --ticks;
    }

    frt.frc  = (uint16_t)frc;
    frt.tcsr = tcsr;

    // The original raises requests on every tick, for whichever flags are set at
    // the time. Requests are idempotent bit sets, tcr cannot change and no flag
    // can be cleared inside this span, so the union of all those calls is this
    // single pass over the final flags. At least one tick happened.
    if ((frt.tcr & FRT_TCR_OVIE) != 0 && (tcsr & FRT_TCSR_OVF) != 0)
        MCU_Interrupt_SetRequest(*timer.mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_FOVI + frt_id * 4), 1);
    if ((frt.tcr & FRT_TCR_OCIEA) != 0 && (tcsr & FRT_TCSR_OCFA) != 0)
        MCU_Interrupt_SetRequest(*timer.mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_OCIA + frt_id * 4), 1);
    if ((frt.tcr & FRT_TCR_OCIEB) != 0 && (tcsr & FRT_TCSR_OCFB) != 0)
        MCU_Interrupt_SetRequest(*timer.mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_OCIB + frt_id * 4), 1);
}

// Advance the 8-bit timer by `ticks` clocks. Same shape as the FRT above.
static void TIMER_AdvanceTmr(mcu_timer_t& timer, uint64_t ticks)
{
    tmr_t& tmr = timer.tmr;

    if (ticks == 0)
        return;

    const uint32_t tcora = tmr.tcora;
    const uint32_t tcorb = tmr.tcorb;
    const uint8_t  cclr  = tmr.tcr & (TMR_TCR_CCLR0 | TMR_TCR_CCLR1);

    uint32_t tcnt = tmr.tcnt;
    uint8_t  tcsr = tmr.tcsr;

    while (ticks != 0)
    {
        uint32_t       run = 0xffu - tcnt;
        const uint32_t da  = (tcora - tcnt) & 0xffu;
        const uint32_t db  = (tcorb - tcnt) & 0xffu;
        if (da < run)
            run = da;
        if (db < run)
            run = db;

        if (run >= ticks)
        {
            tcnt += (uint32_t)ticks;
            break;
        }

        tcnt  += run;
        ticks -= run;

        const bool matcha = tcnt == tcora;
        const bool matchb = tcnt == tcorb;
        if (cclr == TMR_TCR_CCLR0 && matcha)
        {
            tcnt = 0;
        }
        else if (cclr == TMR_TCR_CCLR1 && matchb)
        {
            tcnt = 0;
        }
        else
        {
            tcnt = (tcnt + 1) & 0xffu;
            if (tcnt == 0)
                tcsr |= TMR_TCSR_OVF;
        }
        if (matcha)
            tcsr |= TMR_TCSR_CMFA;
        if (matchb)
            tcsr |= TMR_TCSR_CMFB;

        --ticks;
    }

    tmr.tcnt = (uint8_t)tcnt;
    tmr.tcsr = tcsr;

    if ((tmr.tcr & TMR_TCR_OVIE) != 0 && (tcsr & TMR_TCSR_OVF) != 0)
        MCU_Interrupt_SetRequest(*timer.mcu, INTERRUPT_SOURCE_TIMER_OVI, 1);
    if ((tmr.tcr & TMR_TCR_CMIEA) != 0 && (tcsr & TMR_TCSR_CMFA) != 0)
        MCU_Interrupt_SetRequest(*timer.mcu, INTERRUPT_SOURCE_TIMER_CMIA, 1);
    if ((tmr.tcr & TMR_TCR_CMIEB) != 0 && (tcsr & TMR_TCSR_CMFB) != 0)
        MCU_Interrupt_SetRequest(*timer.mcu, INTERRUPT_SOURCE_TIMER_CMIB, 1);
}

// Ticks until this FRT does something observable, or TIMER_NEVER.
static uint32_t TIMER_FrtEventDist(const mcu_timer_t& timer, int frt_id)
{
    const frt_t& frt = timer.frt[frt_id];

    const uint8_t  tcr       = frt.tcr;
    const uint8_t  tcsr      = frt.tcsr;
    const bool     has_clear = (tcsr & FRT_TCSR_CCLRA) != 0;
    const uint32_t clear     = frt.ocra;
    const uint32_t cur       = frt.frc;

    // A set flag whose interrupt is enabled but no longer pending -- the MCU
    // acknowledged the request without clearing the flag -- is re-raised on the
    // next tick, so there is nothing to defer.
    const auto raises_again = [&](uint8_t enable, uint8_t flag, int source) {
        return (tcr & enable) != 0 && (tcsr & flag) != 0 &&
               !timer.mcu->interrupt_pending.Contains((MCU_Interrupt_Source)source);
    };
    if (raises_again(FRT_TCR_OVIE, FRT_TCSR_OVF, INTERRUPT_SOURCE_FRT0_FOVI + frt_id * 4) ||
        raises_again(FRT_TCR_OCIEA, FRT_TCSR_OCFA, INTERRUPT_SOURCE_FRT0_OCIA + frt_id * 4) ||
        raises_again(FRT_TCR_OCIEB, FRT_TCSR_OCFB, INTERRUPT_SOURCE_FRT0_OCIB + frt_id * 4))
    {
        return 0;
    }

    uint32_t dist = TIMER_NEVER;
    if ((tcsr & FRT_TCSR_OCFA) == 0)
    {
        const uint32_t d = TIMER_DistToValue(cur, frt.ocra, 0xffff, has_clear, clear);
        if (d < dist)
            dist = d;
    }
    if ((tcsr & FRT_TCSR_OCFB) == 0)
    {
        const uint32_t d = TIMER_DistToValue(cur, frt.ocrb, 0xffff, has_clear, clear);
        if (d < dist)
            dist = d;
    }
    if ((tcsr & FRT_TCSR_OVF) == 0)
    {
        const uint32_t d = TIMER_DistToOverflow(cur, 0xffff, has_clear, clear);
        if (d < dist)
            dist = d;
    }
    return dist;
}

// Ticks until the 8-bit timer does something observable, or TIMER_NEVER.
static uint32_t TIMER_TmrEventDist(const mcu_timer_t& timer)
{
    const tmr_t& tmr = timer.tmr;

    const uint8_t tcr  = tmr.tcr;
    const uint8_t tcsr = tmr.tcsr;
    const uint8_t cclr = tcr & (TMR_TCR_CCLR0 | TMR_TCR_CCLR1);

    // Only one of the two compare-matches clears the counter, and setting both
    // bits (or neither) leaves it free-running.
    const bool     has_clear = cclr == TMR_TCR_CCLR0 || cclr == TMR_TCR_CCLR1;
    const uint32_t clear     = cclr == TMR_TCR_CCLR0 ? tmr.tcora : tmr.tcorb;
    const uint32_t cur       = tmr.tcnt;

    const auto raises_again = [&](uint8_t enable, uint8_t flag, MCU_Interrupt_Source source) {
        return (tcr & enable) != 0 && (tcsr & flag) != 0 && !timer.mcu->interrupt_pending.Contains(source);
    };
    if (raises_again(TMR_TCR_OVIE, TMR_TCSR_OVF, INTERRUPT_SOURCE_TIMER_OVI) ||
        raises_again(TMR_TCR_CMIEA, TMR_TCSR_CMFA, INTERRUPT_SOURCE_TIMER_CMIA) ||
        raises_again(TMR_TCR_CMIEB, TMR_TCSR_CMFB, INTERRUPT_SOURCE_TIMER_CMIB))
    {
        return 0;
    }

    uint32_t dist = TIMER_NEVER;
    if ((tcsr & TMR_TCSR_CMFA) == 0)
    {
        const uint32_t d = TIMER_DistToValue(cur, tmr.tcora, 0xff, has_clear, clear);
        if (d < dist)
            dist = d;
    }
    if ((tcsr & TMR_TCSR_CMFB) == 0)
    {
        const uint32_t d = TIMER_DistToValue(cur, tmr.tcorb, 0xff, has_clear, clear);
        if (d < dist)
            dist = d;
    }
    if ((tcsr & TMR_TCSR_OVF) == 0)
    {
        const uint32_t d = TIMER_DistToOverflow(cur, 0xff, has_clear, clear);
        if (d < dist)
            dist = d;
    }
    return dist;
}

// Work out the earliest cycle on which any timer does something observable, and
// store the first `cycles` value that must not be deferred past.
static void TIMER_ScheduleNextEvent(mcu_timer_t& timer)
{
    uint64_t next = ~(uint64_t)0;

    for (int i = 0; i < 3; i++)
    {
        const uint32_t dist = TIMER_FrtEventDist(timer, i);
        if (dist == TIMER_NEVER)
            continue;
        const uint64_t mask = timer.frt_step_table[timer.frt[i].tcr & (FRT_TCR_CKS0 | FRT_TCR_CKS1)];
        const uint64_t at   = TIMER_FirstTick(timer.synced, mask) + (uint64_t)dist * (mask + 1);
        if (at < next)
            next = at;
    }

    // A step mask of 0 means this timer never steps at all, so it can never
    // reach an event; it is not an edge on every cycle.
    const uint64_t tmr_mask = timer.tmr_step_table[timer.tmr.tcr & (TMR_TCR_CKS0 | TMR_TCR_CKS1 | TMR_TCR_CKS2)];
    if (tmr_mask != 0)
    {
        const uint32_t dist = TIMER_TmrEventDist(timer);
        if (dist != TIMER_NEVER)
        {
            const uint64_t at = TIMER_FirstTick(timer.synced, tmr_mask) + (uint64_t)dist * (tmr_mask + 1);
            if (at < next)
                next = at;
        }
    }

    // TIMER_Clock() processes the ticks strictly below its limit, so the limit
    // it must not stop short of is one past the event cycle.
    timer.next_event = next == ~(uint64_t)0 ? next : next + 1;
}

static void TIMER_Sync(mcu_timer_t& timer, uint64_t limit)
{
    if (limit > timer.synced)
    {
        for (int i = 0; i < 3; i++)
        {
            const uint64_t mask = timer.frt_step_table[timer.frt[i].tcr & (FRT_TCR_CKS0 | FRT_TCR_CKS1)];
            TIMER_AdvanceFrt(timer, i, TIMER_TickCount(timer.synced, limit, mask));
        }

        const uint64_t tmr_mask =
            timer.tmr_step_table[timer.tmr.tcr & (TMR_TCR_CKS0 | TMR_TCR_CKS1 | TMR_TCR_CKS2)];
        if (tmr_mask != 0)
            TIMER_AdvanceTmr(timer, TIMER_TickCount(timer.synced, limit, tmr_mask));

        timer.synced = limit;
    }

    TIMER_ScheduleNextEvent(timer);
}

void TIMER_Clock(mcu_timer_t& timer, uint64_t cycles)
{
    // Where the original one-cycle-at-a-time loop would have stopped. Landing
    // exactly here matters: timer.cycles persists across calls, and running
    // past it would consume cycles a finer divider could later have fired on.
    const uint64_t limit = (cycles + 1) / 2; // FIXME: the /2 is upstream's

    if (timer.cycles < limit)
        timer.cycles = limit;

    if (limit >= timer.next_event)
        TIMER_Sync(timer, limit);
}

// These tables are indexed by the low CKSn bits of the TCR.
constexpr FRT_Step_Table FRT_STEP_TABLE_GENERIC = {3, 7, 31, 1};
constexpr FRT_Step_Table FRT_STEP_TABLE_MK1     = {3, 7, 31, 3};

// A value of 0 means do not step.
constexpr TMR_Step_Table TMR_STEP_TABLE_GENERIC = {0, 7, 63, 1023, 0, 1, 1, 1};
constexpr TMR_Step_Table TMR_STEP_TABLE_MK1     = {0, 7, 63, 1023, 0, 3, 3, 3};

void TIMER_NotifyRomsetChange(mcu_timer_t& timer)
{
    // Deferred ticks belong to the old dividers; apply them before switching.
    if (timer.synced < timer.cycles)
        TIMER_Sync(timer, timer.cycles);

    const bool is_mk1    = timer.mcu->is_mk1;
    timer.frt_step_table = is_mk1 ? FRT_STEP_TABLE_MK1 : FRT_STEP_TABLE_GENERIC;
    timer.tmr_step_table = is_mk1 ? TMR_STEP_TABLE_MK1 : TMR_STEP_TABLE_GENERIC;
    timer.next_event     = 0;
}
