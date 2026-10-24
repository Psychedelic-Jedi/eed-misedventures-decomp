#pragma once
#include <stdbool.h>
#include "cpu/cpu.h"
#define MODULE_WINDOW_BASE 0x81800000u
#define MODULE_WINDOW_END  0x82000000u
typedef void (*DolRecompFunction)(CPUState *ctx);
extern DolRecompFunction (*engine_find)(u32);
extern const char *module_dir;
extern int module_trace;
bool module_host_call(CPUState *cpu, u32 addr);
void module_relocate_hook(CPUState *cpu);
void module_fixup_hook(CPUState *cpu);
