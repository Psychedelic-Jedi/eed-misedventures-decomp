/* Minimal GameCube hardware register model (0xCC000000 MMIO).
 * Registers read back what was written, with a few "always ready" quirks so
 * SDK polling loops complete. Real behaviour (DVD, VI, GX FIFO, SI pads) is
 * replaced at the SDK level over time (see hle.c). */
#include <stdio.h>
#include <string.h>
#include "hw.h"
#include "disc.h"
#include "gx_fifo.h"
#include <stdlib.h>

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

/* ---- DVD interface (DI, 0xCC006000) ---- */
#define DISR   0x6000
#define DICVR  0x6004
#define DICMD0 0x6008
#define DICMD1 0x600C
#define DICMD2 0x6010
#define DIMAR  0x6014
#define DILEN  0x6018
#define DICR   0x601C
#define DIIMM  0x6020
#define DI_TCINT 0x10u
#define DI_TCMSK 0x08u
#define DI_DEINT 0x04u
#define DI_DEMSK 0x02u
int di_trace = 1;

static void di_execute(CPUState *cpu) {
    u32 cmd = (u32)get(DICMD0, 4), a = (u32)get(DICMD1, 4), b = (u32)get(DICMD2, 4);
    u32 mar = (u32)get(DIMAR, 4) | 0x80000000u, len = (u32)get(DILEN, 4);
    switch (cmd >> 24) {
    case 0xA8: { /* read: disc offset = a << 2, length = b, DMA to mar */
        u32 off = a << 2;
        if (di_trace) fprintf(stderr, "[di] read disc 0x%08X len 0x%X -> %08X\n", off, len, mar);
        u8 *buf = malloc(len ? len : 1);
        disc_read(off, buf, len);
        for (u32 i = 0; i < len; i++) mem_write8(cpu, mar + i, buf[i]);
        free(buf);
        (void)b;
        break;
    }
    case 0x12: /* inquiry: drive info */
        for (u32 i = 0; i < len; i++) mem_write8(cpu, mar + i, 0);
        mem_write16(cpu, mar + 2, 0x0002);  /* revision */
        mem_write32(cpu, mar + 4, 0x20060526); /* release date */
        break;
    case 0xE0: put(DIIMM, 0, 4); break; /* request error: none */
    default:
        if (di_trace) fprintf(stderr, "[di] command %08X %08X %08X (completed)\n", cmd, a, b);
        break;
    }
    put(DILEN, 0, 4);
    put(DICR, get(DICR, 4) & ~1ull, 4);
    put(DISR, get(DISR, 4) | DI_TCINT, 4);
}

/* ---- ARAM (16 MB auxiliary RAM) + its DMA engine (DSP registers 0x5020-0x502A) ---- */
#define ARAM_SIZE (16u << 20)
static u8 *g_aram;
#define DSP_CSR 0x500A
#define CSR_AIDINT 0x08u
#define CSR_AIDMSK 0x10u
#define CSR_ARINT  0x20u
#define CSR_ARMSK  0x40u
#define CSR_DSPINT 0x80u
#define CSR_DSPMSK 0x100u
int aram_trace = 0;

static void aram_dma(CPUState *cpu) {
    if (!g_aram) g_aram = calloc(1, ARAM_SIZE);
    u32 mm = ((u32)get(0x5020, 4) & 0x03FFFFE0u) | 0x80000000u;
    u32 ar = (u32)get(0x5024, 4) & (ARAM_SIZE - 1) & ~31u;
    u32 cnt = (u32)get(0x5028, 4);
    u32 n = cnt & 0x7FFFFFE0u, to_mram = cnt >> 31;
    if (aram_trace) fprintf(stderr, "[aram] %s mram %08X aram %07X len 0x%X\n", to_mram ? "ARAM->MRAM" : "MRAM->ARAM", mm, ar, n);
    for (u32 i = 0; i < n && ar + i < ARAM_SIZE; i++) {
        if (to_mram) mem_write8(cpu, mm + i, g_aram[ar + i]);
        else g_aram[ar + i] = mem_read8(cpu, mm + i);
    }
    put(0x5028, cnt & 0x80000000u, 4);                  /* count reads back 0 when done */
    put(DSP_CSR, get(DSP_CSR, 2) | CSR_ARINT, 2);       /* ARAM DMA complete */
}

/* PI interrupt cause register, computed from device state. */
#define PI_INTSR 0x3000
#define PI_INTMR 0x3004
#define PI_DI    0x00000004u
#define PI_DSP   0x00000040u
#define PI_VI    0x00000100u
#define PI_PE_TOKEN  0x00000200u
#define PI_PE_FINISH 0x00000400u
#define PE_CTRL  0x100A   /* bit0 token int enable, bit1 finish int enable, bit2 token, bit3 finish */
#define PE_TOKEN 0x100E
#define VI_DI0   0x2030
#define VI_DI1   0x2034
#define VI_INT   0x80000000u
#define VI_ENB   0x10000000u
#define PI_RSWST 0x00010000u  /* reset switch not pressed */
static u32 pi_cause(void) {
    u32 c = (u32)get(PI_INTSR, 4) & ~(PI_DI | PI_VI | PI_PE_TOKEN | PI_PE_FINISH | PI_DSP);
    u32 csr = (u32)get(DSP_CSR, 2);
    if (((csr & CSR_ARINT) && (csr & CSR_ARMSK)) || ((csr & CSR_AIDINT) && (csr & CSR_AIDMSK)) ||
        ((csr & CSR_DSPINT) && (csr & CSR_DSPMSK))) c |= PI_DSP;
    u32 pe = (u32)get(PE_CTRL, 2);
    if (pe_token_pending && (pe & 1)) c |= PI_PE_TOKEN;
    if (pe_finish_pending && (pe & 2)) c |= PI_PE_FINISH;
    for (u32 r = VI_DI0; r <= VI_DI1; r += 4)
        if (((u32)get(r, 4) & (VI_INT | VI_ENB)) == (VI_INT | VI_ENB)) c |= PI_VI;
    u32 disr = (u32)get(DISR, 4);
    if (((disr & DI_TCINT) && (disr & DI_TCMSK)) || ((disr & DI_DEINT) && (disr & DI_DEMSK))) c |= PI_DI;
    return c | PI_RSWST;
}

