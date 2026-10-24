#pragma once
#include "cpu/cpu.h"
extern unsigned long long gx_cmds, gx_draws, gx_draw_done, gx_tokens;
extern int gx_trace;
extern u32 pe_finish_pending, pe_token_pending;
extern u16 pe_token_value;
void gx_fifo_write(const u8 *data, u32 n);
