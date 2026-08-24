// Minimal stand-in for the core's mcu.h, holding just what mcu_timer.cpp uses.
// Deliberately has NO include guard: the test includes it once inside each of
// the two namespaces it builds the timer into, so each namespace gets its own
// mcu_t and its own interrupt bitset.

enum MCU_Interrupt_Source : uint8_t
{
    INTERRUPT_SOURCE_NMI = 0,
    INTERRUPT_SOURCE_IRQ0,
    INTERRUPT_SOURCE_IRQ1,
    INTERRUPT_SOURCE_FRT0_ICI,
    INTERRUPT_SOURCE_FRT0_OCIA,
    INTERRUPT_SOURCE_FRT0_OCIB,
    INTERRUPT_SOURCE_FRT0_FOVI,
    INTERRUPT_SOURCE_FRT1_ICI,
    INTERRUPT_SOURCE_FRT1_OCIA,
    INTERRUPT_SOURCE_FRT1_OCIB,
    INTERRUPT_SOURCE_FRT1_FOVI,
    INTERRUPT_SOURCE_FRT2_ICI,
    INTERRUPT_SOURCE_FRT2_OCIA,
    INTERRUPT_SOURCE_FRT2_OCIB,
    INTERRUPT_SOURCE_FRT2_FOVI,
    INTERRUPT_SOURCE_TIMER_CMIA,
    INTERRUPT_SOURCE_TIMER_CMIB,
    INTERRUPT_SOURCE_TIMER_OVI,
    INTERRUPT_SOURCE_MAX,
};

enum
{
    DEV_TMR_TCR   = 0x50,
    DEV_TMR_TCSR  = 0x51,
    DEV_TMR_TCORA = 0x52,
    DEV_TMR_TCORB = 0x53,
    DEV_TMR_TCNT  = 0x54,
};

struct PendingSet
{
    uint64_t bits = 0;
    void Include(MCU_Interrupt_Source s) { bits |= (uint64_t)1 << s; }
    void Exclude(MCU_Interrupt_Source s) { bits &= ~((uint64_t)1 << s); }
    bool Contains(MCU_Interrupt_Source s) const { return (bits >> s) & 1; }
};

struct mcu_t
{
    bool       is_mk1 = false;
    PendingSet interrupt_pending;
};

// Same body as the core's, which is an idempotent bit set/clear.
inline void MCU_Interrupt_SetRequest(mcu_t& mcu, MCU_Interrupt_Source interrupt, bool value)
{
    if (value)
        mcu.interrupt_pending.Include(interrupt);
    else
        mcu.interrupt_pending.Exclude(interrupt);
}
