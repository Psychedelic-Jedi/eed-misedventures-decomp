#pragma once
#include "cpu/cpu.h"
extern CPUState cpu;
int eed_core_init(const u8 *dol, u32 dol_len, const char *disc_path);
int eed_core_run(volatile int *stop);
void eed_core_debug_dump(void);
