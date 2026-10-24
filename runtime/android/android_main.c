/* Android front end (NativeActivity, no Java).
 *
 * - Game thread: owns the EGL context, runs the recompiled game (eed_core_run),
 *   presents frames (plat_present_frame) and polls input (plat_input_update).
 * - UI thread: NativeActivity callbacks only hand over the window / input queue.
 * - The disc image (.ciso/.iso/.gcm) is looked up in Download/; the app needs
 *   "All files access" (Settings > Apps > Ed, Edd n Eddy > Permissions).
 * - stderr goes to Download/eed_log.txt for troubleshooting.
 */
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dirent.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../core.h"
#include "../input.h"
#include "../platform.h"
#include "eed_gl.h"

#define TAG "eed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* Embedded by the build (embed.S): main.dol and the pre-linked LU modules. */
extern const u8 eed_main_dol[];
extern const u32 eed_main_dol_size;
typedef struct { const char *name; const u8 *data; u32 size; } EmbeddedModule;
extern const EmbeddedModule eed_modules[];
extern const int eed_module_count;

const u8 *plat_embedded_module(const char *name, u32 *size) {
    for (int i = 0; i < eed_module_count; i++)
        if (!strcmp(eed_modules[i].name, name)) { *size = eed_modules[i].size; return eed_modules[i].data; }
    return NULL;
}

/* ------------------------------------------------------------ shared state */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static ANativeWindow *g_window;         /* set by UI thread */
static AInputQueue *g_queue;
static int g_queue_changed, g_window_changed;
static volatile int g_stop;

/* ------------------------------------------------------------ EGL / GL */
static EGLDisplay g_dpy;
static EGLConfig g_cfg;
static EGLContext g_ctx;
static EGLSurface g_surf;
static ANativeWindow *g_cur_window;
static int g_w, g_h;
static GLuint g_tex, g_prog_tex, g_prog_ui;


static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOGI("shader: %s", log); }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    return p;
}

static void gl_init_context(void) {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g_dpy, NULL, NULL);
    /* GLES 3 if available (24-bit depth for the EFB), else GLES 2 */
    for (int ver = 3; ver >= 2 && !g_ctx; ver--) {
        const EGLint attrs[] = {EGL_RENDERABLE_TYPE, ver == 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
                                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLint n = 0;
        if (!eglChooseConfig(g_dpy, attrs, &g_cfg, 1, &n) || !n) continue;
        const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, ver, EGL_NONE};
        g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attrs);
    }
}

static void gl_init_resources(void) {
    /* full-screen textured quad: a_pos in [0,1]^2, placed by u_rect (x, y, w, h in NDC) */
    const char *vs =
        "attribute vec2 a_pos; uniform vec4 u_rect; varying vec2 v_uv;\n"
        "void main() { v_uv = a_pos; gl_Position = vec4(u_rect.xy + a_pos * u_rect.zw, 0.0, 1.0); }\n";
    const char *fs_tex =
        "precision mediump float; varying vec2 v_uv; uniform sampler2D u_tex; uniform vec4 u_color;\n"
        "void main() { gl_FragColor = vec4(texture2D(u_tex, u_color.xy + v_uv * u_color.zw).rgb, 1.0); }\n";
    const char *fs_ui =   /* soft-edged circle for touch controls */
        "precision mediump float; varying vec2 v_uv; uniform vec4 u_color;\n"
        "void main() { float d = length(v_uv - 0.5) * 2.0; float a = smoothstep(1.0, 0.92, d);\n"
        "  float ring = smoothstep(0.80, 0.86, d) * a; gl_FragColor = vec4(u_color.rgb, u_color.a * max(a * 0.35, ring)); }\n";
    g_prog_tex = program(vs, fs_tex);
    g_prog_ui = program(vs, fs_ui);
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static const GLfloat k_quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
static void draw_rect(GLuint prog, float x, float y, float w, float h) {
    glUseProgram(prog);
    glUniform4f(glGetUniformLocation(prog, "u_rect"), x, y, w, h);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, k_quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Attach/detach the window surface; blocks while the app has no window (paused). */
static int gl_ensure_surface(void) {
    pthread_mutex_lock(&g_mu);
    while (!g_window && !g_stop) pthread_cond_wait(&g_cv, &g_mu);
    ANativeWindow *w = g_window;
    int changed = g_window_changed;
    g_window_changed = 0;
    pthread_mutex_unlock(&g_mu);
    if (g_stop) return 0;
    if (changed || w != g_cur_window) {
        if (g_surf) { eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroySurface(g_dpy, g_surf); }
        g_surf = eglCreateWindowSurface(g_dpy, g_cfg, (EGLNativeWindowType)w, NULL);
        eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);
        eglSwapInterval(g_dpy, 0);
        static int res_ready;
        if (!res_ready) { gl_init_resources(); res_ready = 1; }
        g_cur_window = w;
    }
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &g_w);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &g_h);
    return 1;
}

