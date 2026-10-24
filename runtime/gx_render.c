/* GX renderer (software reference backend).
 *
 * Decodes vertices from the command stream, transforms them with the XF state,
 * decodes GameCube textures, evaluates the TEV combiner per pixel and rasterizes
 * into a 640x528 EFB. EFB->XFB copies mark finished frames, which can be written
 * as PPM images (EED_FRAMES=<dir>). The vertex/texture/TEV front half is meant to
 * be shared with the GLES backend used on Android.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx.h"

#define EFB_W 640
#define EFB_H 528

static u8 efb[EFB_H][EFB_W][4];
static float zbuf[EFB_H][EFB_W];
static u8 tmem[1 << 20];
unsigned long long gx_frames;
static const char *g_frame_dir;
static int g_frame_every = 30;
static int g_cull_flip;

void gx_render_init(void) {
    gx.bp[0xFE] = 0xFFFFFF;
    g_frame_dir = getenv("EED_FRAMES");
    if (getenv("EED_FRAME_EVERY")) g_frame_every = atoi(getenv("EED_FRAME_EVERY"));
    g_cull_flip = getenv("EED_CULL_FLIP") == NULL;  /* verified against the outline pass in Scam 1 */
    for (int y = 0; y < EFB_H; y++) for (int x = 0; x < EFB_W; x++) zbuf[y][x] = 1.0f;
}

/* ------------------------------------------------------------------ memory */
static u8 rd8(u32 a) { return mem_read8(gx_cpu, a | 0x80000000u); }

typedef struct { const u8 *p; u32 addr; int mem; } Reader;
static u8 rb(Reader *r) { return r->mem ? rd8(r->addr++) : *r->p++; }
static u16 rh(Reader *r) { u16 a = rb(r); return (u16)(a << 8 | rb(r)); }
static u32 rw(Reader *r) { u32 a = rh(r); return a << 16 | rh(r); }

static float rcomp(Reader *r, u32 fmt, u32 frac) {
    float s = 1.0f / (float)(1u << frac);
    switch (fmt) {
    case 0: return rb(r) * s;
    case 1: return (s8)rb(r) * s;
    case 2: return rh(r) * s;
    case 3: return (s16)rh(r) * s;
    default: { u32 w = rw(r); float f; memcpy(&f, &w, 4); return f; }
    }
}

static void rcolor(Reader *r, u32 fmt, u8 out[4]) {
    u32 v;
    switch (fmt) {
    case 0: v = rh(r); out[0] = (u8)(((v >> 11) & 31) * 255 / 31); out[1] = (u8)(((v >> 5) & 63) * 255 / 63);
            out[2] = (u8)((v & 31) * 255 / 31); out[3] = 255; break;
    case 1: out[0] = rb(r); out[1] = rb(r); out[2] = rb(r); out[3] = 255; break;
    case 2: out[0] = rb(r); out[1] = rb(r); out[2] = rb(r); rb(r); out[3] = 255; break;
    case 3: v = rh(r); out[0] = (u8)(((v >> 12) & 15) * 17); out[1] = (u8)(((v >> 8) & 15) * 17);
            out[2] = (u8)(((v >> 4) & 15) * 17); out[3] = (u8)((v & 15) * 17); break;
    case 4: v = (u32)rb(r) << 16; v |= (u32)rb(r) << 8; v |= rb(r);
            out[0] = (u8)(((v >> 18) & 63) * 255 / 63); out[1] = (u8)(((v >> 12) & 63) * 255 / 63);
            out[2] = (u8)(((v >> 6) & 63) * 255 / 63); out[3] = (u8)((v & 63) * 255 / 63); break;
    default: out[0] = rb(r); out[1] = rb(r); out[2] = rb(r); out[3] = rb(r); break;
    }
}

/* ------------------------------------------------------------------ vertices */
typedef struct {
    float pos[3], nrm[3], tc[8][2];
    int has_nrm;
    u8 col[2][4];
    u8 pnmtx, texmtx[8];
    int has_col[2];
} Vtx;

typedef struct { float x, y, z, w; float tc[8][3]; float col[2][4]; } SVtx; /* transformed */

/* Attribute reader: direct data from the stream, or an index into a CP array. */
static Reader attr_reader(Reader *stream, u32 type, u32 array) {
    Reader r = {0};
    if (type == 1) return *stream;
    u32 idx = type == 2 ? rb(stream) : rh(stream);
    r.mem = 1;
    r.addr = gx.array_base[array] + idx * gx.array_stride[array];
    return r;
}

