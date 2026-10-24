#pragma once
#include "cpu/cpu.h"
#define HLE_MARK 0x7C10FAA6u /* mfspr r0, HID0 at a patched function entry */
extern int hle_trace;
int hle_dispatch(CPUState *cpu, u32 cia);
