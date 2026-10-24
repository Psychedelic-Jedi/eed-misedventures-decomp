/* Linux test front end: runs the game headless with a timeout, writes frames as
 * PPM images (EED_FRAMES=dir, every EED_FRAME_EVERY frames) and takes scripted input
 * (EED_INPUT). Usage: boot_test main.dol seconds disc(.ciso|.iso|dir) */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "core.h"
#include "input.h"
#include "modules.h"
#include "platform.h"

static void on_alarm(int s) {
    (void)s;
    if (getenv("EED_RAMDUMP")) { FILE *f = fopen(getenv("EED_RAMDUMP"), "wb"); if (f) { fwrite(cpu.ram, 1, cpu.ram_size, f); fclose(f); } }
    eed_core_debug_dump();
    fprintf(stderr, "\n[timeout]\n");
    exit(2);
}

static unsigned long long g_frame;
void plat_present_frame(const u8 *rgba, int stride, int x0, int y0, int w, int h) {
    const char *dir = getenv("EED_FRAMES");
    int every = getenv("EED_FRAME_EVERY") ? atoi(getenv("EED_FRAME_EVERY")) : 30;
    unsigned long long n = g_frame++;
    if (!dir || n % (unsigned)every) return;
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%05llu.ppm", dir, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) fwrite(rgba + ((size_t)y * stride + x) * 4, 1, 3, f);
    fclose(f);
}
void plat_input_update(unsigned long long frame) { input_update(frame); }
const u8 *plat_embedded_module(const char *name, u32 *size) { (void)name; (void)size; return NULL; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s main.dol seconds disc.(ciso|iso)|dir\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    module_dir = getenv("EED_MODULES");
    if (!eed_core_init(d, (u32)n, argv[3])) return 1;
    signal(SIGALRM, on_alarm);
    alarm(atoi(argv[2]));
    static volatile int stop;
    int r = eed_core_run(&stop);
    eed_core_debug_dump();
    return r ? 1 : 0;
}
