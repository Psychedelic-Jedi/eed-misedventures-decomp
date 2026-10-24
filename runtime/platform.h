#pragma once
/* Hooks each platform front end provides to the runtime core. */
#include "cpu/cpu.h"
/* A finished frame (EFB -> XFB copy): RGBA8 rows of `stride` pixels, region x,y,w,h. */
void plat_present_frame(const u8 *rgba, int stride, int x, int y, int w, int h);
/* Refresh g_pad[] (input.h) for the given VI frame. */
void plat_input_update(unsigned long long frame);
/* Optional embedded data (Android build): NULL if not embedded. */
const u8 *plat_embedded_module(const char *name, u32 *size);