/* ------------------------------------------------------------ touch controls */
typedef struct { const char *name; float x, y, r; u16 button; float cr, cg, cb; } TouchButton;
/* positions in screen fractions (x right, y down), radius as fraction of screen height */
static const TouchButton k_buttons[] = {
    {"A", 0.88f, 0.70f, 0.10f, PAD_A, 0.2f, 0.85f, 0.4f},
    {"B", 0.77f, 0.82f, 0.07f, PAD_B, 0.9f, 0.25f, 0.25f},
    {"X", 0.95f, 0.50f, 0.06f, PAD_X, 0.85f, 0.85f, 0.85f},
    {"Y", 0.82f, 0.48f, 0.06f, PAD_Y, 0.85f, 0.85f, 0.85f},
    {"START", 0.50f, 0.92f, 0.05f, PAD_START, 1.0f, 1.0f, 1.0f},
    {"Z", 0.93f, 0.25f, 0.055f, PAD_Z, 0.5f, 0.4f, 0.9f},
    {"L", 0.06f, 0.12f, 0.07f, PAD_L, 0.8f, 0.8f, 0.8f},
    {"R", 0.94f, 0.12f, 0.07f, PAD_R, 0.8f, 0.8f, 0.8f},
};
#define NBUTTONS (int)(sizeof k_buttons / sizeof k_buttons[0])
static const float k_stick_x = 0.14f, k_stick_y = 0.70f, k_stick_r = 0.17f;

#define MAX_POINTERS 10
static struct { int active, id, target; float x, y; } g_ptr[MAX_POINTERS]; /* target: -1 none, -2 stick, >=0 button */
static u16 g_gamepad_buttons;
static float g_gp_lx, g_gp_ly, g_gp_rx, g_gp_ry, g_gp_lt, g_gp_rt;
static int g_show_touch = 1;

static int touch_target(float x, float y) {
    float ax = (float)g_h / (float)g_w;  /* radius is in screen-height units */
    float dx = (x - k_stick_x) / ax, dy = y - k_stick_y;
    if (dx * dx + dy * dy < (k_stick_r * 1.4f) * (k_stick_r * 1.4f)) return -2;
    for (int i = 0; i < NBUTTONS; i++) {
        dx = (x - k_buttons[i].x) / ax; dy = y - k_buttons[i].y;
        if (dx * dx + dy * dy < (k_buttons[i].r * 1.3f) * (k_buttons[i].r * 1.3f)) return i;
    }
    return -1;
}

static void handle_motion(AInputEvent *e) {
    int32_t src = AInputEvent_getSource(e);
    int32_t act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        g_gp_lx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
        g_gp_ly = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
        g_gp_rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0);
        g_gp_ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
        g_gp_lt = fmaxf(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0));
        g_gp_rt = fmaxf(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0));
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        g_gamepad_buttons &= ~(PAD_LEFT | PAD_RIGHT | PAD_UP | PAD_DOWN);
        if (hx < -0.5f) g_gamepad_buttons |= PAD_LEFT;
        if (hx > 0.5f) g_gamepad_buttons |= PAD_RIGHT;
        if (hy < -0.5f) g_gamepad_buttons |= PAD_UP;
        if (hy > 0.5f) g_gamepad_buttons |= PAD_DOWN;
        g_show_touch = 0;
        return;
    }
    size_t idx = (act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        g_show_touch = 1;
        int32_t id = AMotionEvent_getPointerId(e, idx);
        float x = AMotionEvent_getX(e, idx) / (float)g_w, y = AMotionEvent_getY(e, idx) / (float)g_h;
        for (int i = 0; i < MAX_POINTERS; i++) if (!g_ptr[i].active) {
            g_ptr[i].active = 1; g_ptr[i].id = id; g_ptr[i].x = x; g_ptr[i].y = y; g_ptr[i].target = touch_target(x, y);
            break;
        }
    } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP || kind == AMOTION_EVENT_ACTION_CANCEL) {
        int32_t id = AMotionEvent_getPointerId(e, idx);
        for (int i = 0; i < MAX_POINTERS; i++) if (g_ptr[i].active && (g_ptr[i].id == id || kind == AMOTION_EVENT_ACTION_CANCEL)) g_ptr[i].active = 0;
    } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t p = 0; p < AMotionEvent_getPointerCount(e); p++) {
            int32_t id = AMotionEvent_getPointerId(e, p);
            for (int i = 0; i < MAX_POINTERS; i++) if (g_ptr[i].active && g_ptr[i].id == id) {
                g_ptr[i].x = AMotionEvent_getX(e, p) / (float)g_w; g_ptr[i].y = AMotionEvent_getY(e, p) / (float)g_h;
            }
        }
    }
}