static void decode_vertex(Reader *s, u32 vat, Vtx *v) {
    u32 a = gx.vat_a[vat], b = gx.vat_b[vat], c = gx.vat_c[vat];
    u32 lo = gx.vcd_lo, hi = gx.vcd_hi;
    int bytedq = (a >> 30) & 1;
    memset(v, 0, sizeof *v);
    v->pnmtx = (u8)(gx.xfreg[0x18] & 0x3F);
    for (int i = 0; i < 8; i++)
        v->texmtx[i] = (u8)(i < 4 ? (gx.xfreg[0x18] >> (6 + 6 * i)) & 0x3F : (gx.xfreg[0x19] >> (6 * (i - 4))) & 0x3F);
    if (lo & 1) v->pnmtx = rb(s) & 0x3F;
    for (int i = 0; i < 8; i++) if ((lo >> (1 + i)) & 1) v->texmtx[i] = rb(s) & 0x3F;

    u32 t = (lo >> 9) & 3;
    if (t) {
        Reader r = attr_reader(s, t, 0);
        u32 fmt = (a >> 1) & 7, frac = (a >> 4) & 31;
        if (fmt <= 1 && !bytedq) frac = 0;
        v->pos[0] = rcomp(&r, fmt, frac); v->pos[1] = rcomp(&r, fmt, frac);
        v->pos[2] = (a & 1) ? rcomp(&r, fmt, frac) : 0.0f;
        if (t == 1) *s = r;
    }
    t = (lo >> 11) & 3;
    if (t) {                                       /* normals: read and discard (lighting TODO) */
        u32 fmt = (a >> 10) & 7, n = ((a >> 9) & 1) ? 9 : 3;
        u32 frac = fmt == 1 ? 6 : fmt == 3 ? 14 : 0;
        if (t >= 2 && n == 9 && ((a >> 31) & 1)) {
            for (int k = 0; k < 3; k++) { Reader r = attr_reader(s, t, 1); for (int j = 0; j < 3; j++) { float f = rcomp(&r, fmt, frac); if (k == 0) v->nrm[j] = f; } }
            v->has_nrm = 1;
        } else {
            Reader r = attr_reader(s, t, 1);
            for (u32 j = 0; j < n; j++) { float f = rcomp(&r, fmt, frac); if (j < 3) v->nrm[j] = f; }
            v->has_nrm = 1;
            if (t == 1) *s = r;
        }
    }
    for (int ci = 0; ci < 2; ci++) {
        t = (lo >> (13 + 2 * ci)) & 3;
        if (!t) continue;
        Reader r = attr_reader(s, t, 2 + ci);
        rcolor(&r, (a >> (14 + 4 * ci)) & 7, v->col[ci]);
        v->has_col[ci] = 1;
        if (t == 1) *s = r;
    }
    u32 tcnt[8], tfmt[8], tfrac[8];
    tcnt[0] = (a >> 21) & 1; tfmt[0] = (a >> 22) & 7; tfrac[0] = (a >> 25) & 31;
    tcnt[1] = b & 1;         tfmt[1] = (b >> 1) & 7;  tfrac[1] = (b >> 4) & 31;
    tcnt[2] = (b >> 9) & 1;  tfmt[2] = (b >> 10) & 7; tfrac[2] = (b >> 13) & 31;
    tcnt[3] = (b >> 18) & 1; tfmt[3] = (b >> 19) & 7; tfrac[3] = (b >> 22) & 31;
    tcnt[4] = (b >> 27) & 1; tfmt[4] = (b >> 28) & 7; tfrac[4] = c & 31;
    tcnt[5] = (c >> 5) & 1;  tfmt[5] = (c >> 6) & 7;  tfrac[5] = (c >> 9) & 31;
    tcnt[6] = (c >> 14) & 1; tfmt[6] = (c >> 15) & 7; tfrac[6] = (c >> 18) & 31;
    tcnt[7] = (c >> 23) & 1; tfmt[7] = (c >> 24) & 7; tfrac[7] = (c >> 27) & 31;
    for (int i = 0; i < 8; i++) {
        t = (hi >> (2 * i)) & 3;
        if (!t) continue;
        Reader r = attr_reader(s, t, 4 + i);
        u32 frac = (tfmt[i] <= 1 && !bytedq) ? 0 : tfrac[i];
        v->tc[i][0] = rcomp(&r, tfmt[i], frac);
        v->tc[i][1] = tcnt[i] ? rcomp(&r, tfmt[i], frac) : 0.0f;
        if (t == 1) *s = r;
    }
}

static const float *mtx(u32 idx) { return &gx.xfmem[(idx & 0x3F) * 4]; }
static float xff(u32 reg) { float f; memcpy(&f, &gx.xfreg[reg], 4); return f; }

