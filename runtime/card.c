/* Memory card: native replacement for the Dolphin SDK CARD library.
 *
 * Saves live in a host folder ($EED_MEMCARD, default ./memcard) as
 *   <fileName>.dat   file contents
 *   <fileName>.meta  CARDStat bytes 0x24..0x3B (time, game code, banner/icon/comment info)
 * The game's calls use NULL callbacks and poll CARDGetResultCode, so every
 * operation completes immediately and records its result per channel.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "card.h"

enum { CARD_READY = 0, CARD_BUSY = -1, CARD_NOCARD = -3, CARD_NOFILE = -4, CARD_IOERROR = -5,
       CARD_EXIST = -7, CARD_NOENT = -8, CARD_INSSPACE = -9, CARD_NAMETOOLONG = -12, CARD_FATAL = -128 };

#define MAX_FILES 127
#define CARD_BYTES (59 * 8192)          /* a 59-block card (memSize 4 Mbit) */
static char g_names[MAX_FILES][33];     /* fileNo -> name ("" = free) */
static s32 g_result[2];
static int g_loaded;

static const char *card_dir(void) {
    const char *d = getenv("EED_MEMCARD");
    return d ? d : "memcard";
}
static void path_for(const char *name, const char *ext, char *out, size_t n) {
    snprintf(out, n, "%s/%s.%s", card_dir(), name, ext);
}
static long file_size(const char *name) {
    char p[512];
    struct stat st;
    path_for(name, "dat", p, sizeof p);
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}
static void load_dir(void) {
    if (g_loaded) return;
    g_loaded = 1;
    mkdir(card_dir(), 0755);
    DIR *d = opendir(card_dir());
    if (!d) return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < MAX_FILES) {
        size_t l = strlen(e->d_name);
        if (l > 4 && l - 4 <= 32 && !strcmp(e->d_name + l - 4, ".dat")) {
            memcpy(g_names[n], e->d_name, l - 4);
            g_names[n][l - 4] = 0;
            n++;
        }
    }
    closedir(d);
}
static int find(const char *name) {
    for (int i = 0; i < MAX_FILES; i++) if (g_names[i][0] && !strcmp(g_names[i], name)) return i;
    return -1;
}
static void guest_name(CPUState *c, u32 a, char out[33]) {
    int i = 0;
    for (; i < 32; i++) { out[i] = (char)mem_read8(c, a + i); if (!out[i]) break; }
    out[i] = 0;
}
static int ok_chan(u32 chan) { return chan < 2; }
static void set(CPUState *c, u32 chan, s32 r) {
    if (ok_chan(chan)) g_result[chan] = r;
    c->gpr[3] = (u32)r;
}
static long used_bytes(void) {
    long t = 0;
    for (int i = 0; i < MAX_FILES; i++) if (g_names[i][0]) t += (file_size(g_names[i]) + 8191) / 8192 * 8192;
    return t;
}
/* CARDFileInfo: { s32 chan; s32 fileNo; s32 offset; s32 length; u16 iBlock; } */
static void fill_info(CPUState *c, u32 fi, u32 chan, int no) {
    mem_write32(c, fi + 0x0, chan);
    mem_write32(c, fi + 0x4, (u32)no);
    mem_write32(c, fi + 0x8, 0);
    mem_write32(c, fi + 0xC, (u32)file_size(g_names[no]));
    mem_write16(c, fi + 0x10, (u16)no);
}

void card_GetResultCode(CPUState *c) { u32 ch = c->gpr[3]; c->gpr[3] = ok_chan(ch) ? (u32)g_result[ch] : (u32)CARD_FATAL; }
/* s32 CARDFreeBlocks(s32 chan, s32* bytesNotUsed, s32* filesNotUsed) */
void card_FreeBlocks(CPUState *c) {
    u32 ch = c->gpr[3];
    if (ch != 0) { set(c, ch, CARD_NOCARD); return; }
    load_dir();
    int files = 0;
    for (int i = 0; i < MAX_FILES; i++) if (!g_names[i][0]) files++;
    if (c->gpr[4]) mem_write32(c, c->gpr[4], (u32)(CARD_BYTES - used_bytes()));
    if (c->gpr[5]) mem_write32(c, c->gpr[5], (u32)files);
    set(c, ch, CARD_READY);
}