int hw_interrupt_pending(void) {
    return (pi_cause() & (u32)get(PI_INTMR, 4) & ~PI_RSWST) != 0;
}

static unsigned g_unmapped_logged;
static int unmapped(CPUState *cpu, u32 ea, u8 size, int w, u64 v) {
    if ((ea & 0xFFFF0000u) == 0xCC000000u) return 0;
    if (g_unmapped_logged++ < 40)
        fprintf(stderr, "[mem] unmapped %s%u %08X%s pc=%08X lr=%08X\n", w ? "W" : "R", size * 8, ea,
                w ? "" : "", cpu->pc, cpu->lr);
    (void)v;
    return 1;
}

u64 hw_read(CPUState *cpu, u32 ea, u8 size) {
    if (unmapped(cpu, ea, size, 0, 0)) return 0;
    hw_reads++;
    u32 off = ea & 0xFFFF;
    u64 v = get(off, size);
    switch (off & ~1u) {
    case 0x500A: v &= ~0x0200u; break;      /* DSP CSR: ARAM DMA never busy */
    case 0x5016: v = 1; break;              /* AR_MODE: ARAM ready */
    case PI_INTSR: case PI_INTSR + 2:
        v = pi_cause(); if (size == 2) v = (off & 2) ? (v & 0xFFFF) : (v >> 16);
        break;
    case DICVR: case DICVR + 2: v &= ~1ull; break; /* lid closed */
    case PE_CTRL:
        v = (get(PE_CTRL, 2) & 3) | (pe_token_pending ? 4 : 0) | (pe_finish_pending ? 8 : 0);
        break;
    case PE_TOKEN: v = pe_token_value; break;
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
    if (unmapped(cpu, ea, size, 1, v)) return;
    hw_writes++;
    if ((ea & 0xFFFFF000u) == 0xCC008000u) { /* GX write-gather pipe: any offset feeds the FIFO */
        u8 b[8];
        for (int i = 0; i < size; i++) b[i] = (u8)(v >> ((size - 1 - i) * 8));
        gx_fifo_write(b, size);
        return;
    }
    u32 off = ea & 0xFFFF;
    if (g_logged < (unsigned)hw_log_limit && seen_once(ea, cpu->pc, 1)) {
        g_logged++;
        fprintf(stderr, "[mmio] W%u %08X (%s) = %llX pc=%08X\n", size * 8, ea, hw_name(ea),
                (unsigned long long)v, cpu->pc);
    }
    if ((size == 2 && off == 0x502A) || (size == 4 && off == 0x5028)) {
        put(off, v, size);
        aram_dma(cpu);
        return;
    }
    if (size == 2 && off == PE_CTRL) {      /* status bits are write-1-to-clear */
        if (v & 4) pe_token_pending = 0;
        if (v & 8) pe_finish_pending = 0;
        put(PE_CTRL, v & 3, 2);
        return;
    }
    if (size == 4 && off == DISR) {         /* interrupt status bits are write-1-to-clear */
        u64 old = get(DISR, 4);
        u64 w1c = DI_TCINT | DI_DEINT | 0x40;
        put(DISR, (v & ~w1c) | (old & w1c & ~v), 4);
        return;
    }
    if (size == 4 && off == DICVR) {
        u64 old = get(DICVR, 4);
        put(DICVR, (v & ~4ull) | (old & 4ull & ~v), 4);
        return;
    }
    if (size == 4 && off == DICR) {
        put(DICR, v, 4);
        if (v & 1) di_execute(cpu);
        return;
    }
    switch (off & ~1u) {
    case 0x500A:                            /* DSP CSR: interrupt bits are write-1-to-clear */
        if (size == 2) { u64 old = get(off, 2); v = (v & ~0xA8u) | (old & 0xA8u & ~v); }
        break;
    }
    put(off, v, size);
}

void hw_debug_dump(void) {
    fprintf(stderr, "[hw] DISR=%08llX DICR=%08llX PI_INTSR(cause)=%08X PI_INTMR=%08llX pending=%d\n",
            (unsigned long long)get(DISR, 4), (unsigned long long)get(DICR, 4), pi_cause(),
            (unsigned long long)get(PI_INTMR, 4), hw_interrupt_pending());
}

/* Vertical retrace every 1/60 s of time base (40.5 MHz): raise VI display interrupts. */
unsigned long long vi_frames;
static u64 next;
u64 hw_next_tick_tb(void) { return next; }
void hw_tick(CPUState *cpu) {
    if (cpu->timebase < next) return;
    next = cpu->timebase + 675000;
    vi_frames++;
    for (u32 r = VI_DI0; r <= VI_DI1; r += 4) {
        u32 v = (u32)get(r, 4);
        if (v & VI_ENB) put(r, v | VI_INT, 4);
    }
}