static void transform(const Vtx *v, SVtx *o) {
    const float *m = mtx(v->pnmtx);
    float x = m[0] * v->pos[0] + m[1] * v->pos[1] + m[2] * v->pos[2] + m[3];
    float y = m[4] * v->pos[0] + m[5] * v->pos[1] + m[6] * v->pos[2] + m[7];
    float z = m[8] * v->pos[0] + m[9] * v->pos[1] + m[10] * v->pos[2] + m[11];
    float p[6];
    for (int i = 0; i < 6; i++) p[i] = xff(0x20 + i);
    if (gx.xfreg[0x26] == 0) {                  /* perspective */
        o->x = p[0] * x + p[1] * z; o->y = p[2] * y + p[3] * z; o->z = p[4] * z + p[5]; o->w = -z;
    } else {                                    /* orthographic */
        o->x = p[0] * x + p[1]; o->y = p[2] * y + p[3]; o->z = p[4] * z + p[5]; o->w = 1.0f;
    }
    /* texture coordinate generation (XF 0x1040+i): matrix * source row, then optional
     * "dual texture" post-transform (XF 0x1050+i, enabled by XF 0x1012). Sources: 0 position,
     * 1 normal, 2 colours, 3/4 binormals, 5-12 TEX0-7. Toon shading uses normal -> ramp. */
    u32 ntex = gx.xfreg[0x3F] & 15, dual = gx.xfreg[0x12] & 1;
    for (u32 i = 0; i < 8; i++) {
        u32 tg = gx.xfreg[0x40 + i], row = (tg >> 7) & 31, abc1 = (tg >> 2) & 1;
        float in[4] = {0, 0, 1, 1};
        if (row == 0) { in[0] = v->pos[0]; in[1] = v->pos[1]; in[2] = v->pos[2]; }
        else if (row == 1) { in[0] = v->nrm[0]; in[1] = v->nrm[1]; in[2] = v->nrm[2]; }
        else if (row == 2) { in[0] = v->col[0][0] / 255.f; in[1] = v->col[0][1] / 255.f; in[2] = v->col[0][2] / 255.f; }
        else if (row >= 5 && row <= 12) { in[0] = v->tc[row - 5][0]; in[1] = v->tc[row - 5][1]; in[2] = 1; }
        if (!abc1) in[2] = 1.0f;
        const float *t = mtx(v->texmtx[i]);
        float st[3];
        st[0] = t[0] * in[0] + t[1] * in[1] + t[2] * in[2] + t[3];
        st[1] = t[4] * in[0] + t[5] * in[1] + t[6] * in[2] + t[7];
        st[2] = (tg & 2) ? t[8] * in[0] + t[9] * in[1] + t[10] * in[2] + t[11] : 1.0f;
        if (dual) {
            u32 post = gx.xfreg[0x50 + i];
            if ((post >> 8) & 1) {
                float l = sqrtf(st[0] * st[0] + st[1] * st[1] + st[2] * st[2]);
                if (l > 0) for (int k = 0; k < 3; k++) st[k] /= l;
            }
            const float *pm = &gx.xfmem[0x500 + (post & 0x3F) * 4];
            float r[3];
            for (int k = 0; k < 3; k++) r[k] = pm[k * 4] * st[0] + pm[k * 4 + 1] * st[1] + pm[k * 4 + 2] * st[2] + pm[k * 4 + 3];
            st[0] = r[0]; st[1] = r[1]; st[2] = (tg & 2) ? r[2] : 1.0f;
        }
        if (i >= ntex) { st[0] = v->tc[i][0]; st[1] = v->tc[i][1]; st[2] = 1; }
        o->tc[i][0] = st[0]; o->tc[i][1] = st[1]; o->tc[i][2] = st[2];
    }
    /* ---- color channels with hardware lighting (XF 0x100A-0x1011, lights at XF 0x600) ---- */
    float eye[3] = {x, y, z}, n[3] = {0, 0, 0};
    if (v->has_nrm) {
        const float *nm = &gx.xfmem[0x400 + (v->pnmtx & 31) * 3];
        for (int r = 0; r < 3; r++) n[r] = nm[r * 3] * v->nrm[0] + nm[r * 3 + 1] * v->nrm[1] + nm[r * 3 + 2] * v->nrm[2];
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0) for (int r = 0; r < 3; r++) n[r] /= l;
    }
    for (int ci = 0; ci < 2; ci++) {
        for (int alpha = 0; alpha < 2; alpha++) {
            u32 ctl = gx.xfreg[(alpha ? 0x10 : 0x0E) + ci];
            u32 matc = gx.xfreg[0x0C + ci], ambc = gx.xfreg[0x0A + ci];
            float mat[4], amb[4];
            for (int k = 0; k < 4; k++) {
                mat[k] = ((ctl & 1) && v->has_col[ci] ? v->col[ci][k] : (u8)(matc >> (24 - 8 * k))) / 255.0f;
                amb[k] = (((ctl >> 6) & 1) && v->has_col[ci] ? v->col[ci][k] : (u8)(ambc >> (24 - 8 * k))) / 255.0f;
            }
            float res[4];
            if ((ctl >> 1) & 1) {                          /* lighting enabled */
                float acc[4] = {amb[0], amb[1], amb[2], amb[3]};
                u32 mask = ((ctl >> 2) & 15) | (((ctl >> 11) & 15) << 4);
                u32 dfn = (ctl >> 7) & 3, afn = (ctl >> 9) & 3;
                for (int li = 0; li < 8; li++) {
                    if (!((mask >> li) & 1)) continue;
                    const float *L = &gx.xfmem[0x600 + li * 0x10];
                    u32 lc; memcpy(&lc, &L[3], 4);
                    float col[4] = {(u8)(lc >> 24) / 255.f, (u8)(lc >> 16) / 255.f, (u8)(lc >> 8) / 255.f, (u8)lc / 255.f};
                    float ld[3] = {L[10] - eye[0], L[11] - eye[1], L[12] - eye[2]};
                    float d2 = ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2], d = sqrtf(d2);
                    float attn = 1.0f;
                    if (afn == 1) {                         /* specular */
                        float lp = sqrtf(L[10] * L[10] + L[11] * L[11] + L[12] * L[12]);
                        for (int k = 0; k < 3; k++) ld[k] = lp > 0 ? L[10 + k] / lp : 0;
                        float nd = n[0] * ld[0] + n[1] * ld[1] + n[2] * ld[2];
                        float a = nd >= 0 ? fmaxf(0, n[0] * L[13] + n[1] * L[14] + n[2] * L[15]) : 0;
                        float num = fmaxf(0, L[4] + L[5] * a + L[6] * a * a), den = L[7] + L[8] * a + L[9] * a * a;
                        attn = den != 0 ? num / den : 0;
                    } else {
                        if (d > 0) for (int k = 0; k < 3; k++) ld[k] /= d;
                        if (afn == 3) {                     /* spot */
                            float a = fmaxf(0, ld[0] * L[13] + ld[1] * L[14] + ld[2] * L[15]);
                            float num = fmaxf(0, L[4] + L[5] * a + L[6] * a * a), den = L[7] + L[8] * d + L[9] * d2;
                            attn = den != 0 ? num / den : 0;
                        }
                    }
                    float diff = 1.0f;
                    if (dfn) {
                        diff = ld[0] * n[0] + ld[1] * n[1] + ld[2] * n[2];
                        if (dfn == 2) diff = fmaxf(0, diff);
                    }
                    for (int k = 0; k < 4; k++) acc[k] += col[k] * diff * attn;
                }
                for (int k = 0; k < 4; k++) res[k] = mat[k] * fminf(1, fmaxf(0, acc[k]));
            } else {
                memcpy(res, mat, sizeof res);
            }
            if (alpha) o->col[ci][3] = res[3];
            else for (int k = 0; k < 3; k++) o->col[ci][k] = res[k];
        }
    }
}

