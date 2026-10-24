/* GX command processor front end.
 *
 * Bytes written to the write-gather pipe (0xCC008000) arrive here. The parser
 * splits them into GX commands (BP/CP/XF register loads, indexed XF loads,
 * display-list calls, draws), keeps the GX register state in `gx`, implements
 * pixel-engine completion (draw done / tokens, which the SDK waits on), and hands
 * draws and pixel-pipeline events to the renderer (gx_render.c).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gx_fifo.h"
#include "gx.h"

GXState gx;
CPUState *gx_cpu;

static u8 buf[1 << 20];
static u32 len;

unsigned long long gx_cmds, gx_draws, gx_draw_done, gx_tokens;
int gx_trace = 0;

/* Pixel engine state (read back through PE registers by hw.c). */
u32 pe_finish_pending, pe_token_pending;
u16 pe_token_value;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static u32 comp_size(u32 fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
static u32 color_size(u32 fmt) {
    static const u8 s[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    return s[fmt & 7];
}
static u32 attr_bytes(u32 type, u32 direct) { return type == 0 ? 0 : type == 1 ? direct : type == 2 ? 1 : 2; }

/* Bytes per vertex for vertex format `v`, from VCD + VAT (see GX docs / Dolphin VertexLoader). */
u32 gx_vertex_size(u32 v) {
    u32 a = gx.vat_a[v], b = gx.vat_b[v], c = gx.vat_c[v];
    u32 n = 0;
    n += gx.vcd_lo & 1;                               /* position/normal matrix index */
    for (u32 i = 0; i < 8; i++) n += (gx.vcd_lo >> (1 + i)) & 1; /* tex matrix indices */
    u32 pos = (gx.vcd_lo >> 9) & 3, nrm = (gx.vcd_lo >> 11) & 3;
    u32 c0 = (gx.vcd_lo >> 13) & 3, c1 = (gx.vcd_lo >> 15) & 3;
    n += attr_bytes(pos, ((a & 1) ? 3 : 2) * comp_size((a >> 1) & 7));
    if (nrm) {
        u32 nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
        u32 direct = (nbt ? 9 : 3) * comp_size((a >> 10) & 7);
        if (nrm >= 2 && nbt && idx3) n += 3 * (nrm == 2 ? 1 : 2);
        else n += attr_bytes(nrm, direct);
    }
    n += attr_bytes(c0, color_size((a >> 14) & 7));
    n += attr_bytes(c1, color_size((a >> 18) & 7));
    u32 tcnt[8], tfmt[8];
    tcnt[0] = (a >> 21) & 1; tfmt[0] = (a >> 22) & 7;
    tcnt[1] = b & 1;         tfmt[1] = (b >> 1) & 7;
    tcnt[2] = (b >> 9) & 1;  tfmt[2] = (b >> 10) & 7;
    tcnt[3] = (b >> 18) & 1; tfmt[3] = (b >> 19) & 7;
    tcnt[4] = (b >> 27) & 1; tfmt[4] = (b >> 28) & 7;
    tcnt[5] = (c >> 5) & 1;  tfmt[5] = (c >> 6) & 7;
    tcnt[6] = (c >> 14) & 1; tfmt[6] = (c >> 15) & 7;
    tcnt[7] = (c >> 23) & 1; tfmt[7] = (c >> 24) & 7;
    for (u32 i = 0; i < 8; i++) {
        u32 t = (gx.vcd_hi >> (i * 2)) & 3;
        n += attr_bytes(t, (tcnt[i] ? 2 : 1) * comp_size(tfmt[i]));
    }
    return n;
}

static void load_cp(u8 reg, u32 v) {
    switch (reg & 0xF0) {
    case 0x30: gx.matidx_a = v; break;
    case 0x40: gx.matidx_b = v; break;
    case 0x50: gx.vcd_lo = v; break;
    case 0x60: gx.vcd_hi = v; break;
    case 0x70: gx.vat_a[reg & 7] = v; break;
    case 0x80: gx.vat_b[reg & 7] = v; break;
    case 0x90: gx.vat_c[reg & 7] = v; break;
    case 0xA0: gx.array_base[reg & 15] = v; break;
    case 0xB0: gx.array_stride[reg & 15] = v & 0xFF; break;
    }
}

static void load_xf(u32 addr, const u8 *data, u32 count) {
    for (u32 i = 0; i < count; i++, addr++) {
        u32 w = be32(data + i * 4);
        if (addr < 0x680) memcpy(&gx.xfmem[addr], &w, 4);
        else if (addr >= 0x1000 && addr < 0x1100) gx.xfreg[addr - 0x1000] = w;
    }
}

/* LOAD_INDX_A..D: copy matrix data from a CP array (12=pos, 13=nrm, 14=tex, 15=light) into XF memory. */
static void load_indexed(u8 op, u32 v) {
    u32 array = 12 + ((op - 0x20) >> 3);
    u32 index = v >> 16, size = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    u32 src = (gx.array_base[array] + index * gx.array_stride[array]) | 0x80000000u;
    for (u32 i = 0; i < size; i++) {
        u32 w = mem_read32(gx_cpu, src + i * 4);
        if (addr + i < 0x680) memcpy(&gx.xfmem[addr + i], &w, 4);
    }
}

static void load_bp(u32 v) {
    u8 reg = v >> 24;
    u32 val = v & 0xFFFFFF;
    if (reg == 0xFE) { gx.bp[0xFE] = val; return; }  /* BP mask for the next write */
    if (gx.bp[0xFE] != 0xFFFFFF) {
        val = (gx.bp[reg] & ~gx.bp[0xFE]) | (val & gx.bp[0xFE]);
        gx.bp[0xFE] = 0xFFFFFF;
    }
    gx.bp[reg] = val;
    switch (reg) {
    case 0x45: /* PE_DONE: GXDrawDone / GXSetDrawDone */
        gx_draw_done++;
        pe_finish_pending = 1;
        break;
    case 0x47: /* PE_TOKEN (no interrupt) */
        pe_token_value = (u16)val;
        break;
    case 0x48: /* PE_TOKEN_INT */
        pe_token_value = (u16)val;
        pe_token_pending = 1;
        gx_tokens++;
        break;
    }
    gx_render_bp(reg, val);
}

static void run_display_list(u32 addr, u32 size);

/* Parses one command at p (avail bytes). Returns bytes used, or 0 if incomplete. */
static u32 parse_one(const u8 *p, u32 avail) {
    u8 op = p[0];
    switch (op) {
    case 0x00: return 1;                                  /* NOP */
    case 0x48: return 1;                                  /* invalidate vertex cache */
    case 0x08:                                            /* LOAD_CP_REG */
        if (avail < 6) return 0;
        load_cp(p[1], be32(p + 2));
        return 6;
    case 0x10: {                                          /* LOAD_XF_REG */
        if (avail < 5) return 0;
        u32 h = be32(p + 1), count = ((h >> 16) & 0xF) + 1, n = 5 + 4 * count;
        if (avail < n) return 0;
        load_xf(h & 0xFFFF, p + 5, count);
        return n;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:           /* LOAD_INDX_A..D */
        if (avail < 5) return 0;
        load_indexed(op, be32(p + 1));
        return 5;
    case 0x40:                                            /* CALL_DL (addr, size) */
        if (avail < 9) return 0;
        run_display_list(be32(p + 1), be32(p + 5));
        return 9;
    case 0x44: return 1;
    case 0x61:                                            /* LOAD_BP_REG */
        if (avail < 5) return 0;
        load_bp(be32(p + 1));
        return 5;
    }
    if (op >= 0x80 && op < 0xC0) {                        /* draw primitive */
        if (avail < 3) return 0;
        u32 count = (u32)p[1] << 8 | p[2];
        u32 n = 3 + count * gx_vertex_size(op & 7);
        if (n > sizeof buf) { fprintf(stderr, "[gx] draw too large (%u bytes), resyncing\n", n); return avail; }
        if (avail < n) return 0;
        gx_draws++;
        gx_render_draw(op & 0xF8, op & 7, (u16)count, p + 3);
        return n;
    }
    if (gx_trace) fprintf(stderr, "[gx] unknown opcode %02X, dropping byte\n", op);
    return 1;
}

/* Display lists live in guest RAM and are executed synchronously. */
static void run_display_list(u32 addr, u32 size) {
    static int depth;
    if (depth > 4 || size > (8u << 20)) return;
    u8 *dl = malloc(size ? size : 1);
    addr |= 0x80000000u;
    for (u32 i = 0; i < size; i++) dl[i] = mem_read8(gx_cpu, addr + i);
    depth++;
    u32 pos = 0, used;
    while (pos < size && (used = parse_one(dl + pos, size - pos)) != 0) { gx_cmds++; pos += used; }
    depth--;
    free(dl);
}

static FILE *g_dump;
static u32 head;   /* parse position within buf (avoids memmove per command) */
void gx_fifo_write(const u8 *data, u32 n) {
    if (gx_trace) { if (!g_dump) g_dump = fopen("/tmp/gxfifo.bin", "wb"); fwrite(data, 1, n, g_dump); fflush(g_dump); }
    while (n) {
        if (len + n > sizeof buf && head) {             /* compact */
            memmove(buf, buf + head, len - head);
            len -= head; head = 0;
        }
        u32 take = n < sizeof buf - len ? n : sizeof buf - len;
        memcpy(buf + len, data, take);
        len += take; data += take; n -= take;
        u32 used;
        while (head < len && (used = parse_one(buf + head, len - head)) != 0) {
            gx_cmds++;
            head += used;
        }
        if (head == len) head = len = 0;
    }
}
