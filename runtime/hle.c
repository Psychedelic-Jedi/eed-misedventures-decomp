/* High-level emulation: native replacements for Dolphin SDK functions.
 * Each function receives the guest CPU state; arguments are in r3..r10 / f1..f8,
 * return values go in r3 / f1. The dispatcher returns to the caller (pc = lr). */
#include <stdio.h>
#include <string.h>
#include "hle.h"

static void hle___OSInitAudioSystem(CPUState *cpu) {
    (void)cpu; /* DSP/ARAM bring-up not needed: audio will be produced natively. */
}

static void hle___DSP_boot_task(CPUState *cpu) {
    (void)cpu; /* No DSP: the AX ucode is replaced by native audio mixing (TODO). */
}

/* DSPTaskInfo* DSPAddTask(DSPTaskInfo* task). There is no DSP: report the task as
 * running and perform what AX's init callback would do (set __AXDSPInitFlag). */
#define AX_DSP_INIT_FLAG 0x803D668Cu
static void hle_DSPAddTask(CPUState *cpu) {
    mem_write32(cpu, AX_DSP_INIT_FLAG, 1);
    /* r3 (task) is returned unchanged */
}

/* ---- Guest printf ------------------------------------------------------
 * PowerPC SysV va_list: { u8 gpr; u8 fpr; u16 pad; u32 overflow_arg_area; u32 reg_save_area }
 * reg_save_area holds r3..r10 (8 x u32) followed by f1..f8 (8 x f64). */
typedef struct { CPUState *cpu; u32 va; } GuestVa;

static u32 va_int(GuestVa *v) {
    CPUState *c = v->cpu;
    u8 gpr = mem_read8(c, v->va);
    if (gpr < 8) {
        mem_write8(c, v->va, gpr + 1);
        return mem_read32(c, mem_read32(c, v->va + 8) + gpr * 4);
    }
    u32 ov = mem_read32(c, v->va + 4);
    mem_write32(c, v->va + 4, ov + 4);
    return mem_read32(c, ov);
}
static double va_dbl(GuestVa *v) {
    CPUState *c = v->cpu;
    u8 fpr = mem_read8(c, v->va + 1);
    u64 bits;
    if (fpr < 8) {
        mem_write8(c, v->va + 1, fpr + 1);
        bits = mem_read64(c, mem_read32(c, v->va + 8) + 32 + fpr * 8);
    } else {
        u32 ov = (mem_read32(c, v->va + 4) + 7) & ~7u;
        mem_write32(c, v->va + 4, ov + 8);
        bits = mem_read64(c, ov);
    }
    double d; memcpy(&d, &bits, 8); return d;
}
static void guest_str(CPUState *c, u32 a, char *out, size_t n) {
    size_t i = 0;
    if (!a) { snprintf(out, n, "(null)"); return; }
    for (; i + 1 < n; i++) { char ch = (char)mem_read8(c, a + i); if (!ch) break; out[i] = ch; }
    out[i] = 0;
}

/* Formats a guest format string + va_list into `out`. */
static void guest_vsnprintf(CPUState *c, char *out, size_t n, u32 fmt, u32 va) {
    GuestVa v = { c, va };
    size_t o = 0;
    char f[512];
    guest_str(c, fmt, f, sizeof f);
    for (const char *p = f; *p && o + 1 < n; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        char spec[32]; size_t k = 0; spec[k++] = '%'; p++;
        while (*p && strchr("-+ #0123456789.*", *p) && k < 20) {
            if (*p == '*') { k += snprintf(spec + k, sizeof spec - k, "%d", (int)va_int(&v)); p++; continue; }
            spec[k++] = *p++;
        }
        int lng = 0;
        while (*p == 'l' || *p == 'h') { if (*p == 'l') lng++; p++; }
        char conv = *p; char tmp[512];
        if (!conv) break;
        spec[k++] = conv; spec[k] = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (lng >= 2) { u64 hi = va_int(&v); u64 lo = va_int(&v);
                spec[k - 1] = 0; snprintf(tmp, sizeof tmp, "%sll%c", spec, conv);
                char s2[64]; snprintf(s2, sizeof s2, tmp, (unsigned long long)((hi << 32) | lo));
                snprintf(tmp, sizeof tmp, "%s", s2); }
            else snprintf(tmp, sizeof tmp, spec, (int)va_int(&v));
            break;
        case 'p': snprintf(tmp, sizeof tmp, "0x%08X", va_int(&v)); break;
        case 's': { char s2[256]; guest_str(c, va_int(&v), s2, sizeof s2); snprintf(tmp, sizeof tmp, spec, s2); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(tmp, sizeof tmp, spec, va_dbl(&v)); break;
        case '%': snprintf(tmp, sizeof tmp, "%%"); break;
        default: snprintf(tmp, sizeof tmp, "%s", spec); break;
        }
        for (char *t = tmp; *t && o + 1 < n; t++) out[o++] = *t;
    }
    out[o] = 0;
}

/* int vprintf(const char* fmt, va_list args) -- MSL; OSReport/OSPanic route through it. */
static void hle_vprintf(CPUState *cpu) {
    char buf[2048];
    guest_vsnprintf(cpu, buf, sizeof buf, cpu->gpr[3], cpu->gpr[4]);
    fputs(buf, stdout);
    fflush(stdout);
    cpu->gpr[3] = (u32)strlen(buf);
}

typedef void (*HleFn)(CPUState *);
static const struct { u32 addr; HleFn fn; const char *name; } k_hle[] = {
#define HLE_ENTRY(addr, name, orig) { addr, hle_##name, #name },
#include "hle_table.h"
#undef HLE_ENTRY
};

int hle_dispatch(CPUState *cpu, u32 cia) {
    for (unsigned i = 0; i < sizeof(k_hle) / sizeof(k_hle[0]); i++) {
        if (k_hle[i].addr == cia) {
            if (hle_trace) fprintf(stderr, "[hle] %s\n", k_hle[i].name);
            k_hle[i].fn(cpu);
            cpu->pc = cpu->lr;
            return 1;
        }
    }
    return 0;
}

int hle_trace = 0;