/* ------------------------------------------------------------------ textures */
typedef struct { u32 key[6]; int w, h; u8 *rgba; } Tex;
static Tex g_tex[64];
static int g_ntex;

static void rgb565(u16 v, u8 *o) { o[0] = (u8)(((v >> 11) & 31) * 255 / 31); o[1] = (u8)(((v >> 5) & 63) * 255 / 63); o[2] = (u8)((v & 31) * 255 / 31); o[3] = 255; }
static void rgb5a3(u16 v, u8 *o) {
    if (v & 0x8000) { o[0] = (u8)(((v >> 10) & 31) * 255 / 31); o[1] = (u8)(((v >> 5) & 31) * 255 / 31); o[2] = (u8)((v & 31) * 255 / 31); o[3] = 255; }
    else { o[0] = (u8)(((v >> 8) & 15) * 17); o[1] = (u8)(((v >> 4) & 15) * 17); o[2] = (u8)((v & 15) * 17); o[3] = (u8)(((v >> 12) & 7) * 255 / 7); }
}
static void palette(u32 fmt, u16 v, u8 *o) {
    if (fmt == 0) { o[0] = o[1] = o[2] = (u8)v; o[3] = (u8)(v >> 8); }   /* IA8 */
    else if (fmt == 1) rgb565(v, o);
    else rgb5a3(v, o);
}

