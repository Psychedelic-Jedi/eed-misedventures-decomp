/* LU code modules: dispatch into their recompiled code, and replace the engine's
 * runtime relocation with pre-linked images at fixed addresses.
 *
 * Build side: `gctool.py linkall` pre-links every module (layout.txt + <name>.dol),
 * DolRecomp translates each .dol, build.sh generates modules_table.h.
 * Runtime side: the engine still loads the .gc pack and calls its relocator
 * fn_800FF904(module, first, count) in batches. Our HLE replacement copies the
 * pre-linked image to the module's fixed address on the first batch and points
 * the engine's module record at it, so all code the engine reaches through the
 * record lands in recompiled functions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules.h"
#include "alias.h"
#include "core.h"
#include "platform.h"

typedef DolRecompFunction (*FindFn)(u32);

#define MODULE(name, base, size) DolRecompFunction mod_##name##_find(u32);
#include "modules_table.h"
#undef MODULE

static const struct { const char *name; u32 base, size; FindFn find; } k_mods[] = {
#define MODULE(name, base, size) { #name, base, size, mod_##name##_find },
#include "modules_table.h"
#undef MODULE
    { NULL, 0, 0, NULL }
};

DolRecompFunction (*engine_find)(u32);
const char *module_dir;
int module_trace = 1;

HostRegion g_host_region[MAX_HOST_REGIONS];
int g_host_region_count;
int host_region_add(u32 base, u32 size) {
    for (int i = 0; i < g_host_region_count; i++)
        if (base < g_host_region[i].base + g_host_region[i].size && g_host_region[i].base < base + size) return 1;
    if (g_host_region_count == MAX_HOST_REGIONS) return 0;
    u8 *m = calloc(1, size);
    if (!m) return 0;
    g_host_region[g_host_region_count++] = (HostRegion){base, size, m};
    page_table_rebuild();
    return 1;
}

MemAlias g_alias[MAX_ALIASES];
int g_alias_count;

/* Remove alias i, copying its current bytes back to the "from" range so nothing
 * written through the alias is lost (the heap range may already hold new data). */
static void alias_remove(CPUState *cpu, int i) {
    MemAlias a = g_alias[i];
    g_alias[i] = g_alias[--g_alias_count];
    page_table_rebuild();
    u8 *tmp = malloc(a.size);
    MemAlias saved = a;
    for (u32 j = 0; j < a.size; j++) {         /* read via the (now removed) target */
        tmp[j] = mem_read8(cpu, saved.to + j);
    }
    for (u32 j = 0; j < a.size; j++) mem_write8(cpu, saved.from + j, tmp[j]);
    free(tmp);
}

void alias_add(CPUState *cpu, u32 from, u32 to, u32 size) {
    for (int i = g_alias_count - 1; i >= 0; i--) {
        MemAlias *a = &g_alias[i];
        int overlap = from < a->from + a->size && a->from < from + size;
        if (overlap || a->to == to) alias_remove(cpu, i);
    }
    if (g_alias_count == MAX_ALIASES) alias_remove(cpu, 0);
    g_alias[g_alias_count++] = (MemAlias){from, to, size};
    page_table_rebuild();
}

u8 **g_page;
void page_table_rebuild(void) {
    if (!g_page && !(g_page = calloc(1u << 20, sizeof *g_page))) return;
    memset(g_page, 0, (1u << 20) * sizeof *g_page);
    if (!cpu.ram) return;
    for (u32 seg = 0; seg < 2; seg++) {               /* cached 0x80.., uncached 0xC0.. */
        u32 base = seg ? 0xC0000000u : 0x80000000u;
        for (u32 off = 0; off < cpu.ram_size; off += 0x1000) {
            u32 a = base + off, lo = off, hi = off + 0x1000;
            u8 *p = cpu.ram + off;
            for (int i = 0; i < g_alias_count && p; i++) {
                u32 f = g_alias[i].from & 0x3FFFFFFFu, t = g_alias[i].to & 0x3FFFFFFFu, n = g_alias[i].size;
                if (hi <= f || lo >= f + n) continue;          /* untouched */
                if (lo >= f && hi <= f + n && t + (lo - f) + 0x1000 <= cpu.ram_size)
                    p = cpu.ram + t + (lo - f);               /* whole page aliased */
                else p = 0;                                    /* partial: slow path */
            }
            g_page[a >> 12] = p;
        }
    }
    for (int i = 0; i < g_host_region_count; i++)
        for (u32 off = 0; off + 0x1000 <= g_host_region[i].size; off += 0x1000)
            if (!((g_host_region[i].base + off) & 0xFFF))
                g_page[(g_host_region[i].base + off) >> 12] = g_host_region[i].mem + off;
}

