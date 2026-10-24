/* Boot test harness: loads main.dol into guest RAM and runs the statically
 * recompiled code from the entry point, logging hardware (MMIO) accesses.
 * Build: see runtime/README.md. Not a real runtime yet. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include "generated.h"
#include "hle.h"
#include "hw.h"

/* dolrecomp_call_depth is defined in cpu.c */

static u32 g_spr[1024];
static u32 spr_read(CPUState *cpu, u16 spr, u32 cia) {
    (void)cpu; (void)cia;
    if (spr == 287) return PPC_GEKKO_PVR;   /* PVR */
    return g_spr[spr & 1023];
}
static void spr_write(CPUState *cpu, u16 spr, u32 v, u32 cia) {
    (void)cpu; (void)cia;
    g_spr[spr & 1023] = v;
}

/* Instructions DolRecomp leaves to the host (HID0/HID1/L2CR/WPAR etc. SPR moves, cache ops). */
static unsigned g_fallback_logged;
static void fallback(CPUState *cpu, u32 raw, u32 cia) {
    if (raw == HLE_MARK && hle_dispatch(cpu, cia))
        return;
    u32 op = raw >> 26, xo = (raw >> 1) & 0x3FF, rd = (raw >> 21) & 31;
    u32 spr = ((raw >> 16) & 31) | (((raw >> 11) & 31) << 5);
    if (op == 31 && xo == 339) {            /* mfspr */
        cpu->gpr[rd] = spr_read(cpu, (u16)spr, cia);
    } else if (op == 31 && xo == 467) {     /* mtspr */
        spr_write(cpu, (u16)spr, cpu->gpr[rd], cia);
    } else if (op == 31 && (xo == 982 || xo == 86 || xo == 54 || xo == 470 || xo == 246 || xo == 1014)) {
        /* icbi / dcbf / dcbst / dcbi / dcbt / dcbz_l: no-op on host (dcbz handled by generator) */
    } else {
        if (g_fallback_logged++ < 50)
            fprintf(stderr, "[fallback] unhandled insn %08X at %08X\n", raw, cia);
    }
    cpu->pc = cia + 4;
}

/* Time base runs at bus clock / 4 = 40.5 MHz, driven by the host's real-time clock. */
static struct timespec g_t0;
static void update_timebase(CPUState *cpu) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    u64 ns = (u64)(t.tv_sec - g_t0.tv_sec) * 1000000000ull + (u64)(t.tv_nsec - g_t0.tv_nsec);
    cpu->timebase = ns * 81ull / 2000ull; /* 40.5e6 / 1e9 */
}

static u32 be32(const unsigned char *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static CPUState cpu;
static void on_alarm(int s) {
    (void)s;
    fprintf(stderr, "\n[timeout] pc=%08X lr=%08X r1=%08X mmio R=%llu W=%llu\n", cpu.pc, cpu.lr,
            cpu.gpr[1], hw_reads, hw_writes);
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s main.dol [seconds]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) return 1;
    fclose(f);

    if (!cpu_init(&cpu)) { fprintf(stderr, "cpu_init failed\n"); return 1; }
    cpu.external_read = hw_read;
    cpu.external_write = hw_write;
    cpu.spr_read = spr_read;
    cpu.spr_write = spr_write;
    cpu.instruction_fallback = fallback;

    /* BSS first: the DOL's BSS range spans .sdata/.sdata2, which are loaded on top. */
    u32 bss = be32(d + 0xD8), bss_sz = be32(d + 0xDC);
    for (u32 j = 0; j < bss_sz; j++) mem_write8(&cpu, bss + j, 0);
    for (int i = 0; i < 18; i++) {
        u32 off = be32(d + i * 4), addr = be32(d + 0x48 + i * 4), sz = be32(d + 0x90 + i * 4);
        for (u32 j = 0; j < sz; j++) mem_write8(&cpu, addr + j, d[off + j]);
    }

    /* Minimal low-memory boot info normally written by the apploader/IPL. */
    mem_write32(&cpu, 0x80000020, 0x0D15EA5E);           /* boot magic */
    mem_write32(&cpu, 0x80000024, 1);                    /* version */
    mem_write32(&cpu, 0x80000028, 24 * 1024 * 1024);     /* physical memory size */
    mem_write32(&cpu, 0x8000002C, 1);                    /* console type: retail */
    mem_write32(&cpu, 0x800000F0, 24 * 1024 * 1024);     /* simulated memory size */
    mem_write32(&cpu, 0x800000F8, 162000000);            /* bus clock */
    mem_write32(&cpu, 0x800000FC, 486000000);            /* core clock */

    /* MSR as left by the IPL: FP available, address translation on, recoverable. */
    cpu.msr = 0x00002032u;
    cpu.pc = DOLRECOMP_ENTRY_POINT;
    fprintf(stderr, "[boot] entry %08X\n", cpu.pc);
    signal(SIGALRM, on_alarm);
    alarm(argc > 2 ? atoi(argv[2]) : 10);

    unsigned long long blocks = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    for (;;) {
        cpu.downcount = 0;
        update_timebase(&cpu);
        if (!dolrecomp_call(&cpu, cpu.pc)) {
            fprintf(stderr, "[stop] no code at pc=%08X lr=%08X after %llu blocks\n", cpu.pc, cpu.lr, blocks);
            break;
        }
        if (cpu.exception == PPC_EXC_SYSTEM_CALL) {
            /* Dolphin OS's 0xC00 handler only syncs caches (used by DCFlushRange etc.). */
            cpu.exception = 0;
            cpu.pc = cpu.srr0;
            continue;
        }
        if (cpu.exception == PPC_EXC_FP_UNAVAILABLE) {
            /* Dolphin OS enables FP lazily in its 0x800 handler (OSSwitchFPUContext). */
            cpu.exception = 0;
            cpu.msr |= 0x2000u;
            cpu.pc = cpu.srr0;
            continue;
        }
        if (cpu.exception) {
            fprintf(stderr, "[stop] exception %08X (program %08X) at pc=%08X srr0=%08X srr1=%08X lr=%08X msr=%08X\n", cpu.exception, cpu.program_exception, cpu.pc, cpu.srr0, cpu.srr1, cpu.lr, cpu.msr);
            break;
        }
        blocks++;
    }
    fprintf(stderr, "mmio R=%llu W=%llu\n", hw_reads, hw_writes);
    return 0;
}