static u8 *decode_texture(u32 addr, int w, int h, u32 fmt, u32 tlut_off, u32 tlut_fmt) {
    u8 *out = calloc((size_t)w * h, 4);
    addr |= 0x80000000u;
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int BW = bw[fmt & 15], BH = bh[fmt & 15];
    if (!BW) return out;
    u32 p = addr;
    for (int by = 0; by < h; by += BH)
        for (int bx = 0; bx < w; bx += BW) {
            if (fmt == 6) {                         /* RGBA8: AR block then GB block */
                u8 ar[32], gb[32];
                for (int i = 0; i < 32; i++) ar[i] = rd8(p + i);
                for (int i = 0; i < 32; i++) gb[i] = rd8(p + 32 + i);
                p += 64;
                for (int i = 0; i < 16; i++) {
                    int x = bx + (i & 3), y = by + (i >> 2);
                    if (x >= w || y >= h) continue;
                    u8 *o = out + (y * w + x) * 4;
                    o[3] = ar[i * 2]; o[0] = ar[i * 2 + 1]; o[1] = gb[i * 2]; o[2] = gb[i * 2 + 1];
                }
                continue;
            }
            if (fmt == 14) {                        /* CMPR: 2x2 DXT1 sub-blocks */
                for (int sb = 0; sb < 4; sb++) {
                    u16 c0 = (u16)(rd8(p) << 8 | rd8(p + 1)), c1 = (u16)(rd8(p + 2) << 8 | rd8(p + 3));
                    u32 bits = (u32)rd8(p + 4) << 24 | (u32)rd8(p + 5) << 16 | (u32)rd8(p + 6) << 8 | rd8(p + 7);
                    p += 8;
                    u8 pal[4][4];
                    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
                    for (int k = 0; k < 3; k++) {
                        if (c0 > c1) { pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3); }
                        else { pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2); pal[3][k] = pal[2][k]; }
                    }
                    pal[2][3] = 255; pal[3][3] = c0 > c1 ? 255 : 0;
                    for (int i = 0; i < 16; i++) {
                        int x = bx + (sb & 1) * 4 + (i & 3), y = by + (sb >> 1) * 4 + (i >> 2);
                        u32 ix = (bits >> (30 - 2 * i)) & 3;
                        if (x < w && y < h) memcpy(out + (y * w + x) * 4, pal[ix], 4);
                    }
                }
                continue;
            }
            for (int ty = 0; ty < BH; ty++)
                for (int tx = 0; tx < BW; tx++) {
                    int x = bx + tx, y = by + ty;
                    u8 px[4] = {0, 0, 0, 255};
                    switch (fmt) {
                    case 0: { u8 v = rd8(p + (ty * BW + tx) / 2); v = (tx & 1) ? (v & 15) : (v >> 4); px[0] = px[1] = px[2] = px[3] = (u8)(v * 17); break; }
                    case 1: { u8 v = rd8(p + ty * BW + tx); px[0] = px[1] = px[2] = px[3] = v; break; }
                    case 2: { u8 v = rd8(p + ty * BW + tx); px[0] = px[1] = px[2] = (u8)((v & 15) * 17); px[3] = (u8)((v >> 4) * 17); break; }
                    case 3: { u32 o = p + (ty * BW + tx) * 2; px[3] = rd8(o); px[0] = px[1] = px[2] = rd8(o + 1); break; }
                    case 4: { u32 o = p + (ty * BW + tx) * 2; rgb565((u16)(rd8(o) << 8 | rd8(o + 1)), px); break; }
                    case 5: { u32 o = p + (ty * BW + tx) * 2; rgb5a3((u16)(rd8(o) << 8 | rd8(o + 1)), px); break; }
                    case 8: case 9: case 10: {
                        u32 idx;
                        if (fmt == 8) { u8 v = rd8(p + (ty * BW + tx) / 2); idx = (tx & 1) ? (v & 15) : (v >> 4); }
                        else if (fmt == 9) idx = rd8(p + ty * BW + tx);
                        else { u32 o = p + (ty * BW + tx) * 2; idx = (u32)(rd8(o) << 8 | rd8(o + 1)) & 0x3FFF; }
                        u32 e = tlut_off + idx * 2;
                        palette(tlut_fmt, (u16)(tmem[e & 0xFFFFF] << 8 | tmem[(e + 1) & 0xFFFFF]), px);
                        break;
                    }
                    }
                    if (x < w && y < h) memcpy(out + (y * w + x) * 4, px, 4);
                }
            p += 32;
        }
    return out;
}

