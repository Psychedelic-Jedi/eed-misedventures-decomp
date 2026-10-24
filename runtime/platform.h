#pragma once
/* Hooks each platform front end provides to the runtime core. */
#include "cpu/cpu.h"
/* A finished frame (EFB -> XFB copy): RGBA8 rows of `stride` pixels, region x,y,w,h. */
void plat_present_frame(const u8 *rgba, int stride, int x, int y, int w, int h);
/* Refresh g_pad[] (input.h) for the given VI frame. */
void plat_input_update(unsigned long long frame);
/* Optional embedded data (Android build): NULL if not embedded. */
const u8 *plat_embedded_module(const char *name, u32 *size);
/* GLES backend: a finished frame lives in the EFB color texture `tex` (tex_w x tex_h,
 * GL convention: row 0 at the bottom). The XFB region is x,y,w,h in texture pixels,
 * measured top-down. The backend has unbound its framebuffer/buffers before calling;
 * the front end may draw to the window and swap. */
void plat_present_gl(unsigned tex, int tex_w, int tex_h, int x, int y, int w, int h);
