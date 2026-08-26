/*
 * Differential check for
 * patches/0016-submcu-fast-forward-idle-loop.patch.
 *
 * The patch lets the sub-MCU's clock run ahead across whole passes of a
 * stationary idle loop and rewinds it on any external access. The property
 * to prove is that the main MCU, and anything outside, can never tell: every
 * value the main MCU reads from the sub-MCU, every call the sub-MCU makes
 * outward, and -- at every synchronisation point -- the entire sub-MCU state
 * including its clock and timer, are identical to upstream's.
 *
 * The core's real submcu.cpp is linked twice: the patched one normally and
 * upstream's with every global renamed Ref_* (the runner generates the list
 * from nm). Both are driven by the same simulated main MCU: the step cadence
 * of MCU_Step (12 main cycles per step), shared-RAM and IPC reads and writes
 * at random moments, UART bytes posted at random moments, port state changes,
 * and the real timer. Comparison happens after every observable and, at
 * random intervals, on the full state after the patched side is synchronised
 * to the main MCU's clock -- exactly what MCU_PostUART/SM_SysRead/SM_SysWrite
 * do before touching it.
 *
 * Programs: the real sub-MCU firmware if SM_ROM names a file (the idle loop
 * the patch exists for), and three synthetic programs assembled here from the
 * core's own opcode table: a pure idle loop with timer and UART handlers (the
 * stationary case), the same loop storing to shared RAM each pass (impure --
 * must never fast-forward), and a loop incrementing a counter (never
 * stationary). Mutants are produced by the runner from the patched source.
 */
#include "submcu.h"
#include "mcu.h"
#include "diagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* --- outward calls, logged per main-MCU instance so both sides must agree --- */
struct Call { const char* what; uint64_t a, b; };
static std::vector<Call> g_log[2];
static mcu_t* g_mcu[2];
static int Side(mcu_t& m) { return &m == g_mcu[0] ? 0 : 1; }
void Diag_Printf(Diag_Category, const char*, ...) {}
uint8_t MCU_ReadP0(mcu_t& m) { return (uint8_t)(0x3c ^ m.p0_data); }
uint8_t MCU_ReadP1(mcu_t& m) { return (uint8_t)(0x5a ^ m.p0_data ^ m.p1_data); }
void MCU_WriteP0(mcu_t& m, uint8_t v) { m.p0_data = v; g_log[Side(m)].push_back({"P0", v, 0}); }
void MCU_WriteP1(mcu_t& m, uint8_t v) { m.p1_data = v; g_log[Side(m)].push_back({"P1", v, 0}); }
void MCU_GA_SetGAInt(mcu_t& m, uint8_t i, bool v) { g_log[Side(m)].push_back({"GA", i, v}); }
void MCU_DefaultSampleCallback(void*, const AudioFrame<int32_t>&) {}

void SM_Init(submcu_t& sm, mcu_t& mcu); void SM_Reset(submcu_t& sm);
void SM_Update(submcu_t& sm, uint64_t cycles);
uint8_t SM_SysRead(submcu_t& sm, uint32_t address); void SM_SysWrite(submcu_t& sm, uint32_t address, uint8_t data);
void Ref_SM_Init(submcu_t& sm, mcu_t& mcu); void Ref_SM_Reset(submcu_t& sm);
void Ref_SM_Update(submcu_t& sm, uint64_t cycles);
uint8_t Ref_SM_SysRead(submcu_t& sm, uint32_t address); void Ref_SM_SysWrite(submcu_t& sm, uint32_t address, uint8_t data);
void Ref_SM_FF_Sync(submcu_t&, uint64_t) {}   /* upstream has no clock to rewind */