static Tex *get_texture(int map) {
    static const u8 img0[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
    static const u8 img3[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
    static const u8 tlut[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
    u32 i0 = gx.bp[img0[map]], addr = (gx.bp[img3[map]] & 0xFFFFFF) << 5, tl = gx.bp[tlut[map]];
    int w = (i0 & 0x3FF) + 1, h = ((i0 >> 10) & 0x3FF) + 1;
    u32 fmt = (i0 >> 20) & 15;
    u32 key[6] = {addr, (u32)w, (u32)h, fmt, tl, 0};
    for (int i = 0; i < g_ntex; i++) if (!memcmp(g_tex[i].key, key, sizeof key)) return &g_tex[i];
    if (g_ntex == 64) { for (int i = 0; i < g_ntex; i++) free(g_tex[i].rgba); g_ntex = 0; }
    Tex *t = &g_tex[g_ntex++];
    memcpy(t->key, key, sizeof key);
    t->w = w; t->h = h;
    t->rgba = decode_texture(addr, w, h, fmt, (tl & 0x3FF) << 9, (tl >> 10) & 3);
    return t;
}

static void sample(Tex *t, int map, float s, float tt, float out[4]) {
    static const u8 mode0[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
    u32 m = gx.bp[mode0[map]];
    int x = (int)floorf(s * t->w), y = (int)floorf(tt * t->h);
    u32 ws = m & 3, wt = (m >> 2) & 3;
    if (ws == 0) x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x;
    else if (ws == 2) { int p = 2 * t->w; x = ((x % p) + p) % p; if (x >= t->w) x = p - 1 - x; }
    else x = ((x % t->w) + t->w) % t->w;
    if (wt == 0) y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y;
    else if (wt == 2) { int p = 2 * t->h; y = ((y % p) + p) % p; if (y >= t->h) y = p - 1 - y; }
    else y = ((y % t->h) + t->h) % t->h;
    const u8 *px = t->rgba + (y * t->w + x) * 4;
    for (int k = 0; k < 4; k++) out[k] = px[k] / 255.0f;
}

/* ------------------------------------------------------------------ TEV */
static float tev_reg[4][4];      /* PREV, C0, C1, C2 (signed, 11-bit range) */
static float tev_konst[4][4];

static void konst(int stage, float kc[4]) {
    u32 ksel = gx.bp[0xF6 + stage / 2];
    u32 kcs = (stage & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31;
    u32 kas = (stage & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31;
    static const float fr[8] = {1, 7 / 8.f, 6 / 8.f, 5 / 8.f, 4 / 8.f, 3 / 8.f, 2 / 8.f, 1 / 8.f};
    for (int c = 0; c < 3; c++) {
        if (kcs < 8) kc[c] = fr[kcs];
        else if (kcs >= 12 && kcs < 16) kc[c] = tev_konst[kcs - 12][c];
        else if (kcs >= 16) kc[c] = tev_konst[(kcs - 16) & 3][(kcs - 16) >> 2];
        else kc[c] = 1;
    }
    if (kas < 8) kc[3] = fr[kas];
    else if (kas >= 16) kc[3] = tev_konst[(kas - 16) & 3][(kas - 16) >> 2];
    else kc[3] = 1;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Evaluate the TEV for one pixel. tex[stage] / ras[stage] are prepared by the caller. */
static void tev(int nstages, float tex[16][4], float ras[16][4], float out[4]) {
    float r[4][4];
    memcpy(r, tev_reg, sizeof r);
    for (int s = 0; s < nstages; s++) {
        u32 cenv = gx.bp[0xC0 + s * 2], aenv = gx.bp[0xC1 + s * 2];
        float kc[4];
        konst(s, kc);
        float in[4][3];
        for (int k = 0; k < 4; k++) {
            u32 sel = (cenv >> (12 - 4 * k)) & 15;
            for (int c = 0; c < 3; c++) {
                float v;
                switch (sel) {
                case 0: v = r[0][c]; break; case 1: v = r[0][3]; break;
                case 2: v = r[1][c]; break; case 3: v = r[1][3]; break;
                case 4: v = r[2][c]; break; case 5: v = r[2][3]; break;
                case 6: v = r[3][c]; break; case 7: v = r[3][3]; break;
                case 8: v = tex[s][c]; break; case 9: v = tex[s][3]; break;
                case 10: v = ras[s][c]; break; case 11: v = ras[s][3]; break;
                case 12: v = 1; break; case 13: v = 0.5f; break;
                case 14: v = kc[c]; break; default: v = 0; break;
                }
                in[k][c] = v;
            }
        }
        float ain[4];
        for (int k = 0; k < 4; k++) {
            u32 sel = (aenv >> (13 - 3 * k)) & 7;
            switch (sel) {
            case 0: ain[k] = r[0][3]; break; case 1: ain[k] = r[1][3]; break;
            case 2: ain[k] = r[2][3]; break; case 3: ain[k] = r[3][3]; break;
            case 4: ain[k] = tex[s][3]; break; case 5: ain[k] = ras[s][3]; break;
            case 6: ain[k] = kc[3]; break; default: ain[k] = 0; break;
            }
        }
        static const float scale[4] = {1, 2, 4, 0.5f}, bias[4] = {0, 0.5f, -0.5f, 0};
        int cd = (cenv >> 22) & 3, ad = (aenv >> 22) & 3;
        for (int c = 0; c < 3; c++) {
            float a = clampf(in[0][c], 0, 1), b = clampf(in[1][c], 0, 1), cc = clampf(in[2][c], 0, 1);
            float v = a * (1 - cc) + b * cc;
            v = ((cenv >> 18) & 1) ? in[3][c] - v : in[3][c] + v;
            v = (v + bias[(cenv >> 16) & 3]) * scale[(cenv >> 20) & 3];
            r[cd][c] = ((cenv >> 19) & 1) ? clampf(v, 0, 1) : clampf(v, -1024 / 255.f, 1023 / 255.f);
        }
        {
            float a = clampf(ain[0], 0, 1), b = clampf(ain[1], 0, 1), cc = clampf(ain[2], 0, 1);
            float v = a * (1 - cc) + b * cc;
            v = ((aenv >> 18) & 1) ? ain[3] - v : ain[3] + v;
            v = (v + bias[(aenv >> 16) & 3]) * scale[(aenv >> 20) & 3];
            r[ad][3] = ((aenv >> 19) & 1) ? clampf(v, 0, 1) : clampf(v, -1024 / 255.f, 1023 / 255.f);
        }
        if (s == nstages - 1) {
            for (int k = 0; k < 3; k++) out[k] = clampf(r[cd][k], 0, 1);
            out[3] = clampf(r[ad][3], 0, 1);
        }
    }
}

static int alpha_test(float a) {
    u32 at = gx.bp[0xF3];
    int v = (int)(a * 255 + 0.5f);
    int res[2];
    for (int i = 0; i < 2; i++) {
        int ref = (at >> (8 * i)) & 0xFF;
        switch ((at >> (16 + 3 * i)) & 7) {
        case 0: res[i] = 0; break; case 1: res[i] = v < ref; break; case 2: res[i] = v == ref; break;
        case 3: res[i] = v <= ref; break; case 4: res[i] = v > ref; break; case 5: res[i] = v != ref; break;
        case 6: res[i] = v >= ref; break; default: res[i] = 1; break;
        }
    }
    switch ((at >> 22) & 3) {
    case 0: return res[0] && res[1];
    case 1: return res[0] || res[1];
    case 2: return res[0] != res[1];
    default: return res[0] == res[1];
    }
}

static float blend_factor(u32 f, const float *src, const float *dst, int c, int is_src) {
    switch (f) {
    case 0: return 0; case 1: return 1;
    case 2: return is_src ? dst[c] : src[c];
    case 3: return 1 - (is_src ? dst[c] : src[c]);
    case 4: return src[3]; case 5: return 1 - src[3];
    case 6: return dst[3]; default: return 1 - dst[3];
    }
}

/* ------------------------------------------------------------------ raster */
static void raster_tri(const SVtx *v0, const SVtx *v1, const SVtx *v2) {
    const SVtx *v[3] = {v0, v1, v2};
    float sx[3], sy[3], sz[3], iw[3];
    float vpx = xff(0x1A), vpy = xff(0x1B), vpz = xff(0x1C), ox = xff(0x1D) - 342, oy = xff(0x1E) - 342, fz = xff(0x1F);
    for (int i = 0; i < 3; i++) {
        if (v[i]->w <= 1e-6f && v[i]->w >= -1e-6f) return;
        if (v[i]->w < 0) return;              /* behind the camera (no clipping yet) */
        iw[i] = 1.0f / v[i]->w;
        sx[i] = v[i]->x * iw[i] * vpx + ox;
        sy[i] = v[i]->y * iw[i] * vpy + oy;
        sz[i] = (v[i]->z * iw[i] * vpz + fz) / 16777215.0f;
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-6f) return;
    /* face culling: BP genmode bits 14-15 (hardware: 0 none, 1 back, 2 front, 3 all);
     * GX treats clockwise (in window space, y down) as front facing. */
    u32 cull = (gx.bp[0x00] >> 14) & 3;
    if (cull == 3) return;
    int backface = g_cull_flip ? area < 0 : area > 0;
    if ((cull == 1 && backface) || (cull == 2 && !backface)) return;
    int minx = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2]))), maxx = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    int miny = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2]))), maxy = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    /* scissor (BP 0x20/0x21, offset 342) */
    int scl = (int)(gx.bp[0x20] >> 12 & 0x7FF) - 342, sct = (int)(gx.bp[0x20] & 0x7FF) - 342;
    int scr = (int)(gx.bp[0x21] >> 12 & 0x7FF) - 342, scb = (int)(gx.bp[0x21] & 0x7FF) - 342;
    if (scr <= scl || scb <= sct) { scl = 0; sct = 0; scr = EFB_W - 1; scb = EFB_H - 1; }
    if (minx < scl) minx = scl;
    if (miny < sct) miny = sct;
    if (maxx > scr) maxx = scr;
    if (maxy > scb) maxy = scb;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= EFB_W) maxx = EFB_W - 1;
    if (maxy >= EFB_H) maxy = EFB_H - 1;

    u32 gen = gx.bp[0x00];
    int nstages = (int)((gen >> 10) & 15) + 1, ntex = (int)(gen & 15);
    (void)ntex;
    Tex *tex[16] = {0};
    int texmap[16], texcoord[16], chan[16];
    for (int s = 0; s < nstages; s++) {
        u32 o = gx.bp[0x28 + s / 2] >> ((s & 1) * 12);
        texmap[s] = o & 7; texcoord[s] = (o >> 3) & 7; chan[s] = (o >> 7) & 7;
        if ((o >> 6) & 1) tex[s] = get_texture(texmap[s]);
    }
    u32 zm = gx.bp[0x40], bm = gx.bp[0x41];
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
            float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
            if (zm & 1) {
                float d = zbuf[y][x];
                int pass;
                switch ((zm >> 1) & 7) {
                case 0: pass = 0; break; case 1: pass = z < d; break; case 2: pass = z == d; break;
                case 3: pass = z <= d; break; case 4: pass = z > d; break; case 5: pass = z != d; break;
                case 6: pass = z >= d; break; default: pass = 1; break;
                }
                if (!pass) continue;
            }
            /* perspective-correct interpolation */
            float pw0 = w0 * iw[0], pw1 = w1 * iw[1], pw2 = w2 * iw[2], ps = pw0 + pw1 + pw2;
            pw0 /= ps; pw1 /= ps; pw2 /= ps;
            float texc[16][4], rasc[16][4];
            for (int s = 0; s < nstages; s++) {
                if (tex[s]) {
                    int tc = texcoord[s];
                    float q = pw0 * v0->tc[tc][2] + pw1 * v1->tc[tc][2] + pw2 * v2->tc[tc][2];
                    float u = (pw0 * v0->tc[tc][0] + pw1 * v1->tc[tc][0] + pw2 * v2->tc[tc][0]) / (q ? q : 1);
                    float t = (pw0 * v0->tc[tc][1] + pw1 * v1->tc[tc][1] + pw2 * v2->tc[tc][1]) / (q ? q : 1);
                    sample(tex[s], texmap[s], u, t, texc[s]);
                } else {
                    texc[s][0] = texc[s][1] = texc[s][2] = texc[s][3] = 1;
                }
                int ch = chan[s] == 1 || chan[s] == 3 || chan[s] == 5 ? 1 : 0;
                if (chan[s] == 7) { rasc[s][0] = rasc[s][1] = rasc[s][2] = rasc[s][3] = 0; }
                else for (int k = 0; k < 4; k++)
                    rasc[s][k] = pw0 * v0->col[ch][k] + pw1 * v1->col[ch][k] + pw2 * v2->col[ch][k];
            }
            float out[4];
            tev(nstages, texc, rasc, out);
            if (!alpha_test(out[3])) continue;
            u8 *d = efb[y][x];
            float dst[4] = {d[0] / 255.f, d[1] / 255.f, d[2] / 255.f, d[3] / 255.f};
            float res[4];
            if (bm & 1) {
                u32 sf = (bm >> 8) & 7, df = (bm >> 5) & 7;
                for (int c = 0; c < 4; c++) {
                    float v = out[c] * blend_factor(sf, out, dst, c, 1);
                    float dv = dst[c] * blend_factor(df, out, dst, c, 0);
                    res[c] = (bm >> 11) & 1 ? dv - v : v + dv;
                }
            } else memcpy(res, out, sizeof res);
            if ((bm >> 3) & 1) for (int c = 0; c < 3; c++) d[c] = (u8)(clampf(res[c], 0, 1) * 255 + 0.5f);
            if ((bm >> 4) & 1) d[3] = (u8)(clampf(res[3], 0, 1) * 255 + 0.5f);
            if ((zm & 1) && ((zm >> 4) & 1)) zbuf[y][x] = z;
        }
}

