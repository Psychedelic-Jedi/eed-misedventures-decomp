#include "irq.h"
#include "hw.h"
#include <stdio.h>
int irq_trace = 1;

/* Deliver an external interrupt the way the OS's 0x500 vector stub would:
 * save volatile state into the current OSContext, then enter the (recompiled)
 * ExternalInterruptHandler(exception=4, context). It returns via OSLoadContext/rfi. */
#define OS_CURRENT_CONTEXT 0x800000D4u
#define EXTERNAL_INTERRUPT_HANDLER 0x80147E10u
unsigned long long irq_count;
#define DECREMENTER_HANDLER 0x80143388u
static void take_exception(CPUState *cpu, u32 exc, u32 handler) {
    u32 ctx = mem_read32(cpu, OS_CURRENT_CONTEXT);
    if (irq_trace && irq_count < 40)
        fprintf(stderr, "[irq] #%llu exc %u: ctx=%08X pc=%08X msr=%08X\n", irq_count, exc, ctx, cpu->pc, cpu->msr);
    irq_count++;
    mem_write32(cpu, ctx + 0x0C, cpu->gpr[3]);
    mem_write32(cpu, ctx + 0x10, cpu->gpr[4]);
    mem_write32(cpu, ctx + 0x14, cpu->gpr[5]);
    mem_write16(cpu, ctx + 0x1A2, mem_read16(cpu, ctx + 0x1A2) | 2);
    mem_write32(cpu, ctx + 0x80, cpu->cr);
    mem_write32(cpu, ctx + 0x84, cpu->lr);
    mem_write32(cpu, ctx + 0x88, cpu->ctr);
    mem_write32(cpu, ctx + 0x8C, cpu->xer);
    mem_write32(cpu, ctx + 0x198, cpu->pc);
    mem_write32(cpu, ctx + 0x19C, cpu->msr);
    cpu->srr0 = cpu->pc;
    cpu->srr1 = cpu->msr;
    cpu->msr &= ~0x8000u;          /* EE off while handling */
    cpu->gpr[3] = exc;
    cpu->gpr[4] = ctx;
    cpu->pc = handler;
}

void irq_take_external(CPUState *cpu) { take_exception(cpu, 4, EXTERNAL_INTERRUPT_HANDLER); }

/* ---- Decrementer: counts down at the time-base rate; exception when it passes zero ---- */
static u32 g_dec_value;
static u64 g_dec_tb;
static int g_dec_armed;
u32 dec_read(CPUState *cpu) { return g_dec_value - (u32)(cpu->timebase - g_dec_tb); }
void dec_write(CPUState *cpu, u32 v) {
    g_dec_value = v;
    g_dec_tb = cpu->timebase;
    g_dec_armed = 1;
}
static int dec_pending(CPUState *cpu) { return g_dec_armed && (s32)dec_read(cpu) < 0; }

/* Called at block boundaries: deliver the highest-priority pending interrupt. */
int irq_poll(CPUState *cpu) {
    if (!(cpu->msr & 0x8000u)) return 0;
    if (hw_interrupt_pending()) { irq_take_external(cpu); return 1; }
    if (dec_pending(cpu)) {
        g_dec_armed = 0;
        take_exception(cpu, 8, DECREMENTER_HANDLER);
        return 1;
    }
    return 0;
}


/* Called when guest code enables interrupts: deliver a pending one immediately,
 * resuming at `resume_pc` afterwards. */
int irq_check_on_enable(CPUState *cpu, u32 resume_pc) {
    u32 saved = cpu->pc;
    cpu->pc = resume_pc;
    if (irq_poll(cpu)) return 1;
    cpu->pc = saved;
    return 0;
}
