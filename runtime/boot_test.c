/* Linux test front end: runs the game headless with a timeout, writes frames as
 * PPM images (EED_FRAMES=dir, every EED_FRAME_EVERY frames) and takes scripted input
 * (EED_INPUT). Usage: boot_test main.dol seconds disc(.ciso|.iso|dir) */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "core.h"
#include "input.h"
#include "modules.h"
#include "platform.h"
#include "gx_render.h"
#include "gl_api.h"

static unsigned long long g_frame;
static unsigned long long g_frame_count(void) { return g_frame; }

/* EED_PROF=file: sample the host PC 1000x/s of CPU time and dump the raw samples */
static unsigned long g_samples[1 << 20];
static int g_nsamples;
static void on_prof(int s, siginfo_t *si, void *uc) {
    (void)s; (void)si;
    if (g_nsamples < (int)(sizeof g_samples / sizeof *g_samples))
        g_samples[g_nsamples++] = ((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
}
static void prof_dump(void) {
    const char *p = getenv("EED_PROF");
    if (!p) return;
    FILE *f = fopen(p, "w");
    extern int main(int, char **);
    if (f) fprintf(f, "main %lx\n", (unsigned long)&main);
    for (int i = 0; f && i < g_nsamples; i++) fprintf(f, "%lx\n", g_samples[i]);
    if (f) fclose(f);
}
static struct timespec g_t0;
static void on_alarm(int s) {
    (void)s;
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "[perf] %llu frames presented in %.1fs\n", g_frame_count(), (t1.tv_sec - g_t0.tv_sec) + (t1.tv_nsec - g_t0.tv_nsec) / 1e9);
    prof_dump();
    if (getenv("EED_RAMDUMP")) { FILE *f = fopen(getenv("EED_RAMDUMP"), "wb"); if (f) { fwrite(cpu.ram, 1, cpu.ram_size, f); fclose(f); } }
    eed_core_debug_dump();
    fprintf(stderr, "\n[timeout]\n");
    exit(2);
}

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
/* GLES backend: read the EFB back only for the frames being saved */
void plat_present_gl(unsigned tex, int tex_w, int tex_h, int x0, int y0, int w, int h) {
    (void)tex; (void)tex_w; (void)tex_h;
    const char *dir = getenv("EED_FRAMES");
    int every = getenv("EED_FRAME_EVERY") ? atoi(getenv("EED_FRAME_EVERY")) : 30;
    unsigned long long n = g_frame++;
    if (!dir || n % (unsigned)every) return;
    u8 *buf = malloc((size_t)w * h * 4);
    gles_read_efb(buf, x0, y0, w, h);
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%05llu.ppm", dir, n);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++) fwrite(buf + i * 4, 1, 3, f);
        fclose(f);
    }
    free(buf);
}

/* EED_GLES=1: render with the GLES backend on a surfaceless Mesa context */
static int init_gles(void) {
    setenv("EGL_PLATFORM", "surfaceless", 0);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) { fprintf(stderr, "eglInitialize failed 0x%X\n", eglGetError()); return 0; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(d, attrs, &cfg, 1, &n) || !n) { fprintf(stderr, "eglChooseConfig failed\n"); return 0; }
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, getenv("EED_GLES2") ? 2 : 3, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
    if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "EGL context failed 0x%X\n", eglGetError()); return 0; }
    gx_use_gles = 1;
    if (getenv("EED_SCALE")) gx_gles_scale = atoi(getenv("EED_SCALE"));
    return 1;
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
    if (getenv("EED_GLES") && !init_gles()) return 1;
    if (!eed_core_init(d, (u32)n, argv[3])) return 1;
    signal(SIGALRM, on_alarm);
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    if (getenv("EED_PROF")) {
        struct sigaction sa = {0};
        sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        struct itimerval it = {{0, 1000}, {0, 1000}};
        setitimer(ITIMER_PROF, &it, NULL);
    }
    alarm(atoi(argv[2]));
    static volatile int stop;
    int r = eed_core_run(&stop);
    eed_core_debug_dump();
    return r ? 1 : 0;
}