static int handle_key(AInputEvent *e) {
    int32_t code = AKeyEvent_getKeyCode(e), down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
    u16 b = 0;
    switch (code) {
    case AKEYCODE_BUTTON_A: b = PAD_A; break;
    case AKEYCODE_BUTTON_B: b = PAD_B; break;
    case AKEYCODE_BUTTON_X: b = PAD_X; break;
    case AKEYCODE_BUTTON_Y: b = PAD_Y; break;
    case AKEYCODE_BUTTON_START: b = PAD_START; break;
    case AKEYCODE_BUTTON_L1: case AKEYCODE_BUTTON_L2: b = PAD_L; break;
    case AKEYCODE_BUTTON_R1: b = PAD_Z; break;
    case AKEYCODE_BUTTON_R2: b = PAD_R; break;
    case AKEYCODE_DPAD_UP: b = PAD_UP; break;
    case AKEYCODE_DPAD_DOWN: b = PAD_DOWN; break;
    case AKEYCODE_DPAD_LEFT: b = PAD_LEFT; break;
    case AKEYCODE_DPAD_RIGHT: b = PAD_RIGHT; break;
    case AKEYCODE_BACK: return 1;  /* swallow: don't close the game by accident */
    default: return 0;
    }
    if (down) g_gamepad_buttons |= b; else g_gamepad_buttons &= ~b;
    g_show_touch = 0;
    return 1;
}

static ALooper *g_looper;
static AInputQueue *g_attached;
static void pump_input(void) {
    pthread_mutex_lock(&g_mu);
    AInputQueue *q = g_queue;
    int changed = g_queue_changed;
    g_queue_changed = 0;
    pthread_mutex_unlock(&g_mu);
    if (changed) {
        if (g_attached) AInputQueue_detachLooper(g_attached);
        g_attached = q;
        if (q) AInputQueue_attachLooper(q, g_looper, 1, NULL, NULL);
    }
    if (!g_attached) return;
    AInputEvent *e;
    while (AInputQueue_hasEvents(g_attached) > 0 && AInputQueue_getEvent(g_attached, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(g_attached, e)) continue;
        int handled = 0;
        if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) { handle_motion(e); handled = 1; }
        else if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) handled = handle_key(e);
        AInputQueue_finishEvent(g_attached, e, handled);
    }
}

static s8 axis(float v) { v = fmaxf(-1, fminf(1, v)); return (s8)(v * 100.0f); }

void plat_input_update(unsigned long long frame) {
    (void)frame;
    pump_input();
    PadState *p = &g_pad[0];
    p->connected = 1;
    p->buttons = g_gamepad_buttons;
    p->stick_x = axis(g_gp_lx); p->stick_y = axis(-g_gp_ly);
    p->cstick_x = axis(g_gp_rx); p->cstick_y = axis(-g_gp_ry);
    p->trigger_l = (u8)(fminf(1, g_gp_lt) * 255); p->trigger_r = (u8)(fminf(1, g_gp_rt) * 255);
    float ax = (float)g_h / (float)(g_w ? g_w : 1);
    for (int i = 0; i < MAX_POINTERS; i++) {
        if (!g_ptr[i].active) continue;
        if (g_ptr[i].target == -2) {
            float dx = (g_ptr[i].x - k_stick_x) / ax / k_stick_r, dy = (g_ptr[i].y - k_stick_y) / k_stick_r;
            float l = sqrtf(dx * dx + dy * dy);
            if (l > 1) { dx /= l; dy /= l; }
            p->stick_x = axis(dx); p->stick_y = axis(-dy);
        } else if (g_ptr[i].target >= 0) {
            p->buttons |= k_buttons[g_ptr[i].target].button;
            if (k_buttons[g_ptr[i].target].button == PAD_L) p->trigger_l = 255;
            if (k_buttons[g_ptr[i].target].button == PAD_R) p->trigger_r = 255;
        }
    }
}

