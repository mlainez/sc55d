// Shared harness: drives PCM_Update in isolation, no ROMs, no MCU.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "mcu.h"
#include "mcu_interrupt.h"
#include "pcm.h"

// ---- stubs for the three MCU entry points pcm.cpp calls -------------------
bool g_hash_off = false;   // set in quiet (Ir-measurement) runs only
static uint64_t g_post_hash = 1469598103934665603ull;
static uint64_t g_int_hash  = 1469598103934665603ull;
static inline void fnv(uint64_t& h, uint64_t v)
{
    for (int i = 0; i < 8; i++) { h ^= (v >> (i*8)) & 0xff; h *= 1099511628211ull; }
}
void MCU_PostSample(mcu_t&, const AudioFrame<int32_t>& f)
{
    if (g_hash_off) return;
    fnv(g_post_hash, (uint32_t)f.left); fnv(g_post_hash, (uint32_t)f.right);
}
void MCU_Interrupt_SetRequest(mcu_t&, MCU_Interrupt_Source s, bool v)
{
    if (g_hash_off) return;
    fnv(g_int_hash, (uint64_t)s); fnv(g_int_hash, v);
}
void MCU_GA_SetGAInt(mcu_t&, uint8_t line, bool value)
{
    fnv(g_int_hash, line); fnv(g_int_hash, value);
}
void MCU_DefaultSampleCallback(void*, const AudioFrame<int32_t>&) {}
