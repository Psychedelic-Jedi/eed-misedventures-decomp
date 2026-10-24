#pragma once
/* GX (GameCube GPU) state shared by the command-stream front end (gx_fifo.c)
 * and the renderer (gx_render.c). Register layouts follow the hardware; see
 * YAGCD and Dolphin's VideoCommon for references. */
#include "cpu/cpu.h"

typedef struct {
    /* CP (command processor) */
    u32 vcd_lo, vcd_hi;
    u32 vat_a[8], vat_b[8], vat_c[8];
    u32 array_base[16], array_stride[16];
    u32 matidx_a, matidx_b;          /* CP copies of XF 0x1018/0x1019 */
    /* XF (transform unit) */
    float xfmem[0x680];              /* 0x000-0x3FF pos/tex matrices, 0x400 normal, 0x500 post, 0x600 lights */
    u32 xfreg[0x100];                /* XF registers 0x1000-0x10FF */
    /* BP (pixel pipeline) */
    u32 bp[0x100];
} GXState;

extern GXState gx;
extern CPUState *gx_cpu;            /* for reading guest memory (arrays, display lists, textures) */

/* Called by the front end. */
void gx_render_draw(u8 prim, u8 vat, u16 count, const u8 *data);
void gx_render_bp(u8 reg, u32 value); /* after gx.bp[reg] is updated */
void gx_render_init(void);
extern unsigned long long gx_frames;