static int nmods(void) { return (int)(sizeof k_mods / sizeof k_mods[0]) - 1; }

/* ctx->host_call: called by every dolrecomp_call before the local lookup. */
bool module_host_call(CPUState *cpu, u32 addr) {
    addr = alias_translate(addr);
    if (addr >= MODULE_WINDOW_BASE && addr < MODULE_WINDOW_END) {
        for (int i = 0; i < nmods(); i++) {
            if (addr - k_mods[i].base < k_mods[i].size) {
                DolRecompFunction fn = k_mods[i].find(addr);
                if (!fn) return false;
                cpu->pc = addr;
                fn(cpu);
                return true;
            }
        }
        return false;
    }
    /* module code calling into main.dol (memcpy, engine APIs) */
    if (engine_find) {
        DolRecompFunction fn = engine_find(addr);
        if (fn) { cpu->pc = addr; fn(cpu); return true; }
    }
    return false;
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* Copy a pre-linked module (.dol written by gctool.py) into guest RAM. */
static void copy_dol(CPUState *cpu, const u8 *d, long n);
static int install_image(CPUState *cpu, const char *name) {
    u32 esz;
    const u8 *emb = plat_embedded_module(name, &esz);
    if (emb) { copy_dol(cpu, emb, (long)esz); return 1; }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.dol", module_dir ? module_dir : ".", name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[mod] missing pre-linked image %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    copy_dol(cpu, d, n);
    free(d);
    return 1;
}

static void copy_dol(CPUState *cpu, const u8 *d, long n) {
    for (int i = 0; i < 18; i++) {
        u32 off = be32(d + i * 4), addr = be32(d + 0x48 + i * 4), sz = be32(d + 0x90 + i * 4);
        for (u32 j = 0; j < sz && off + j < (u32)n; j++) mem_write8(cpu, addr + j, d[off + j]);
    }
}

static int find_module(CPUState *cpu, u32 mod, char *name) {
    u32 rec = mod - 0x40;                /* record header: name at +0x10 */
    for (int i = 0; i < 0x20; i++) name[i] = (char)mem_read8(cpu, rec + 0x10 + i);
    name[0x20] = 0;
    for (int i = 0; i < nmods(); i++)
        if (strcmp(k_mods[i].name, name) == 0) return i;
    return -1;
}

/* HLE for LU_FixupModuleHeader (fn_800FF72C): the engine converts the module
 * header's offsets into pointers, +0x08 being the image base. We do the same, but
 * place the image at the module's fixed address with its pre-linked contents. */
#define LU_MODULE_VTABLE 0x80273188u
void module_fixup_hook(CPUState *cpu) {
    u32 mod = cpu->gpr[3];
    mem_write32(cpu, mod + 0x00, LU_MODULE_VTABLE);
    static const u32 fields[] = {0x08, 0x10, 0x18, 0x20, 0x24};
    for (unsigned i = 0; i < 5; i++)
        mem_write32(cpu, mod + fields[i], mod + mem_read32(cpu, mod + fields[i]));
    char name[0x21];
    int m = find_module(cpu, mod, name);
    if (m < 0) {
        fprintf(stderr, "[mod] no pre-linked module named '%s' (record %08X): left in heap\n", name, mod);
        return;
    }
    u32 heap = mem_read32(cpu, mod + 0x08);
    if (install_image(cpu, name)) {
        /* Keep +0x08 at the heap address: pointers the engine derives from the pack
         * (class descriptors etc.) and from the header all resolve through the alias. */
        alias_add(cpu, heap, k_mods[m].base, mem_read32(cpu, mod + 0x04));
        if (module_trace)
            fprintf(stderr, "[mod] %s: heap image %08X aliased to pre-linked %08X\n", name, heap, k_mods[m].base);
    }
}

/* HLE for LU_RelocateBatch (fn_800FF904): pre-linked modules need no relocation.
 * A module not in the layout would need the original relocator (not supported). */
void module_relocate_hook(CPUState *cpu) {
    char name[0x21];
    u32 mod = cpu->gpr[3];
    if (cpu->gpr[4] == 0 && find_module(cpu, mod, name) < 0)
        fprintf(stderr, "[mod] relocation requested for unknown module '%s'\n", name);
}
