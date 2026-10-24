/* Disc access for DVD emulation.
 *
 * Two backends behind one disc_read(offset, dst, len):
 *  - Image: a plain GameCube ISO/GCM, or a CISO (as produced by common dumping
 *    tools). This is what the Android build uses: point it at your own dump.
 *  - Directory: an extracted disc (sys/ + files/ as written by `dtk disc extract`
 *    or `dtk vfs cp game.ciso: dir`). Disc offsets are mapped back to files via
 *    sys/fst.bin. Missing files read as zeros, so a partial extraction works.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "disc.h"

static FILE *g_img;
static int g_is_ciso;
static u32 g_ciso_block;
static u32 *g_ciso_map; /* block index -> stored block number + 1 (0 = absent) */
static u32 g_ciso_nblocks;

typedef struct { u32 off, len; char *path; } DiscFile;
static DiscFile *g_files;
static int g_nfiles;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 le32(const u8 *p) { return (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0]; }

static u8 *slurp(const char *path, u32 *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n ? n : 1);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    *size = (u32)n;
    return d;
}

static void add_file(u32 off, u32 len, const char *path) {
    g_files = realloc(g_files, sizeof(DiscFile) * (g_nfiles + 1));
    g_files[g_nfiles].off = off;
    g_files[g_nfiles].len = len;
    g_files[g_nfiles].path = strdup(path);
    g_nfiles++;
}

static int cmp_file(const void *a, const void *b) {
    const DiscFile *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

/* Walk the FST: entry 0 is the root; directories store the index after their last child. */
static void walk_fst(const u8 *fst, u32 count, const char *strtab, u32 *idx, u32 end, const char *dir) {
    while (*idx < end) {
        const u8 *e = fst + *idx * 12;
        u32 w = be32(e), a = be32(e + 4), b = be32(e + 8);
        const char *name = strtab + (w & 0xFFFFFF);
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (w >> 24) {
            (*idx)++;
            walk_fst(fst, count, strtab, idx, b, path);
        } else {
            add_file(a, b, path);
            (*idx)++;
        }
    }
}

static int open_dir(const char *root) {
    char p[1024];
    u32 bsz, fsz;
    snprintf(p, sizeof p, "%s/sys/boot.bin", root);
    u8 *boot = slurp(p, &bsz);
    snprintf(p, sizeof p, "%s/sys/fst.bin", root);
    u8 *fst = slurp(p, &fsz);
    if (!fst) { fprintf(stderr, "[disc] %s: no sys/fst.bin\n", root); free(boot); return 0; }
    if (boot && bsz >= 0x430) {
        snprintf(p, sizeof p, "%s/sys/boot.bin", root);   add_file(0, 0x440, p);
        snprintf(p, sizeof p, "%s/sys/bi2.bin", root);    add_file(0x440, 0x2000, p);
        snprintf(p, sizeof p, "%s/sys/apploader.img", root); add_file(0x2440, be32(boot + 0x420) - 0x2440, p);
        snprintf(p, sizeof p, "%s/sys/main.dol", root);   add_file(be32(boot + 0x420), be32(boot + 0x424) - be32(boot + 0x420), p);
        snprintf(p, sizeof p, "%s/sys/fst.bin", root);    add_file(be32(boot + 0x424), fsz, p);
    }
    u32 count = be32(fst + 8), idx = 1;
    snprintf(p, sizeof p, "%s/files", root);
    walk_fst(fst, count, (const char *)fst + count * 12, &idx, count, p);
    qsort(g_files, g_nfiles, sizeof(DiscFile), cmp_file);
    fprintf(stderr, "[disc] directory %s: %d files\n", root, g_nfiles);
    free(boot);
    free(fst);
    return 1;
}

static int open_image(const char *path) {
    g_img = fopen(path, "rb");
    if (!g_img) return 0;
    u8 h[8];
    if (fread(h, 1, 8, g_img) != 8) return 0;
    if (!memcmp(h, "CISO", 4)) {
        g_is_ciso = 1;
        g_ciso_block = le32(h + 4);
        u8 map[0x8000 - 8];
        if (fread(map, 1, sizeof map, g_img) != sizeof map) return 0;
        g_ciso_nblocks = sizeof map;
        g_ciso_map = calloc(g_ciso_nblocks, sizeof(u32));
        u32 n = 0;
        for (u32 i = 0; i < g_ciso_nblocks; i++) if (map[i]) g_ciso_map[i] = ++n;
        fprintf(stderr, "[disc] CISO %s: block size %u, %u blocks present\n", path, g_ciso_block, n);
    } else {
        fprintf(stderr, "[disc] ISO %s\n", path);
    }
    return 1;
}

int disc_open(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) { perror(path); return 0; }
    return S_ISDIR(st.st_mode) ? open_dir(path) : open_image(path);
}

static void read_image(u32 off, u8 *dst, u32 len) {
    if (!g_is_ciso) {
        fseek(g_img, off, SEEK_SET);
        size_t got = fread(dst, 1, len, g_img);
        if (got < len) memset(dst + got, 0, len - got);
        return;
    }
    while (len) {
        u32 blk = off / g_ciso_block, in = off % g_ciso_block;
        u32 n = g_ciso_block - in;
        if (n > len) n = len;
        if (blk < g_ciso_nblocks && g_ciso_map[blk]) {
            fseek(g_img, 0x8000 + (long)(g_ciso_map[blk] - 1) * g_ciso_block + in, SEEK_SET);
            size_t got = fread(dst, 1, n, g_img);
            if (got < n) memset(dst + got, 0, n - got);
        } else {
            memset(dst, 0, n);
        }
        off += n; dst += n; len -= n;
    }
}

int disc_trace = 1;

static void read_dir(u32 off, u8 *dst, u32 len) {
    memset(dst, 0, len);
    /* find the last file starting at or before off */
    int lo = 0, hi = g_nfiles - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_files[mid].off <= off) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    for (int i = best < 0 ? 0 : best; i < g_nfiles && g_files[i].off < off + len; i++) {
        DiscFile *f = &g_files[i];
        if (f->off + f->len <= off) continue;
        u32 s = off > f->off ? off : f->off;
        u32 e = (off + len) < (f->off + f->len) ? off + len : f->off + f->len;
        if (disc_trace) fprintf(stderr, "[disc] read %s +0x%X len 0x%X\n", f->path, s - f->off, e - s);
        FILE *fp = fopen(f->path, "rb");
        if (!fp) { if (disc_trace) fprintf(stderr, "[disc]   (missing, zeros)\n"); continue; }
        fseek(fp, s - f->off, SEEK_SET);
        if (fread(dst + (s - off), 1, e - s, fp) != e - s && disc_trace)
            fprintf(stderr, "[disc]   short read\n");
        fclose(fp);
    }
}

void disc_read(u32 off, void *dst, u32 len) {
    if (g_img) read_image(off, dst, len);
    else read_dir(off, dst, len);
}