namespace {

uint32_t Rand(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

/* --- synthetic programs, from the core's opcode table ---------------------- */
enum { LDA_IMM = 0xa9, LDA_ZP = 0xa5, STA_ZP = 0x85, STA_ABS = 0x8d, LDM = 0x3c, LDX_IMM = 0xa2,
       TXS = 0x9a, SEI = 0x78, CLI = 0x58, JMP_ABS = 0x4c, RTI = 0x40, INC_ZP = 0xe6 };
uint8_t BBS(int bit) { return (uint8_t)(0x07 | (bit << 5)); }   /* BBS bit,zp,rel */

struct Asm {
    uint8_t rom[4096]; uint16_t at;
    Asm() { memset(rom, 0x40 /* RTI */, sizeof rom); at = 0; }
    void org(uint16_t a) { at = (uint16_t)(a & 0xfff); }
    void b(int v) { rom[at++] = (uint8_t)v; }
    void vec(int v, uint16_t target) { rom[(0xfec + v * 2) & 0xfff] = (uint8_t)target; rom[(0xfec + v * 2 + 1) & 0xfff] = (uint8_t)(target >> 8); }
};

/* kind 0: pure idle loop; 1: same loop with a shared-RAM store; 2: counting loop */
void Program(Asm& a, int kind)
{
    const uint16_t HEAD = 0x1120, TIMER = 0x1180, UART = 0x1190, IPC = 0x11a0, IDLE = 0x11b0;
    a.org(0x1100);
    a.b(LDX_IMM); a.b(0x40); a.b(TXS);                      /* stack inside zero page */
    a.b(LDA_IMM); a.b(0x04); a.b(STA_ZP); a.b(0xe6);        /* UART1_CTRL: RX enable */
    a.b(LDA_IMM); a.b(0x08); a.b(STA_ZP); a.b(0xea);        /* UART2_CTRL: RX interrupt arm */
    a.b(LDA_IMM); a.b(0x3f); a.b(STA_ZP); a.b(0xfd);        /* PRESCALER reload */
    a.b(LDA_IMM); a.b(0x1f); a.b(STA_ZP); a.b(0xfe);        /* TIMER reload */
    a.b(LDA_IMM); a.b(0xc0); a.b(STA_ZP); a.b(0xff);        /* TIMER_CTRL: arm timer X and IPCM0, running */
    a.b(LDA_IMM); a.b(0x58); a.b(STA_ZP); a.b(0xfb);        /* INT_ENABLE: UART2 RX, IPCM0, timer X */
    a.b(CLI);
    a.b(JMP_ABS); a.b(HEAD & 0xff); a.b(HEAD >> 8);
    a.org(HEAD);
    a.b(LDA_ZP); a.b(0x01);
    a.b(STA_ZP); a.b(0x02);
    a.b(LDM); a.b(0x00); a.b(0x03);                         /* LDM #0 -> $03 */
    a.b(SEI);
    a.b(LDA_ZP); a.b(0x00);
    a.b(STA_ZP); a.b(0x00);
    a.b(CLI);
    if (kind == 1) { a.b(STA_ABS); a.b(0x20); a.b(0x02); } /* shared RAM $0220: impure */
    if (kind == 2) { a.b(INC_ZP); a.b(0x07); }             /* counter: never stationary */
    a.b(BBS(0)); a.b(0x01); a.b(0x03);                      /* flag set by the timer handler? -> service */
    a.b(JMP_ABS); a.b(HEAD & 0xff); a.b(HEAD >> 8);
    a.b(LDM); a.b(0x00); a.b(0x01);                         /* service: clear flag */
    a.b(INC_ZP); a.b(0x02);
    a.b(JMP_ABS); a.b(HEAD & 0xff); a.b(HEAD >> 8);
    a.org(TIMER);  a.b(LDM); a.b(0x01); a.b(0x01); a.b(INC_ZP); a.b(0x04); a.b(RTI);
    a.org(UART);   a.b(LDA_ZP); a.b(0xe8); a.b(STA_ZP); a.b(0x05); a.b(INC_ZP); a.b(0x06); a.b(RTI);
    a.org(IPC);    a.b(INC_ZP); a.b(0x08); a.b(RTI);
    a.org(IDLE);   a.b(RTI);
    for (int v = 0; v < 10; v++) a.vec(v, IDLE);
    a.vec(9, 0x1100); a.vec(4, TIMER); a.vec(7, UART); a.vec(5, IPC);
}

/* --- state comparison -------------------------------------------------------- */
struct Obs {
    uint16_t pc; uint8_t a, x, y, s, sr, sleep; uint64_t cycles;
    uint8_t ram[128], shared[192], access[0x18], p0_dir, p1_dir, dev[32], cts, gotbyte;
    uint64_t timer_cycles; uint8_t timer_prescaler, timer_counter;
    uint32_t uart_read_ptr; uint8_t uart_rx_byte; uint64_t uart_rx_delay; uint8_t p0, p1;
    bool operator==(const Obs& o) const { return memcmp(this, &o, sizeof(Obs)) == 0; }
};
Obs Take(const submcu_t& sm, const mcu_t& m)
{
    Obs o; memset(&o, 0, sizeof o);
    o.pc = sm.pc; o.a = sm.a; o.x = sm.x; o.y = sm.y; o.s = sm.s; o.sr = sm.sr; o.sleep = sm.sleep; o.cycles = sm.cycles;
    memcpy(o.ram, sm.ram, 128); memcpy(o.shared, sm.shared_ram, 192); memcpy(o.access, sm.access, 0x18);
    o.p0_dir = sm.p0_dir; o.p1_dir = sm.p1_dir; memcpy(o.dev, sm.device_mode, 32); o.cts = sm.cts; o.gotbyte = sm.uart_rx_gotbyte;
    o.timer_cycles = sm.timer_cycles; o.timer_prescaler = sm.timer_prescaler; o.timer_counter = sm.timer_counter;
    o.uart_read_ptr = m.uart_read_ptr; o.uart_rx_byte = m.uart_rx_byte; o.uart_rx_delay = m.uart_rx_delay; o.p0 = m.p0_data; o.p1 = m.p1_data;
    return o;
}
bool SameLog()
{
    if (g_log[0].size() != g_log[1].size()) return false;
    for (size_t i = 0; i < g_log[0].size(); i++)
        if (strcmp(g_log[0][i].what, g_log[1][i].what) || g_log[0][i].a != g_log[1][i].a || g_log[0][i].b != g_log[1][i].b) return false;
    return true;
}

int failures = 0;
uint64_t g_jumps = 0;
uint64_t g_trace_until = 0;
const bool g_trace = getenv("FF_TRACE") != nullptr;   /* opt-in step trace for debugging a divergence */

void Post(mcu_t& m, uint8_t data)   /* what MCU_PostUART does, upstream shape */
{
    m.uart_buffer[m.uart_write_ptr] = data;
    m.uart_write_ptr = (m.uart_write_ptr + 1) % uart_buffer_size;
}

bool Run(const char* name, const uint8_t* rom, uint64_t steps, uint32_t seed, bool expect_jumps)
{
    submcu_t& ref = *new submcu_t(); submcu_t& neu = *new submcu_t();
    mcu_t& mr = *g_mcu[0]; mcu_t& mn = *g_mcu[1];
    memset(&mr, 0, sizeof(mcu_t)); memset(&mn, 0, sizeof(mcu_t));
    g_log[0].clear(); g_log[1].clear();
    memcpy(ref.rom, rom, 4096); memcpy(neu.rom, rom, 4096);
    Ref_SM_Init(ref, mr); SM_Init(neu, mn);
    Ref_SM_Reset(ref); SM_Reset(neu);
    uint64_t mismatches = 0, syncs = 0, reads = 0, ahead_seen = 0;
    static const uint32_t addrs[] = { 0x00, 0x20, 0x40, 0x7f, 0xb9, 0xbf, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xff };
    for (uint64_t step = 0; step < steps; step++)
    {
        uint32_t r = Rand(seed);
        /* External accesses happen while the main MCU executes its instruction,
         * i.e. BEFORE MCU_Step adds this instruction's cycles and catches the
         * sub-MCU up -- the hooks see mcu.cycles equal to the last SM_Update
         * target. The order here mirrors MCU_Step exactly. */
        if ((r & 0x3f) == 1)            /* main MCU writes to the sub-MCU */
        {
            uint32_t addr = addrs[(r >> 8) % 14]; uint8_t d = (uint8_t)(r >> 16);
            Ref_SM_SysWrite(ref, addr, d); SM_SysWrite(neu, addr, d);
        }
        if ((r & 0x1f) == 2)            /* main MCU reads from the sub-MCU */
        {
            uint32_t addr = addrs[(r >> 8) % 14];
            uint8_t a = Ref_SM_SysRead(ref, addr), b = SM_SysRead(neu, addr); reads++;
            if (a != b) { mismatches++; if (failures < 10) printf("  [%s] step %llu SysRead %02x: ref %02x neu %02x\n", name, (unsigned long long)step, addr, a, b); }
        }
        if ((r & 0x1ff) == 3)           /* a MIDI byte arrives */
        {
            uint8_t d = (uint8_t)(r >> 16);
            Post(mr, d);
            SM_FF_Sync(neu, mn.cycles * 5); Post(mn, d);
            if (g_trace) printf("  [%s] step %llu POST %02x -> write_ptr %u/%u\n", name, (unsigned long long)step, d, mr.uart_write_ptr, mn.uart_write_ptr);
            g_trace_until = step + 4;
        }
        if ((r & 0xfff) == 4) { mr.p0_data = (uint8_t)(r >> 20); mn.p0_data = (uint8_t)(r >> 20); }

        mr.cycles += 12; mn.cycles += 12;
        Ref_SM_Update(ref, mr.cycles); SM_Update(neu, mn.cycles);
        if (neu.ff_ahead) ahead_seen++;
        if (g_trace && step <= g_trace_until)
            printf("      step %llu after update: cycles %llu/%llu pc %04x/%04x sleep %u/%u ctrl6 %02x/%02x wp %u/%u rp %u/%u got %u/%u delay %llu/%llu req %02x/%02x sr %02x/%02x\n",
                   (unsigned long long)step, (unsigned long long)ref.cycles, (unsigned long long)neu.cycles, ref.pc, neu.pc, ref.sleep, neu.sleep,
                   ref.device_mode[6], neu.device_mode[6], mr.uart_write_ptr, mn.uart_write_ptr, mr.uart_read_ptr, mn.uart_read_ptr,
                   ref.uart_rx_gotbyte, neu.uart_rx_gotbyte, (unsigned long long)mr.uart_rx_delay, (unsigned long long)mn.uart_rx_delay,
                   ref.device_mode[0x1c], neu.device_mode[0x1c], ref.sr, neu.sr);

        if ((r & 0x7) == 0 || (step & 0xff) == 0xff || step + 1 == steps)
        {
            SM_FF_Sync(neu, mn.cycles * 5); syncs++;
            Obs oa = Take(ref, mr), ob = Take(neu, mn);
            if (!(oa == ob) || !SameLog())
            {
                mismatches++;
                if (failures < 10)
                {
                    printf("  [%s] step %llu STATE MISMATCH: pc %04x/%04x cycles %llu/%llu a %02x/%02x sr %02x/%02x timer %llu/%llu presc %u/%u cnt %u/%u log %zu/%zu\n", name,
                           (unsigned long long)step, ref.pc, neu.pc, (unsigned long long)ref.cycles, (unsigned long long)neu.cycles,
                           ref.a, neu.a, ref.sr, neu.sr, (unsigned long long)ref.timer_cycles, (unsigned long long)neu.timer_cycles,
                           ref.timer_prescaler, neu.timer_prescaler, ref.timer_counter, neu.timer_counter, g_log[0].size(), g_log[1].size());
                    printf("      uart write_ptr %u/%u read_ptr %u/%u UART1_CTRL %02x/%02x gotbyte %u/%u delay %llu/%llu\n",
                           mr.uart_write_ptr, mn.uart_write_ptr, mr.uart_read_ptr, mn.uart_read_ptr,
                           ref.device_mode[6], neu.device_mode[6], ref.uart_rx_gotbyte, neu.uart_rx_gotbyte,
                           (unsigned long long)mr.uart_rx_delay, (unsigned long long)mn.uart_rx_delay);
                    const uint8_t* pa = (const uint8_t*)&oa; const uint8_t* pb = (const uint8_t*)&ob;
                    for (size_t i = 0, shown = 0; i < sizeof(Obs) && shown < 6; i++)
                        if (pa[i] != pb[i]) { printf("      byte %zu of Obs differs: %02x/%02x (ram@%zu dev@%zu)\n", i, pa[i], pb[i], offsetof(Obs, ram), offsetof(Obs, dev)); shown++; }
                }
                failures++;
                if (failures >= 10) break;
            }
        }
    }
    printf("%-10s %llu steps, %llu syncs, %llu reads, ahead after %llu steps, stationary=%s: %s\n", name,
           (unsigned long long)steps, (unsigned long long)syncs, (unsigned long long)reads, (unsigned long long)ahead_seen,
           neu.ff_state == 2 ? "yes" : "no", mismatches ? "MISMATCH" : "ok");
    if (expect_jumps && ahead_seen == 0) { printf("  [%s] the fast path never engaged; the test would not have exercised it\n", name); failures++; }
    if (!expect_jumps && ahead_seen != 0) { printf("  [%s] the fast path engaged on a loop it must not touch\n", name); failures++; }
    g_jumps += ahead_seen;
    return mismatches == 0;
}

} // namespace

int main(int argc, char** argv)
{
    g_mcu[0] = new mcu_t(); g_mcu[1] = new mcu_t();
    const uint64_t steps = argc > 1 ? strtoull(argv[1], nullptr, 10) : 4000000;

    Asm a; Program(a, 0); Run("pure", a.rom, steps, 0x1234abcd, true);
    Asm b; Program(b, 1); Run("impure", b.rom, steps, 0x2345bcde, false);
    Asm c; Program(c, 2); Run("counting", c.rom, steps, 0x3456cdef, false);

    if (const char* path = getenv("SM_ROM"))
    {
        FILE* f = fopen(path, "rb");
        static uint8_t rom[4096];
        if (f && fread(rom, 1, 4096, f) == 4096) { fclose(f); Run("firmware", rom, steps * 4, 0x4567def0, true); }
        else printf("firmware: could not read %s (skipped)\n", path);
    }
    else printf("firmware: SM_ROM not set (skipped; synthetic programs only)\n");

    if (failures) { printf("FAIL: %d\n", failures); return 1; }
    printf("PASS\n");
    return 0;
}