/* ------------------------------------------------------------ presenting */
static void draw_touch_controls(void) {
    if (!g_show_touch) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog_ui);
    GLint col = glGetUniformLocation(g_prog_ui, "u_color");
    float ax = (float)g_h / (float)g_w;
    /* stick base + knob */
    glUniform4f(col, 1, 1, 1, 0.5f);
    float r = k_stick_r;
    draw_rect(g_prog_ui, (k_stick_x - r * ax) * 2 - 1, 1 - (k_stick_y + r) * 2, r * ax * 4, r * 4);
    float kx = k_stick_x + g_pad[0].stick_x / 100.0f * r * ax, ky = k_stick_y - g_pad[0].stick_y / 100.0f * r;
    glUseProgram(g_prog_ui);
    glUniform4f(col, 1, 1, 1, 0.8f);
    draw_rect(g_prog_ui, (kx - r * 0.45f * ax) * 2 - 1, 1 - (ky + r * 0.45f) * 2, r * 0.9f * ax * 2, r * 0.9f * 2);
    for (int i = 0; i < NBUTTONS; i++) {
        const TouchButton *b = &k_buttons[i];
        int pressed = (g_pad[0].buttons & b->button) != 0;
        glUseProgram(g_prog_ui);
        glUniform4f(col, b->cr, b->cg, b->cb, pressed ? 0.95f : 0.55f);
        draw_rect(g_prog_ui, (b->x - b->r * ax) * 2 - 1, 1 - (b->y + b->r) * 2, b->r * ax * 4, b->r * 4);
    }
    glDisable(GL_BLEND);
}

/* Draw a texture region (uv origin + size) fitted 4:3 on the window, then the touch overlay. */
static void present_texture(GLuint tex, float u0, float v0, float du, float dv) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    float sw = (float)g_w, sh = (float)g_h, gw = sh * 4.0f / 3.0f, gh = sh;
    if (gw > sw) { gw = sw; gh = sw * 3.0f / 4.0f; }
    float nx = gw / sw, ny = gh / sh;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUseProgram(g_prog_tex);
    glUniform1i(glGetUniformLocation(g_prog_tex, "u_tex"), 0);
    glUniform4f(glGetUniformLocation(g_prog_tex, "u_color"), u0, v0, du, dv);
    draw_rect(g_prog_tex, -nx, -ny, nx * 2, ny * 2);
    draw_touch_controls();
    eglSwapBuffers(g_dpy, g_surf);
}

void plat_present_frame(const u8 *rgba, int stride, int x, int y, int w, int h) {
    if (!gl_ensure_surface()) return;
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    /* upload row by row (EFB stride may differ from the copied width) */
    for (int row = 0; row < h; row++)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba + ((size_t)(y + row) * stride + x) * 4);
    /* screen bottom (v_uv.y = 0) shows row h-1, top shows row 0 */
    present_texture(g_tex, 0, h / 1024.0f, w / 1024.0f, -h / 1024.0f);
}

/* GLES backend: the EFB texture is bottom-up (GL convention); region given top-down. */
void plat_present_gl(unsigned tex, int tex_w, int tex_h, int x, int y, int w, int h) {
    if (!gl_ensure_surface()) return;
    present_texture(tex, (float)x / tex_w, 1.0f - (float)(y + h) / tex_h, (float)w / tex_w, (float)h / tex_h);
}

/* ------------------------------------------------------------ game thread */
static int has_suffix(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}
static ANativeActivity *g_activity;

/* "All files access" (MANAGE_EXTERNAL_STORAGE). Without it Android 11+ hides other
 * apps' files (like the disc image) from directory listings. Returns 1 if granted
 * or not applicable, 0 if not granted, -1 if unknown. */
