#pragma once
#include "cpu/cpu.h"
extern unsigned long long hw_reads, hw_writes;
extern int hw_log_limit;
u64 hw_read(CPUState *cpu, u32 ea, u8 size);
void hw_write(CPUState *cpu, u32 ea, u64 v, u8 size);
extern int di_trace;
int hw_interrupt_pending(void);
void hw_debug_dump(void);
extern unsigned long long vi_frames;
void hw_tick(CPUState *cpu);
