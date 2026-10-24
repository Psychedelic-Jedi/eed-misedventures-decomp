#pragma once
/* Internal interface between the GX front half (gx_render.c: vertex decode,
 * transform, lighting, texture decode) and the raster backends: the software
 * rasterizer (gx_render.c) and the GLES backend (gx_gles.c). */
#include "gx.h"

#define EFB_W 640
#define EFB_H 528

/* A transformed vertex: clip-space position, texgen output (s, t, q), lit colors. */
typedef struct { float x, y, z, w; float tc[8][3]; float col[2][4]; } SVtx;

extern u8 tmem[1 << 20];             /* TLUT memory (palettes) */
extern int g_cull_flip;
extern float tev_reg[4][4];          /* TEV PREV/C0/C1/C2 initial values */
extern float tev_konst[4][4];        /* TEV konst colors K0-K3 */
u8 *decode_texture(u32 addr, int w, int h, u32 fmt, u32 tlut_off, u32 tlut_fmt);   /* malloc'ed RGBA8 */

/* GLES backend (enabled by gx_use_gles; needs a current GL ES context on the game thread). */
extern int gx_use_gles;
extern int gx_gles_scale;            /* EFB resolution multiplier */
void gles_draw(u8 prim, const SVtx *v, int count);
void gles_bp(u8 reg, u32 value);     /* EFB copies / clears */
/* Read the EFB region back as top-down RGBA8 at gx_gles_scale (Linux test harness). */
void gles_read_efb(u8 *out, int x, int y, int w, int h);
