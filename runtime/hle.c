/* High-level emulation: native replacements for Dolphin SDK functions.
 * Each function receives the guest CPU state; arguments are in r3..r10 / f1..f8,
 * return values go in r3 / f1. The dispatcher returns to the caller (pc = lr). */
#include <stdio.h>
#include "hle.h"

static void __OSInitAudioSystem(CPUState *cpu) {
    (void)cpu; /* DSP/ARAM bring-up not needed: audio will be produced natively. */
}

static void __DSP_boot_task(CPUState *cpu) {
    (void)cpu; /* No DSP: the AX ucode is replaced by native audio mixing (TODO). */
}

typedef void (*HleFn)(CPUState *);
static const struct { u32 addr; HleFn fn; const char *name; } k_hle[] = {
#define HLE_ENTRY(addr, name, orig) { addr, name, #name },
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

int hle_trace = 1;
