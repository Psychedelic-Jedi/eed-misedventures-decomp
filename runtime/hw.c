/* Minimal GameCube hardware register model (0xCC000000 MMIO).
 * Registers read back what was written, with a few "always ready" quirks so
 * SDK polling loops complete. Real behaviour (DVD, VI, GX FIFO, SI pads) is
 * replaced at the SDK level over time (see hle.c). */
#include <stdio.h>
#include <string.h>
#include "hw.h"

static u8 g_regs[0x10000];
unsigned long long hw_reads, hw_writes;
int hw_log_limit = 400;
/* Log each distinct (register, guest pc) pair once so polling loops stay readable. */
static u64 g_seen[4096];
static int seen_once(u32 ea, u32 pc, int w) {
    u64 k = ((u64)ea << 32) | pc | ((u64)w << 63);
    unsigned h = (unsigned)((k * 0x9E3779B97F4A7C15ull) >> 52);
    for (unsigned i = 0; i < 4096; i++) {
        unsigned j = (h + i) & 4095;
        if (g_seen[j] == k) return 0;
        if (!g_seen[j]) { g_seen[j] = k; return 1; }
    }
    return 0;
}
static unsigned g_logged;

static const char *hw_name(u32 a) {
    switch (a & 0xFFFFFF00u) {
    case 0xCC000000u: return "CP";   case 0xCC001000u: return "PE";
    case 0xCC002000u: return "VI";   case 0xCC003000u: return "PI";
    case 0xCC004000u: return "MI";   case 0xCC005000u: return "DSP";
    case 0xCC006000u: return "DI";   case 0xCC006400u: return "SI";
    case 0xCC006800u: return "EXI";  case 0xCC006C00u: return "AI";
    case 0xCC008000u: return "GXFIFO";
    }
    return "?";
}

static u64 get(u32 off, u8 size) {
    u64 v = 0;
    for (u8 i = 0; i < size; i++) v = (v << 8) | g_regs[(off + i) & 0xFFFF];
    return v;
}
static void put(u32 off, u64 v, u8 size) {
    for (int i = size - 1; i >= 0; i--) { g_regs[(off + i) & 0xFFFF] = (u8)v; v >>= 8; }
}

u64 hw_read(CPUState *cpu, u32 ea, u8 size) {
    hw_reads++;
    u32 off = ea & 0xFFFF;
    u64 v = get(off, size);
    switch (off & ~1u) {
    case 0x500A: v &= ~0x0200u; break;      /* DSP CSR: ARAM DMA never busy */
    case 0x5016: v = 1; break;              /* AR_MODE: ARAM ready */
    case 0x680C: case 0x680E:               /* EXI0/1/2 control: transfers complete instantly */
    case 0x6820: case 0x6822:
    case 0x6834: case 0x6836:
        if (size == 4 || (off & 2)) v &= ~1ull;
        break;
    case 0x6C08: {                          /* AI sample counter: advances while audio runs */
        static u32 aiscnt;
        aiscnt += 8;
        v = aiscnt;
        break;
    }
    }
    if (g_logged < (unsigned)hw_log_limit && seen_once(ea, cpu->pc, 0)) {
        g_logged++;
        fprintf(stderr, "[mmio] R%u %08X (%s) -> %llX pc=%08X lr=%08X\n", size * 8, ea, hw_name(ea),
                (unsigned long long)v, cpu->pc, cpu->lr);
    }
    return v;
}

void hw_write(CPUState *cpu, u32 ea, u64 v, u8 size) {
    hw_writes++;
    u32 off = ea & 0xFFFF;
    if (g_logged < (unsigned)hw_log_limit && seen_once(ea, cpu->pc, 1)) {
        g_logged++;
        fprintf(stderr, "[mmio] W%u %08X (%s) = %llX pc=%08X\n", size * 8, ea, hw_name(ea),
                (unsigned long long)v, cpu->pc);
    }
    switch (off & ~1u) {
    case 0x500A:                            /* DSP CSR: interrupt bits are write-1-to-clear */
        if (size == 2) { u64 old = get(off, 2); v = (v & ~0xA8u) | (old & 0xA8u & ~v); }
        break;
    }
    put(off, v, size);
}