static JNIEnv *jni_env(void) {
    JNIEnv *env = NULL;
    JavaVM *vm = g_activity->vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        (*vm)->AttachCurrentThread(vm, &env, NULL);
    return env;
}
static int jni_clear(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return 1; }
    return 0;
}
static int storage_granted(void) {
    JNIEnv *env = jni_env();
    if (!env) return -1;
    jclass ec = (*env)->FindClass(env, "android/os/Environment");
    if (jni_clear(env) || !ec) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, ec, "isExternalStorageManager", "()Z");
    if (jni_clear(env) || !m) return 1;              /* Android 10 or older: legacy storage */
    jboolean r = (*env)->CallStaticBooleanMethod(env, ec, m);
    if (jni_clear(env)) return -1;
    return r ? 1 : 0;
}
static void open_storage_settings(void) {
    JNIEnv *env = jni_env();
    if (!env) return;
    jobject act = g_activity->clazz;
    jclass ac = (*env)->GetObjectClass(env, act);
    jclass ic = (*env)->FindClass(env, "android/content/Intent");
    jclass uc = (*env)->FindClass(env, "android/net/Uri");
    if (jni_clear(env) || !ic || !uc) return;
    jstring pkg = (jstring)(*env)->CallObjectMethod(env, act,
        (*env)->GetMethodID(env, ac, "getPackageName", "()Ljava/lang/String;"));
    if (jni_clear(env) || !pkg) return;
    const char *p = (*env)->GetStringUTFChars(env, pkg, NULL);
    char uri[256];
    snprintf(uri, sizeof uri, "package:%s", p);
    (*env)->ReleaseStringUTFChars(env, pkg, p);
    jobject u = (*env)->CallStaticObjectMethod(env, uc,
        (*env)->GetStaticMethodID(env, uc, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
        (*env)->NewStringUTF(env, uri));
    jmethodID start = (*env)->GetMethodID(env, ac, "startActivity", "(Landroid/content/Intent;)V");
    jobject in = (*env)->NewObject(env, ic,
        (*env)->GetMethodID(env, ic, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
        (*env)->NewStringUTF(env, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), u);
    if (!jni_clear(env) && in) {
        (*env)->CallVoidMethod(env, act, start, in);
        if (!jni_clear(env)) return;
    }
    /* fallback: the general list of apps with all-files access */
    in = (*env)->NewObject(env, ic, (*env)->GetMethodID(env, ic, "<init>", "(Ljava/lang/String;)V"),
        (*env)->NewStringUTF(env, "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION"));
    if (!jni_clear(env) && in) { (*env)->CallVoidMethod(env, act, start, in); jni_clear(env); }
}

static int g_quiet_find;
static int find_disc(char *out, size_t n) {
    static const char *dirs[] = {"/storage/emulated/0/Download", "/sdcard/Download", "/storage/emulated/0", NULL};
    for (int i = 0; dirs[i]; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) { if (!g_quiet_find) fprintf(stderr, "[android] cannot open %s\n", dirs[i]); continue; }
        struct dirent *e;
        int count = 0;
        while ((e = readdir(d))) {
            if (!g_quiet_find && count++ < 40) fprintf(stderr, "[android]   %s/%s\n", dirs[i], e->d_name);
            if (has_suffix(e->d_name, ".ciso") || has_suffix(e->d_name, ".iso") || has_suffix(e->d_name, ".gcm")) {
                snprintf(out, n, "%s/%s", dirs[i], e->d_name);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    return 0;
}

static void show_error_screen(float r, float g, float b) {
    for (;;) {
        if (!gl_ensure_surface()) return;
        glViewport(0, 0, g_w, g_h);
        glClearColor(r, g, b, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(g_dpy, g_surf);
        usleep(200000);
        if (g_stop) return;
    }
}

static void *game_thread(void *arg) {
    (void)arg;
    g_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    gl_init_context();
    if (!gl_ensure_surface()) return NULL;
    glViewport(0, 0, g_w, g_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(g_dpy, g_surf);

    {   /* log file: unbuffered, append-mode fd on stderr so nothing is lost or overwritten */
        int fd = open("/storage/emulated/0/Download/eed_log.txt", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 2); close(fd); }
        else LOGI("cannot write log to Download/ (grant All files access)");
        setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "[android] eedport log, pid %d\n", (int)getpid());
    }
    int g = storage_granted();
    fprintf(stderr, "[android] all-files access: %s\n", g == 1 ? "granted" : g == 0 ? "NOT granted" : "unknown");
    if (g == 0) {
        /* open the settings page and wait (yellow screen) until the switch is turned on */
        open_storage_settings();
        while (!g_stop && storage_granted() == 0) {
            if (gl_ensure_surface()) {
                glViewport(0, 0, g_w, g_h);
                glClearColor(0.5f, 0.45f, 0.0f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                eglSwapBuffers(g_dpy, g_surf);
            }
            usleep(500000);
        }
        if (g_stop) return NULL;
        fprintf(stderr, "[android] all-files access granted\n");
    }
    char disc[1024];
    if (!find_disc(disc, sizeof disc)) {
        /* Without "All files access", Android hides other apps' files (like the disc
         * image) from this app. Open the settings page and keep looking (dark red)
         * until the disc shows up, then start without needing a restart. */
        LOGI("no disc image found in Download/");
        fprintf(stderr, "No .ciso/.iso visible in Download/. Copy your dump there and turn on All files access.\n");
        open_storage_settings();
        for (int tries = 0; !find_disc(disc, sizeof disc); tries++) {
            if (gl_ensure_surface()) {
                glViewport(0, 0, g_w, g_h);
                glClearColor(0.5f, 0.0f, 0.0f, 1);   /* dark red: disc not found / no permission */
                glClear(GL_COLOR_BUFFER_BIT);
                eglSwapBuffers(g_dpy, g_surf);
            }
            for (int k = 0; k < 10 && !g_stop; k++) usleep(100000);
            if (g_stop) return NULL;
            g_quiet_find = 1;
        }
        g_quiet_find = 0;
    }
    /* GPU renderer (EED_SOFTWARE=1 in the environment would fall back, for debugging) */
    {
        extern int gx_use_gles, gx_gles_scale, di_trace, disc_trace, irq_trace;
        gx_use_gles = getenv("EED_SOFTWARE") == NULL;
        gx_gles_scale = 2;
        di_trace = disc_trace = irq_trace = 0;
        fprintf(stderr, "[android] renderer: %s\n", gx_use_gles ? "GPU (GLES)" : "software");
    }
    char card[1024];
    snprintf(card, sizeof card, "/storage/emulated/0/Download/eed_memcard");
    setenv("EED_MEMCARD", card, 1);
    fprintf(stderr, "[android] disc %s\n", disc);
    if (!eed_core_init(eed_main_dol, eed_main_dol_size, disc)) {
        show_error_screen(0.5f, 0.3f, 0.0f);   /* orange: disc unreadable */
        return NULL;
    }
    int r = eed_core_run(&g_stop);
    fprintf(stderr, "[android] core stopped (%d)\n", r);
    eed_core_debug_dump();
    if (r) show_error_screen(0.0f, 0.0f, 0.5f);  /* blue: game stopped with an error (see log) */
    return NULL;
}

/* ------------------------------------------------------------ NativeActivity callbacks */
static void on_window_created(ANativeActivity *a, ANativeWindow *w) {
    (void)a;
    pthread_mutex_lock(&g_mu); g_window = w; g_window_changed = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) {
    (void)a; (void)w;
    pthread_mutex_lock(&g_mu); g_window = NULL; g_window_changed = 1; pthread_mutex_unlock(&g_mu);
    /* The surface must not be used after this returns; the game thread only draws
     * while g_window is set, re-checked at the start of every frame. */
    usleep(50000);
}
static void on_queue_created(ANativeActivity *a, AInputQueue *q) {
    (void)a;
    pthread_mutex_lock(&g_mu); g_queue = q; g_queue_changed = 1; pthread_mutex_unlock(&g_mu);
}
static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q) {
    (void)a; (void)q;
    pthread_mutex_lock(&g_mu); g_queue = NULL; g_queue_changed = 1; pthread_mutex_unlock(&g_mu);
    usleep(50000);
}
static void on_destroy(ANativeActivity *a) {
    (void)a;
    g_stop = 1;
    pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *state, size_t size) {
    (void)state; (void)size;
    g_activity = a;
    a->callbacks->onNativeWindowCreated = on_window_created;
    a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onInputQueueCreated = on_queue_created;
    a->callbacks->onInputQueueDestroyed = on_queue_destroyed;
    a->callbacks->onDestroy = on_destroy;
    ANativeActivity_setWindowFlags(a, 0x00000080 /* KEEP_SCREEN_ON */ | 0x00000400 /* FULLSCREEN */, 0);
    static int started;
    if (!started) {
        started = 1;
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 64u << 20);   /* recompiled code recurses deeply */
        pthread_create(&t, &at, game_thread, NULL);
    }
}
