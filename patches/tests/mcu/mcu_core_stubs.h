/*
 * Recording stubs for everything mcu.cpp calls outside itself. Every stub
 * appends to a log, so a test can tell not just what a read returned but
 * whether reaching it disturbed anything.
 *
 * Shared by mcu_code_fetch_equivalence.cpp and
 * mcu_step_fixed_work_equivalence.cpp.
 */
#pragma once

#include "mcu.h"
#include "mcu_interrupt.h"
#include "mcu_opcodes.h"
#include "mcu_timer.h"
#include "pcm.h"
#include "submcu.h"
#include "lcd.h"
#include "diagnostics.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstddef>

struct StubCall
{
    const char* what;
    uint64_t    a, b, c;
    bool operator==(const StubCall& o) const
    {
        return what == o.what && a == o.a && b == o.b && c == o.c;
    }
};

inline std::vector<StubCall>& StubLog()
{
    static std::vector<StubCall> log;
    return log;
}

inline void Note(const char* what, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0)
{
    StubLog().push_back({what, a, b, c});
}

// mcu.cpp's external world. The returns are deterministic functions of the
// arguments so a test can re-run a call and expect the same answer.
void Diag_Printf(Diag_Category, const char* format, ...) { Note("Diag_Printf", (uint64_t)(uintptr_t)format); }
void LCD_Enable(lcd_t&, bool v) { Note("LCD_Enable", v); }
void LCD_Write(lcd_t&, uint32_t a, uint8_t v) { Note("LCD_Write", a, v); }
#ifndef STUBS_REAL_INTERRUPT
void MCU_Interrupt_Exception(mcu_t&, MCU_Exception_Source s) { Note("MCU_Interrupt_Exception", (uint64_t)s); }
void MCU_Interrupt_Handle(mcu_t&) { Note("MCU_Interrupt_Handle"); }
void MCU_Interrupt_SetRequest(mcu_t&, MCU_Interrupt_Source s, bool v) { Note("MCU_Interrupt_SetRequest", (uint64_t)s, v); }
#endif
uint8_t PCM_Read(pcm_t&, uint32_t a) { Note("PCM_Read", a); return (uint8_t)(0xa5 ^ a); }
void PCM_Update(pcm_t&, uint64_t c) { Note("PCM_Update", c); }
void PCM_Write(pcm_t&, uint32_t a, uint8_t v) { Note("PCM_Write", a, v); }
uint8_t SM_SysRead(submcu_t&, uint32_t a) { Note("SM_SysRead", a); return (uint8_t)(0x5a ^ a); }
void SM_SysWrite(submcu_t&, uint32_t a, uint8_t v) { Note("SM_SysWrite", a, v); }
void SM_Update(submcu_t&, uint64_t c) { Note("SM_Update", c); }
void TIMER2_Write(mcu_timer_t&, uint32_t a, uint8_t v) { Note("TIMER2_Write", a, v); }
void TIMER_Clock(mcu_timer_t&, uint64_t c) { Note("TIMER_Clock", c); }
void TIMER_NotifyRomsetChange(mcu_timer_t&) { Note("TIMER_NotifyRomsetChange"); }
uint8_t TIMER_Read(mcu_timer_t&, uint32_t a) { Note("TIMER_Read", a); return (uint8_t)(0x11 ^ a); }
uint8_t TIMER_Read2(mcu_timer_t&, uint32_t a) { Note("TIMER_Read2", a); return (uint8_t)(0x22 ^ a); }
void TIMER_Reset(mcu_timer_t&) { Note("TIMER_Reset"); }
void TIMER_Write(mcu_timer_t&, uint32_t a, uint8_t v) { Note("TIMER_Write", a, v); }

void (*MCU_Operand_Table[256])(mcu_t& mcu, uint8_t operand) = {};

/*
 * A snapshot of every mutable scalar in mcu_t: the whole struct minus the seven
 * bulk arrays, whichever order the fields happen to be declared in. MCU_Read()
 * and MCU_UpdateAnalog() never write to those arrays, so this captures all the
 * state they can disturb, and it stays a few hundred bytes rather than 664 KiB.
 */
struct Region { size_t off, len; };

inline const std::vector<Region>& ScalarRegions()
{
    static const std::vector<Region> regions = [] {
        std::vector<Region> arrays = {
            { offsetof(mcu_t, rom1),        sizeof(((mcu_t*)nullptr)->rom1) },
            { offsetof(mcu_t, rom2),        sizeof(((mcu_t*)nullptr)->rom2) },
            { offsetof(mcu_t, ram),         sizeof(((mcu_t*)nullptr)->ram) },
            { offsetof(mcu_t, sram),        sizeof(((mcu_t*)nullptr)->sram) },
            { offsetof(mcu_t, nvram),       sizeof(((mcu_t*)nullptr)->nvram) },
            { offsetof(mcu_t, cardram),     sizeof(((mcu_t*)nullptr)->cardram) },
            { offsetof(mcu_t, uart_buffer), sizeof(((mcu_t*)nullptr)->uart_buffer) },
        };
        std::sort(arrays.begin(), arrays.end(),
                  [](const Region& x, const Region& y) { return x.off < y.off; });
        std::vector<Region> gaps;
        size_t at = 0;
        for (const Region& r : arrays)
        {
            if (r.off > at)
                gaps.push_back({ at, r.off - at });
            at = r.off + r.len;
        }
        if (at < sizeof(mcu_t))
            gaps.push_back({ at, sizeof(mcu_t) - at });
        return gaps;
    }();
    return regions;
}

inline size_t ScalarBytes()
{
    static const size_t n = [] {
        size_t t = 0;
        for (const Region& r : ScalarRegions())
            t += r.len;
        return t;
    }();
    return n;
}

struct Snapshot
{
    std::vector<unsigned char> bytes = std::vector<unsigned char>(ScalarBytes());

    void Take(const mcu_t& mcu)
    {
        const unsigned char* base = reinterpret_cast<const unsigned char*>(&mcu);
        size_t at = 0;
        for (const Region& r : ScalarRegions())
        {
            memcpy(bytes.data() + at, base + r.off, r.len);
            at += r.len;
        }
    }

    void Restore(mcu_t& mcu) const
    {
        unsigned char* base = reinterpret_cast<unsigned char*>(&mcu);
        size_t at = 0;
        for (const Region& r : ScalarRegions())
        {
            memcpy(base + r.off, bytes.data() + at, r.len);
            at += r.len;
        }
    }

    bool operator==(const Snapshot& o) const { return bytes == o.bytes; }
};