void gx_render_draw(u8 prim, u8 vat, u16 count, const u8 *data) {
    if (!count) return;
    SVtx *sv = malloc(sizeof(SVtx) * count);
    Reader s = {data, 0, 0};
    for (u16 i = 0; i < count; i++) { Vtx v; decode_vertex(&s, vat, &v); transform(&v, &sv[i]); }
    switch (prim) {
    case 0x80: case 0x88:                          /* quads */
        for (int i = 0; i + 3 < count; i += 4) { raster_tri(&sv[i], &sv[i + 1], &sv[i + 2]); raster_tri(&sv[i], &sv[i + 2], &sv[i + 3]); }
        break;
    case 0x90:                                     /* triangles */
        for (int i = 0; i + 2 < count; i += 3) raster_tri(&sv[i], &sv[i + 1], &sv[i + 2]);
        break;
    case 0x98:                                     /* strip */
        for (int i = 0; i + 2 < count; i++) raster_tri(&sv[i], &sv[i + 1], &sv[i + 2]);
        break;
    case 0xA0:                                     /* fan */
        for (int i = 1; i + 1 < count; i++) raster_tri(&sv[0], &sv[i], &sv[i + 1]);
        break;
    default: break;                                /* lines/points: TODO */
    }
    free(sv);
}

