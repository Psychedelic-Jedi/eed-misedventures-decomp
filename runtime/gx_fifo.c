/* GX command processor front end.
 *
 * Bytes written to the write-gather pipe (0xCC008000) arrive here one at a time.
 * The parser splits them into GX commands (BP/CP/XF register loads, display-list
 * calls, draws). For now it tracks the state needed to size draw commands and
 * implements pixel-engine completion (draw done / tokens), which the SDK waits on.
 * Rendering (via a GX → GLES/Vulkan backend) will hang off gx_on_* below.
 */
#include <stdio.h>
#include <string.h>
#include "gx_fifo.h"

static u32 cp_vcd_lo, cp_vcd_hi;
static u32 cp_vat_a[8], cp_vat_b[8], cp_vat_c[8];

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
static u32 vertex_size(u32 v) {
    u32 a = cp_vat_a[v], b = cp_vat_b[v], c = cp_vat_c[v];
    u32 n = 0;
    n += cp_vcd_lo & 1;                               /* position/normal matrix index */
    for (u32 i = 0; i < 8; i++) n += (cp_vcd_lo >> (1 + i)) & 1; /* tex matrix indices */
    u32 pos = (cp_vcd_lo >> 9) & 3, nrm = (cp_vcd_lo >> 11) & 3;
    u32 c0 = (cp_vcd_lo >> 13) & 3, c1 = (cp_vcd_lo >> 15) & 3;
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
        u32 t = (cp_vcd_hi >> (i * 2)) & 3;
        n += attr_bytes(t, (tcnt[i] ? 2 : 1) * comp_size(tfmt[i]));
    }
    return n;
}

static void load_cp(u8 reg, u32 v) {
    switch (reg & 0xF0) {
    case 0x50: cp_vcd_lo = v; break;
    case 0x60: cp_vcd_hi = v; break;
    case 0x70: cp_vat_a[reg & 7] = v; break;
    case 0x80: cp_vat_b[reg & 7] = v; break;
    case 0x90: cp_vat_c[reg & 7] = v; break;
    }
}

static void load_bp(u32 v) {
    u8 reg = v >> 24;
    switch (reg) {
    case 0x45: /* PE_DONE: GXDrawDone / GXSetDrawDone */
        gx_draw_done++;
        pe_finish_pending = 1;
        break;
    case 0x47: /* PE_TOKEN (no interrupt) */
        pe_token_value = (u16)v;
        break;
    case 0x48: /* PE_TOKEN_INT */
        pe_token_value = (u16)v;
        pe_token_pending = 1;
        gx_tokens++;
        break;
    }
}

/* Returns the size of the complete command at buf[0], or 0 if more bytes are needed. */
static u32 parse_one(void) {
    u8 op = buf[0];
    switch (op) {
    case 0x00: return 1;                                  /* NOP */
    case 0x48: return 1;                                  /* invalidate vertex cache */
    case 0x08:                                            /* LOAD_CP_REG */
        if (len < 6) return 0;
        load_cp(buf[1], be32(buf + 2));
        return 6;
    case 0x10: {                                          /* LOAD_XF_REG */
        if (len < 5) return 0;
        u32 n = 5 + 4 * ((be32(buf + 1) >> 16) + 1);
        return len < n ? 0 : n;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:           /* LOAD_INDX_A..D */
        return len < 5 ? 0 : 5;
    case 0x40:                                            /* CALL_DL (addr, size): executed from RAM */
        return len < 9 ? 0 : 9;
    case 0x44: return 1;                                  /* unknown/metrics */
    case 0x61:                                            /* LOAD_BP_REG */
        if (len < 5) return 0;
        load_bp(be32(buf + 1));
        return 5;
    }
    if (op >= 0x80 && op < 0xC0) {                        /* draw primitive */
        if (len < 3) return 0;
        u32 count = (u32)buf[1] << 8 | buf[2];
        u32 n = 3 + count * vertex_size(op & 7);
        if (n > sizeof buf) { fprintf(stderr, "[gx] draw too large (%u bytes), resyncing\n", n); return len; }
        if (len < n) return 0;
        gx_draws++;
        return n;
    }
    if (gx_trace) fprintf(stderr, "[gx] unknown opcode %02X, dropping byte\n", op);
    return 1;
}

static FILE *g_dump;
void gx_fifo_write(const u8 *data, u32 n) {
    if (gx_trace) { if (!g_dump) g_dump = fopen("/tmp/gxfifo.bin", "wb"); fwrite(data, 1, n, g_dump); fflush(g_dump); }
    while (n) {
        u32 take = n < sizeof buf - len ? n : sizeof buf - len;
        memcpy(buf + len, data, take);
        len += take; data += take; n -= take;
        u32 used;
        while (len && (used = parse_one()) != 0) {
            gx_cmds++;
            memmove(buf, buf + used, len - used);
            len -= used;
        }
    }
}
