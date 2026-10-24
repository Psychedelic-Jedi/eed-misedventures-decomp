#pragma once
#include "cpu/cpu.h"
/* Guest address aliasing: [from, from+size) behaves as [to, to+size).
 * Used to make an LU module's heap copy and its pre-linked fixed copy one memory. */
#define MAX_ALIASES 16
typedef struct { u32 from, to, size; } MemAlias;
extern MemAlias g_alias[MAX_ALIASES];
extern int g_alias_count;
static inline u32 alias_translate(u32 addr) {
    for (int i = 0; i < g_alias_count; i++) {
        u32 a = addr & 0x3FFFFFFFu;            /* cached/uncached mirrors */
        if (a - (g_alias[i].from & 0x3FFFFFFFu) < g_alias[i].size)
            return (addr & 0xC0000000u) | ((g_alias[i].to & 0x3FFFFFFFu) + (a - (g_alias[i].from & 0x3FFFFFFFu)));
    }
    return addr;
}
void alias_add(CPUState *cpu, u32 from, u32 to, u32 size);

/* Host-backed guest ranges (Dolphin SDK VM library replacement): the virtual
 * address space the game allocates with VMAlloc is plain host memory. */
#define MAX_HOST_REGIONS 8
typedef struct { u32 base, size; u8 *mem; } HostRegion;
extern HostRegion g_host_region[MAX_HOST_REGIONS];
extern int g_host_region_count;
static inline u8 *host_region_resolve(u32 addr, u32 *avail) {
    for (int i = 0; i < g_host_region_count; i++) {
        u32 off = addr - g_host_region[i].base;
        if (off < g_host_region[i].size) { *avail = g_host_region[i].size - off; return g_host_region[i].mem + off; }
    }
    return 0;
}
int host_region_add(u32 base, u32 size);

/* Fast path: host pointer for each 4 KB guest page (NULL = take the slow path:
 * MMIO, unmapped, or a page only partly covered by an alias). Rebuilt whenever
 * aliases or host regions change. */
extern u8 **g_page;
void page_table_rebuild(void);
static inline u8 *page_fast(u32 addr, u32 *avail) {
    u8 *p = g_page ? g_page[addr >> 12] : 0;
    u32 o = addr & 0xFFF;
    if (p && o <= 0xFF8) { *avail = 0x1000 - o; return p + o; }
    return 0;
}
