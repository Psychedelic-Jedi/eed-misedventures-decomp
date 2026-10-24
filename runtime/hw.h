#pragma once
#include "cpu/cpu.h"
extern unsigned long long hw_reads, hw_writes;
extern int hw_log_limit;
u64 hw_read(CPUState *cpu, u32 ea, u8 size);
void hw_write(CPUState *cpu, u32 ea, u64 v, u8 size);
