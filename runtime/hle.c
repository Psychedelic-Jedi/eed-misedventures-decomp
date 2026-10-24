/* High-level emulation: native replacements for Dolphin SDK functions.
 * Each function receives the guest CPU state; arguments are in r3..r10 / f1..f8,
 * return values go in r3 / f1. The dispatcher returns to the caller (pc = lr). */
#include <stdio.h>
#include <string.h>
#include "hle.h"
#include "irq.h"
#include "modules.h"
#include "alias.h"
#include "input.h"
#include "card.h"
#include "platform.h"
#include "hw.h"

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

/* ---- Interrupt enable/disable: deliver pending interrupts the moment EE is set ---- */
int hle_pc_set; /* set by an HLE function that already chose the next pc */
#define MSR_EE 0x8000u
static void hle_OSDisableInterrupts(CPUState *cpu) {
    cpu->gpr[3] = (cpu->msr >> 15) & 1;
    cpu->msr &= ~MSR_EE;
}
static void hle_OSEnableInterrupts(CPUState *cpu) {
    cpu->gpr[3] = (cpu->msr >> 15) & 1;
    cpu->msr |= MSR_EE;
    hle_pc_set = irq_check_on_enable(cpu, cpu->lr);
}
static void hle_OSRestoreInterrupts(CPUState *cpu) {
    u32 level = cpu->gpr[3];
    cpu->gpr[3] = (cpu->msr >> 15) & 1;
    if (level) cpu->msr |= MSR_EE; else cpu->msr &= ~MSR_EE;
    if (level) hle_pc_set = irq_check_on_enable(cpu, cpu->lr);
}

/* LU code relocation: install pre-linked, recompiled module images instead (modules.c). */
static void hle_LU_RelocateBatch(CPUState *cpu) { module_relocate_hook(cpu); }
static void hle_LU_FixupModuleHeader(CPUState *cpu) { module_fixup_hook(cpu); }

/* ---- Dolphin SDK VM library: virtual memory backed by ARAM with page faults.
 * Replaced by host memory: VMAlloc'd ranges are always resident. ---- */
static void hle_VMInit(CPUState *cpu) {
    fprintf(stderr, "[vm] VMInit(page pool %u bytes, aram %08X, %u bytes): host-backed\n",
            cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
static void hle_VMAlloc(CPUState *cpu) {
    u32 va = cpu->gpr[3], size = cpu->gpr[4];
    int ok = host_region_add(va, size);
    fprintf(stderr, "[vm] VMAlloc(%08X, 0x%X) -> %s\n", va, size, ok ? "host memory" : "FAILED");
    cpu->gpr[3] = ok;
}

/* u32 PADRead(PADStatus status[4]): 12-byte records
 * { u16 button; s8 stickX, stickY, substickX, substickY; u8 triggerL, triggerR, analogA, analogB; s8 err; } */
static void hle_PADRead(CPUState *cpu) {
    u32 st = cpu->gpr[3];
    static unsigned long long last;
    if (vi_frames != last) { plat_input_update(vi_frames); last = vi_frames; }
    for (int i = 0; i < 4; i++, st += 12) {
        PadState *p = &g_pad[i];
        mem_write16(cpu, st + 0, p->connected ? p->buttons : 0);
        mem_write8(cpu, st + 2, (u8)p->stick_x);  mem_write8(cpu, st + 3, (u8)p->stick_y);
        mem_write8(cpu, st + 4, (u8)p->cstick_x); mem_write8(cpu, st + 5, (u8)p->cstick_y);
        mem_write8(cpu, st + 6, p->trigger_l);    mem_write8(cpu, st + 7, p->trigger_r);
        mem_write8(cpu, st + 8, (p->buttons & PAD_A) ? 0xFF : 0);
        mem_write8(cpu, st + 9, (p->buttons & PAD_B) ? 0xFF : 0);
        mem_write8(cpu, st + 10, p->connected ? 0 : (u8)-1);  /* PAD_ERR_NONE / PAD_ERR_NO_CONTROLLER */
        mem_write8(cpu, st + 11, 0);
    }
    cpu->gpr[3] = 0;
}

/* ---- Memory card (card.c) ---- */
static void hle_CARDGetResultCode(CPUState *c) { card_GetResultCode(c); }
static void hle_CARDFreeBlocks(CPUState *c) { card_FreeBlocks(c); }
static void hle_CARDCheckAsync(CPUState *c) { card_CheckAsync(c); }
static void hle_CARDProbeEx(CPUState *c) { card_ProbeEx(c); }
static void hle_CARDMountAsync(CPUState *c) { card_MountAsync(c); }
static void hle_CARDMount(CPUState *c) { card_Mount(c); }
static void hle_CARDUnmount(CPUState *c) { card_Unmount(c); }
static void hle_CARDFormatAsync(CPUState *c) { card_FormatAsync(c); }
static void hle_CARDOpen(CPUState *c) { card_Open(c); }
static void hle_CARDClose(CPUState *c) { card_Close(c); }
static void hle_CARDCreateAsync(CPUState *c) { card_CreateAsync(c); }
static void hle_CARDReadAsync(CPUState *c) { card_ReadAsync(c); }
static void hle_CARDWriteAsync(CPUState *c) { card_WriteAsync(c); }
static void hle_CARDDeleteAsync(CPUState *c) { card_DeleteAsync(c); }
static void hle_CARDGetStatus(CPUState *c) { card_GetStatus(c); }
static void hle_CARDSetStatus(CPUState *c) { card_SetStatus(c); }

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
            hle_pc_set = 0;
            k_hle[i].fn(cpu);
            if (!hle_pc_set) cpu->pc = cpu->lr;
            return 1;
        }
    }
    return 0;
}

int hle_trace = 0;