/* ------------------------------------------------------------------ BP side effects */
static void write_frame(int x0, int y0, int w, int h) {
    if (!g_frame_dir || (gx_frames % (unsigned)g_frame_every)) return;
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%05llu.ppm", g_frame_dir, gx_frames);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = y0; y < y0 + h && y < EFB_H; y++)
        for (int x = x0; x < x0 + w && x < EFB_W; x++) fwrite(efb[y][x], 1, 3, f);
    fclose(f);
}

void gx_render_bp(u8 reg, u32 v) {
    switch (reg) {
    case 0x65: {                                   /* load TLUT: RAM (0x64) -> TMEM */
        u32 src = (gx.bp[0x64] & 0xFFFFFF) << 5, dst = (v & 0x3FF) << 9, n = (v & 0x1FFC00) >> 5;
        for (u32 i = 0; i < n && dst + i < sizeof tmem; i++) tmem[dst + i] = rd8(src + i);
        for (int i = 0; i < g_ntex; i++) free(g_tex[i].rgba);
        g_ntex = 0;
        break;
    }
    case 0xE0: case 0xE2: case 0xE4: case 0xE6: case 0xE1: case 0xE3: case 0xE5: case 0xE7: {
        int i = (reg - 0xE0) >> 1, konst_sel = (v >> 23) & 1;
        float (*dst)[4] = konst_sel ? tev_konst : tev_reg;
        float lo = (float)((s32)(v << 21) >> 21) / 255.f, hi = (float)((s32)((v >> 12) << 21) >> 21) / 255.f;
        if (!(reg & 1)) { dst[i][0] = lo; dst[i][3] = hi; }   /* RA */
        else { dst[i][2] = lo; dst[i][1] = hi; }             /* BG */
        break;
    }
    case 0x52: {                                   /* EFB copy */
        int x0 = gx.bp[0x49] & 0x3FF, y0 = (gx.bp[0x49] >> 10) & 0x3FF;
        int w = (gx.bp[0x4A] & 0x3FF) + 1, h = ((gx.bp[0x4A] >> 10) & 0x3FF) + 1;
        if ((v >> 14) & 1) { write_frame(x0, y0, w, h); gx_frames++; }
        if ((v >> 11) & 1) {                       /* clear after copy */
            u32 ar = gx.bp[0x4F], gb = gx.bp[0x50];
            u8 c[4] = {(u8)ar, (u8)(gb >> 8), (u8)gb, (u8)(ar >> 8)};
            float z = (gx.bp[0x51] & 0xFFFFFF) / 16777215.0f;
            for (int y = y0; y < y0 + h && y < EFB_H; y++)
                for (int x = x0; x < x0 + w && x < EFB_W; x++) { memcpy(efb[y][x], c, 4); zbuf[y][x] = z; }
        }
        break;
    }
    }
}