/* s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) */
void card_ProbeEx(CPUState *c) {
    u32 ch = c->gpr[3];
    if (ch != 0) { c->gpr[3] = (u32)CARD_NOCARD; return; }   /* one card, in slot A */
    load_dir();
    if (c->gpr[4]) mem_write32(c, c->gpr[4], 4);              /* 4 Mbit = 59 blocks */
    if (c->gpr[5]) mem_write32(c, c->gpr[5], 8192);
    c->gpr[3] = CARD_READY;
}
void card_MountAsync(CPUState *c) { u32 ch = c->gpr[3]; load_dir(); set(c, ch, ch == 0 ? CARD_READY : CARD_NOCARD); }
void card_Mount(CPUState *c) { card_MountAsync(c); }
void card_Unmount(CPUState *c) { set(c, c->gpr[3], c->gpr[3] == 0 ? CARD_READY : CARD_NOCARD); }
void card_CheckAsync(CPUState *c) { set(c, c->gpr[3], c->gpr[3] == 0 ? CARD_READY : CARD_NOCARD); }

void card_FormatAsync(CPUState *c) {
    char p[512];
    for (int i = 0; i < MAX_FILES; i++) if (g_names[i][0]) {
        path_for(g_names[i], "dat", p, sizeof p); remove(p);
        path_for(g_names[i], "meta", p, sizeof p); remove(p);
        g_names[i][0] = 0;
    }
    set(c, c->gpr[3], CARD_READY);
}

/* s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo) */
void card_Open(CPUState *c) {
    u32 ch = c->gpr[3];
    char name[33];
    guest_name(c, c->gpr[4], name);
    int no = find(name);
    if (no < 0) { set(c, ch, CARD_NOFILE); return; }
    fill_info(c, c->gpr[5], ch, no);
    set(c, ch, CARD_READY);
}

/* s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo, cb) */
void card_CreateAsync(CPUState *c) {
    u32 ch = c->gpr[3], size = c->gpr[5];
    char name[33], p[512];
    guest_name(c, c->gpr[4], name);
    if (find(name) >= 0) { set(c, ch, CARD_EXIST); return; }
    if (used_bytes() + (long)size > CARD_BYTES) { set(c, ch, CARD_INSSPACE); return; }
    int no = -1;
    for (int i = 0; i < MAX_FILES; i++) if (!g_names[i][0]) { no = i; break; }
    if (no < 0) { set(c, ch, CARD_NOENT); return; }
    path_for(name, "dat", p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) { set(c, ch, CARD_IOERROR); return; }
    for (u32 i = 0; i < size; i++) fputc(0, f);
    fclose(f);
    snprintf(g_names[no], sizeof g_names[no], "%s", name);
    fill_info(c, c->gpr[6], ch, no);
    fprintf(stderr, "[card] created '%s' (%u bytes)\n", name, size);
    set(c, ch, CARD_READY);
}

void card_Close(CPUState *c) { c->gpr[3] = CARD_READY; }

static void rw(CPUState *c, int write) {
    u32 fi = c->gpr[3], buf = c->gpr[4], len = c->gpr[5], off = c->gpr[6];
    u32 ch = mem_read32(c, fi), no = mem_read32(c, fi + 4);
    if (no >= MAX_FILES || !g_names[no][0]) { set(c, ch, CARD_NOFILE); return; }
    char p[512];
    path_for(g_names[no], "dat", p, sizeof p);
    FILE *f = fopen(p, write ? "r+b" : "rb");
    if (!f) { set(c, ch, CARD_IOERROR); return; }
    fseek(f, off, SEEK_SET);
    for (u32 i = 0; i < len; i++) {
        if (write) fputc(mem_read8(c, buf + i), f);
        else { int b = fgetc(f); mem_write8(c, buf + i, (u8)(b < 0 ? 0 : b)); }
    }
    fclose(f);
    set(c, ch, CARD_READY);
}
/* s32 CARDReadAsync / CARDWriteAsync(CARDFileInfo*, void* buf, s32 length, s32 offset, cb) */
void card_ReadAsync(CPUState *c) { rw(c, 0); }
void card_WriteAsync(CPUState *c) { rw(c, 1); }

