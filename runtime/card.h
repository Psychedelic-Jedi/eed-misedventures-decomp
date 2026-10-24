#pragma once
#include "cpu/cpu.h"
void card_GetResultCode(CPUState *c);
void card_FreeBlocks(CPUState *c);
void card_CheckAsync(CPUState *c);
void card_ProbeEx(CPUState *c);
void card_MountAsync(CPUState *c);
void card_Mount(CPUState *c);
void card_Unmount(CPUState *c);
void card_FormatAsync(CPUState *c);
void card_Open(CPUState *c);
void card_Close(CPUState *c);
void card_CreateAsync(CPUState *c);
void card_ReadAsync(CPUState *c);
void card_WriteAsync(CPUState *c);
void card_DeleteAsync(CPUState *c);
void card_GetStatus(CPUState *c);
void card_SetStatus(CPUState *c);
