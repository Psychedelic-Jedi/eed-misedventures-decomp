#pragma once
#include "cpu/cpu.h"
extern unsigned long long irq_count;
void irq_take_external(CPUState *cpu);
int irq_check_on_enable(CPUState *cpu, u32 resume_pc);
extern int irq_trace;
int irq_poll(CPUState *cpu);
u32 dec_read(CPUState *cpu);
void dec_write(CPUState *cpu, u32 v);
u64 dec_next_tb(void);        /* time base when the decrementer fires (0 = not armed) */