/* s32 CARDDeleteAsync(s32 chan, const char* fileName, cb) */
void card_DeleteAsync(CPUState *c) {
    u32 ch = c->gpr[3];
    char name[33], p[512];
    guest_name(c, c->gpr[4], name);
    int no = find(name);
    if (no < 0) { set(c, ch, CARD_NOFILE); return; }
    path_for(name, "dat", p, sizeof p); remove(p);
    path_for(name, "meta", p, sizeof p); remove(p);
    g_names[no][0] = 0;
    set(c, ch, CARD_READY);
}

/* CARDStat: fileName[32] @0, length @0x20, time @0x24, gameName[4] @0x28, company[2] @0x2C,
 * bannerFormat @0x2E, iconAddr @0x30, iconFormat @0x34, iconSpeed @0x36, commentAddr @0x38,
 * offsetBanner @0x3C, offsetBannerTlut @0x40, offsetIcon[8] @0x44, offsetIconTlut @0x64, offsetData @0x68 */
static void update_offsets(CPUState *c, u32 st) {
    u32 off = mem_read32(c, st + 0x30);
    u8 bfmt = mem_read8(c, st + 0x2E);
    u16 ifmt = mem_read16(c, st + 0x34);
    if (off == 0xFFFFFFFFu) { bfmt = 0; ifmt = 0; off = 0; }
    u32 ob = 0xFFFFFFFFu, obt = 0xFFFFFFFFu;
    if ((bfmt & 3) == 1) { ob = off; off += 96 * 32; obt = off; off += 512; }          /* C8 */
    else if ((bfmt & 3) == 2) { ob = off; off += 2 * 96 * 32; }                        /* RGB5A3 */
    mem_write32(c, st + 0x3C, ob);
    mem_write32(c, st + 0x40, obt);
    int tlut = 0;
    for (int i = 0; i < 8; i++) {
        u32 f = (ifmt >> (2 * i)) & 3, oi = 0xFFFFFFFFu;
        if (f == 1) { oi = off; off += 32 * 32; tlut = 1; }
        else if (f == 2) { oi = off; off += 2 * 32 * 32; }
        mem_write32(c, st + 0x44 + 4 * i, oi);
    }
    mem_write32(c, st + 0x64, tlut ? off : 0xFFFFFFFFu);
    if (tlut) off += 512;
    mem_write32(c, st + 0x68, off);
}

/* s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) */
void card_GetStatus(CPUState *c) {
    u32 ch = c->gpr[3], no = c->gpr[4], st = c->gpr[5];
    if (no >= MAX_FILES || !g_names[no][0]) { set(c, ch, CARD_NOFILE); return; }
    for (u32 i = 0; i < 0x6C; i++) mem_write8(c, st + i, 0);
    for (u32 i = 0; i < 32 && g_names[no][i]; i++) mem_write8(c, st + i, (u8)g_names[no][i]);
    mem_write32(c, st + 0x20, (u32)file_size(g_names[no]));
    for (u32 i = 0; i < 6; i++) mem_write8(c, st + 0x28 + i, mem_read8(c, 0x80000000u + i)); /* game + company */
    mem_write32(c, st + 0x30, 0xFFFFFFFFu);
    mem_write32(c, st + 0x38, 0xFFFFFFFFu);
    char p[512];
    path_for(g_names[no], "meta", p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (f) {
        u8 m[0x18];
        if (fread(m, 1, sizeof m, f) == sizeof m) for (u32 i = 0; i < sizeof m; i++) mem_write8(c, st + 0x24 + i, m[i]);
        fclose(f);
    }
    update_offsets(c, st);
    set(c, ch, CARD_READY);
}

/* s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat) */
void card_SetStatus(CPUState *c) {
    u32 ch = c->gpr[3], no = c->gpr[4], st = c->gpr[5];
    if (no >= MAX_FILES || !g_names[no][0]) { set(c, ch, CARD_NOFILE); return; }
    char p[512];
    path_for(g_names[no], "meta", p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (f) {
        for (u32 i = 0; i < 0x18; i++) fputc(mem_read8(c, st + 0x24 + i), f);
        fclose(f);
    }
    update_offsets(c, st);
    set(c, ch, CARD_READY);
}
