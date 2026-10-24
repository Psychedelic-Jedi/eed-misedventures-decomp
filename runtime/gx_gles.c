/* GX raster backend on OpenGL ES 2/3.
 *
 * gx_render.c still decodes and transforms vertices (with lighting) on the CPU;
 * everything per pixel runs on the GPU:
 *   - the EFB is an offscreen framebuffer (color texture + depth), optionally
 *     rendered at gx_gles_scale times the native 640x528
 *   - TEV combiner stages, texture lookups and alpha test become a generated
 *     fragment shader (cached per TEV configuration)
 *   - GX blend / depth / cull / scissor / masks map to GL state
 *   - textures are decoded once and cached by address, format, size and a hash of
 *     their bytes (and palette), re-validated at most once per frame
 * Triangles with identical state are batched into one draw call.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_api.h"
#include "gx_render.h"
#include "alias.h"
#include "platform.h"

int gx_gles_scale = 1;

static int g_ready, g_es3;
static GLuint g_fbo, g_color, g_depth, g_vbo;
static int g_fw, g_fh;                       /* framebuffer size (EFB * scale) */
static unsigned long long g_frame_no;

/* ------------------------------------------------------------------ guest memory */
static u8 rd8(u32 a) { u32 av; const u8 *p = page_fast(a | 0x80000000u, &av); return p ? *p : mem_read8(gx_cpu, a | 0x80000000u); }

/* 64-bit hash of guest bytes [addr, addr+len): direct page pointers where possible. */
static u64 hash_guest(u32 addr, u32 len) {
    u64 h = 0x9E3779B97F4A7C15ull ^ len;
    addr |= 0x80000000u;
    u32 done = 0;
    while (done < len) {
        u32 av;
        const u8 *p = page_fast(addr + done, &av);
        u32 page_left = 0x1000 - ((addr + done) & 0xFFF), n = len - done < page_left ? len - done : page_left;
        if (p && av >= n) {
            u32 i = 0;
            for (; i + 8 <= n; i += 8) { u64 w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
            for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
        } else {
            for (u32 i = 0; i < n; i++) h = (h ^ rd8(addr + done + i)) * 0x100000001B3ull;
        }
        done += n;
    }
    return h;
}
static u64 hash_bytes(const u8 *p, u32 n) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* ------------------------------------------------------------------ shaders */
static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[gles] shader compile failed: %s\n%s\n", log, src);
    }
    return s;
}

/* Attribute slots */
enum { A_POS = 0, A_C0 = 1, A_C1 = 2, A_T0 = 3 };   /* A_T0 + texcoord index (up to 8) */
#define VTX_FLOATS 36                                  /* pos4 c0 4 c1 4 tc 8x3 */

typedef struct {
    u32 nstages;
    u32 order[8];          /* BP 0x28-0x2F (masked to used stages) */
    u32 cenv[16], aenv[16];
    u32 ksel[8];
    u32 alpha_funcs;       /* BP 0xF3 >> 16 */
} ShaderKey;

typedef struct {
    ShaderKey key;
    GLuint prog;
    GLint u_reg, u_konst, u_aref, u_s[8];
    u32 tc_used;           /* bitmask of texcoords read */
    int used;
} Shader;

#define MAX_SHADERS 512
static Shader g_shaders[MAX_SHADERS];
static int g_nshaders;

static void stage_order(const ShaderKey *k, int s, int *map, int *coord, int *enable, int *chan) {
    u32 o = k->order[s / 2] >> ((s & 1) * 12);
    *map = o & 7; *coord = (o >> 3) & 7; *enable = (o >> 6) & 1; *chan = (o >> 7) & 7;
}

static const char *cin(u32 sel) {
    static const char *t[16] = {"r0.rgb", "vec3(r0.a)", "r1.rgb", "vec3(r1.a)", "r2.rgb", "vec3(r2.a)", "r3.rgb", "vec3(r3.a)",
                                "tex.rgb", "vec3(tex.a)", "ras.rgb", "vec3(ras.a)", "vec3(1.0)", "vec3(0.5)", "kc.rgb", "vec3(0.0)"};
    return t[sel & 15];
}
static const char *ain(u32 sel) {
    static const char *t[8] = {"r0.a", "r1.a", "r2.a", "r3.a", "tex.a", "ras.a", "kc.a", "0.0"};
    return t[sel & 7];
}

#define CAT(...) (n += snprintf(fs + n, sizeof fs - n, __VA_ARGS__))

static Shader *build_shader(const ShaderKey *k) {
    static char fs[32768], vs[4096];
    int n = 0;
    u32 tc_used = 0, map_used = 0;
    for (u32 s = 0; s < k->nstages; s++) {
        int map, coord, en, chan;
        stage_order(k, (int)s, &map, &coord, &en, &chan);
        if (en) { tc_used |= 1u << coord; map_used |= 1u << map; }
    }
    CAT("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
    CAT("varying vec4 v_c0, v_c1;\n");
    for (int i = 0; i < 8; i++) if (tc_used >> i & 1) CAT("varying vec3 v_t%d;\n", i);
    for (int i = 0; i < 8; i++) if (map_used >> i & 1) CAT("uniform sampler2D u_s%d;\n", i);
    CAT("uniform vec4 u_reg[4];\nuniform vec4 u_konst[4];\nuniform vec2 u_aref;\n");
    CAT("vec2 st(vec3 t) { return t.xy / (t.z == 0.0 ? 1.0 : t.z); }\n");
    CAT("void main() {\n  vec4 r0 = u_reg[0], r1 = u_reg[1], r2 = u_reg[2], r3 = u_reg[3];\n  vec4 tex, ras, kc;\n");
    CAT("  vec3 ca, cb, cc, cd; float aa, ab, ac, ad;\n");
    static const char *fr[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    static const char *comp = "rgba";
    static const char *bias[4] = {"0.0", "0.5", "-0.5", "0.0"}, *scale[4] = {"1.0", "2.0", "4.0", "0.5"};
    u32 last_cd = 0, last_ad = 0;
    for (u32 s = 0; s < k->nstages; s++) {
        int map, coord, en, chan;
        stage_order(k, (int)s, &map, &coord, &en, &chan);
        u32 cenv = k->cenv[s], aenv = k->aenv[s];
        CAT("  // stage %u\n", s);
        if (en) CAT("  tex = texture2D(u_s%d, st(v_t%d));\n", map, coord);
        else CAT("  tex = vec4(1.0);\n");
        if (chan == 7) CAT("  ras = vec4(0.0);\n");
        else CAT("  ras = %s;\n", (chan == 1 || chan == 3 || chan == 5) ? "v_c1" : "v_c0");
        /* konst selection */
        u32 ksel = k->ksel[s / 2];
        u32 kcs = (s & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31;
        u32 kas = (s & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31;
        char kcrgb[64], kca[64];
        if (kcs < 8) snprintf(kcrgb, sizeof kcrgb, "vec3(%s)", fr[kcs]);
        else if (kcs >= 12 && kcs < 16) snprintf(kcrgb, sizeof kcrgb, "u_konst[%u].rgb", kcs - 12);
        else if (kcs >= 16) snprintf(kcrgb, sizeof kcrgb, "vec3(u_konst[%u].%c)", (kcs - 16) & 3, comp[(kcs - 16) >> 2]);
        else snprintf(kcrgb, sizeof kcrgb, "vec3(1.0)");
        if (kas < 8) snprintf(kca, sizeof kca, "%s", fr[kas]);
        else if (kas >= 16) snprintf(kca, sizeof kca, "u_konst[%u].%c", (kas - 16) & 3, comp[(kas - 16) >> 2]);
        else snprintf(kca, sizeof kca, "1.0");
        CAT("  kc = vec4(%s, %s);\n", kcrgb, kca);
        /* gather all inputs before writing (a stage may read and write the same register) */
        CAT("  ca = clamp(%s, 0.0, 1.0); cb = clamp(%s, 0.0, 1.0); cc = clamp(%s, 0.0, 1.0); cd = %s;\n",
            cin(cenv >> 12), cin(cenv >> 8), cin(cenv >> 4), cin(cenv));
        CAT("  aa = clamp(%s, 0.0, 1.0); ab = clamp(%s, 0.0, 1.0); ac = clamp(%s, 0.0, 1.0); ad = %s;\n",
            ain(aenv >> 13), ain(aenv >> 10), ain(aenv >> 7), ain(aenv >> 4));
        u32 cdst = (cenv >> 22) & 3, adst = (aenv >> 22) & 3;
        CAT("  r%u.rgb = %s((cd %c mix(ca, cb, cc) + %s) * %s, %s);\n", cdst,
            (cenv >> 19) & 1 ? "clamp" : "clamp", (cenv >> 18) & 1 ? '-' : '+', bias[(cenv >> 16) & 3], scale[(cenv >> 20) & 3],
            (cenv >> 19) & 1 ? "0.0, 1.0" : "-1024.0 / 255.0, 1023.0 / 255.0");
        CAT("  r%u.a = clamp((ad %c mix(aa, ab, ac) + %s) * %s, %s);\n", adst,
            (aenv >> 18) & 1 ? '-' : '+', bias[(aenv >> 16) & 3], scale[(aenv >> 20) & 3],
            (aenv >> 19) & 1 ? "0.0, 1.0" : "-1024.0 / 255.0, 1023.0 / 255.0");
        last_cd = cdst; last_ad = adst;
    }
    CAT("  vec4 o = clamp(vec4(r%u.rgb, r%u.a), 0.0, 1.0);\n", last_cd, last_ad);
    /* alpha test: compare the 8-bit alpha against two references */
    u32 af = k->alpha_funcs;
    const char *cmp[8] = {"false", "(a < u_aref.x)", "(a == u_aref.x)", "(a <= u_aref.x)", "(a > u_aref.x)", "(a != u_aref.x)", "(a >= u_aref.x)", "true"};
    const char *cmp1[8] = {"false", "(a < u_aref.y)", "(a == u_aref.y)", "(a <= u_aref.y)", "(a > u_aref.y)", "(a != u_aref.y)", "(a >= u_aref.y)", "true"};
    const char *op[4] = {"&&", "||", "!=", "=="};
    u32 f0 = af & 7, f1 = (af >> 3) & 7, lop = (af >> 6) & 3;
    int always = (lop == 0 && f0 == 7 && f1 == 7) || (lop == 1 && (f0 == 7 || f1 == 7));
    if (!always) {
        CAT("  float a = floor(o.a * 255.0 + 0.5);\n");
        CAT("  if (!(%s %s %s)) discard;\n", cmp[f0], op[lop], cmp1[f1]);
    }
    CAT("  gl_FragColor = o;\n}\n");

    int m = snprintf(vs, sizeof vs, "attribute vec4 a_pos, a_c0, a_c1;\nvarying vec4 v_c0, v_c1;\n");
    for (int i = 0; i < 8; i++) if (tc_used >> i & 1) m += snprintf(vs + m, sizeof vs - m, "attribute vec3 a_t%d;\nvarying vec3 v_t%d;\n", i, i);
    m += snprintf(vs + m, sizeof vs - m, "void main() {\n  gl_Position = a_pos; v_c0 = a_c0; v_c1 = a_c1;\n");
    for (int i = 0; i < 8; i++) if (tc_used >> i & 1) m += snprintf(vs + m, sizeof vs - m, "  v_t%d = a_t%d;\n", i, i);
    snprintf(vs + m, sizeof vs - m, "}\n");

    if (g_nshaders == MAX_SHADERS) {   /* very unlikely: drop the least used half */
        g_nshaders = 0;
    }
    Shader *sh = &g_shaders[g_nshaders++];
    memset(sh, 0, sizeof *sh);
    sh->key = *k;
    sh->tc_used = tc_used;
    sh->prog = glCreateProgram();
    glAttachShader(sh->prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(sh->prog, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(sh->prog, A_POS, "a_pos");
    glBindAttribLocation(sh->prog, A_C0, "a_c0");
    glBindAttribLocation(sh->prog, A_C1, "a_c1");
    for (int i = 0; i < 8; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "a_t%d", i);
        glBindAttribLocation(sh->prog, A_T0 + i, nm);
    }
    glLinkProgram(sh->prog);
    GLint ok = 0;
    glGetProgramiv(sh->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(sh->prog, sizeof log, NULL, log);
        fprintf(stderr, "[gles] link failed: %s\n", log);
    }
    glUseProgram(sh->prog);
    sh->u_reg = glGetUniformLocation(sh->prog, "u_reg");
    sh->u_konst = glGetUniformLocation(sh->prog, "u_konst");
    sh->u_aref = glGetUniformLocation(sh->prog, "u_aref");
    for (int i = 0; i < 8; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "u_s%d", i);
        sh->u_s[i] = glGetUniformLocation(sh->prog, nm);
        if (sh->u_s[i] >= 0) glUniform1i(sh->u_s[i], i);
    }
    return sh;
}

static Shader *get_shader(const ShaderKey *k) {
    for (int i = g_nshaders - 1; i >= 0; i--)
        if (!memcmp(&g_shaders[i].key, k, sizeof *k)) return &g_shaders[i];
    return build_shader(k);
}

/* ------------------------------------------------------------------ textures */
typedef struct {
    u32 addr, w, h, fmt, tlut;
    u64 hash;
    GLuint tex;
    unsigned long long checked, used;
    int wrap_s, wrap_t, filt_min, filt_mag;
    int next;              /* hash chain */
    int live;
} GTex;

#define TEX_MAX 2048
#define TEX_BUCKETS 1024
static GTex g_texs[TEX_MAX];
static int g_bucket[TEX_BUCKETS];
static int g_tex_count;

static u32 tex_bytes(u32 fmt, u32 w, u32 h) {
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    int BW = bw[fmt & 15], BH = bh[fmt & 15];
    if (!BW) return 0;
    u32 blocks = ((w + BW - 1) / BW) * ((h + BH - 1) / BH);
    return blocks * (fmt == 6 ? 64 : 32);
}

static u64 tlut_hash(u32 fmt, u32 tl) {
    u32 entries = fmt == 8 ? 16 : fmt == 9 ? 256 : fmt == 10 ? 16384 : 0;
    if (!entries) return 0;
    u32 off = (tl & 0x3FF) << 9;
    u32 len = entries * 2;
    if (off + len > sizeof tmem) len = off < sizeof tmem ? (u32)sizeof tmem - off : 0;
    return hash_bytes(tmem + off, len) ^ ((u64)tl << 48);
}

static void tex_evict_old(void) {
    /* drop textures unused for a while (keeps GPU memory bounded) */
    for (int b = 0; b < TEX_BUCKETS; b++) g_bucket[b] = -1;
    int keep = 0;
    for (int i = 0; i < g_tex_count; i++) {
        GTex *t = &g_texs[i];
        if (t->live && g_frame_no - t->used < 300) g_texs[keep++] = *t;
        else if (t->live) glDeleteTextures(1, &t->tex);
    }
    g_tex_count = keep;
    for (int i = 0; i < g_tex_count; i++) {
        u32 b = (g_texs[i].addr >> 5 ^ g_texs[i].w * 31 ^ g_texs[i].h) & (TEX_BUCKETS - 1);
        g_texs[i].next = g_bucket[b];
        g_bucket[b] = i;
    }
}

static void flush(void);
static int g_cur_valid;
static GTex *lookup_texture(int map) {
    static const u8 img0[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
    static const u8 img3[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
    static const u8 tlut[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
    u32 i0 = gx.bp[img0[map]], addr = (gx.bp[img3[map]] & 0xFFFFFF) << 5, tl = gx.bp[tlut[map]];
    u32 w = (i0 & 0x3FF) + 1, h = ((i0 >> 10) & 0x3FF) + 1, fmt = (i0 >> 20) & 15;
    int paletted = fmt == 8 || fmt == 9 || fmt == 10;
    if (!paletted) tl = 0;
    u32 b = (addr >> 5 ^ w * 31 ^ h) & (TEX_BUCKETS - 1);
    GTex *t = NULL;
    for (int i = g_bucket[b]; i >= 0; i = g_texs[i].next) {
        GTex *c = &g_texs[i];
        if (c->addr == addr && c->w == w && c->h == h && c->fmt == fmt && c->tlut == tl) { t = c; break; }
    }
    if (t && t->checked == g_frame_no) { t->used = g_frame_no; return t; }
    u64 hsh = hash_guest(addr, tex_bytes(fmt, w, h)) ^ (paletted ? tlut_hash(fmt, tl) : 0);
    if (t && t->hash == hsh) { t->checked = t->used = g_frame_no; return t; }
    /* the pending batch must be drawn before a texture object changes or goes away */
    flush();
    g_cur_valid = 0;
    if (!t) {
        if (g_tex_count == TEX_MAX) {
            tex_evict_old();
            if (g_tex_count == TEX_MAX) {              /* everything recent: start over */
                for (int i = 0; i < g_tex_count; i++) glDeleteTextures(1, &g_texs[i].tex);
                g_tex_count = 0;
                for (int k = 0; k < TEX_BUCKETS; k++) g_bucket[k] = -1;
            }
            b = (addr >> 5 ^ w * 31 ^ h) & (TEX_BUCKETS - 1);
        }
        t = &g_texs[g_tex_count];
        memset(t, 0, sizeof *t);
        t->addr = addr; t->w = w; t->h = h; t->fmt = fmt; t->tlut = tl;
        t->next = g_bucket[b];
        g_bucket[b] = g_tex_count++;
        t->live = 1;
        glGenTextures(1, &t->tex);
        t->wrap_s = t->wrap_t = t->filt_min = t->filt_mag = -1;
    }
    u8 *rgba = decode_texture(addr, (int)w, (int)h, fmt, (tl & 0x3FF) << 9, (tl >> 10) & 3);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    t->hash = hsh;
    t->checked = t->used = g_frame_no;
    return t;
}

static int pot(u32 v) { return v && !(v & (v - 1)); }

static void apply_sampler(GTex *t, int map) {
    static const u8 mode0[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
    u32 m = gx.bp[mode0[map]];
    static const int wraps[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    int ws = wraps[m & 3], wt = wraps[(m >> 2) & 3];
    if (!g_es3 && !(pot(t->w) && pot(t->h))) ws = wt = GL_CLAMP_TO_EDGE;   /* GLES2: NPOT can't repeat */
    int mag = (m >> 4) & 1 ? GL_LINEAR : GL_NEAREST;
    u32 mn = (m >> 5) & 7;
    int min = (mn >= 4) ? GL_LINEAR : GL_NEAREST;
    if (ws != t->wrap_s) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws); t->wrap_s = ws; }
    if (wt != t->wrap_t) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt); t->wrap_t = wt; }
    if (min != t->filt_min) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min); t->filt_min = min; }
    if (mag != t->filt_mag) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag); t->filt_mag = mag; }
}

/* ------------------------------------------------------------------ draw state + batching */
typedef struct {
    ShaderKey sk;
    GLuint tex[8];                      /* GL texture per texmap (0 = unused) */
    u32 tex_mode[8];
    u32 genmode_cull, zmode, blend, scissor_tl, scissor_br;
    float reg[16], konst[16];
    u32 aref;
} DrawState;

static DrawState g_cur;                 /* state of the pending batch (valid if g_cur_valid) */
static Shader *g_cur_shader;
static float *g_vtx;
static int g_nvtx, g_vcap;

static void ensure_init(void) {
    if (g_ready) return;
    g_ready = 1;
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    g_es3 = ver && strstr(ver, "OpenGL ES 3") != NULL;
    fprintf(stderr, "[gles] %s / %s, EFB scale %d\n", ver ? ver : "?", ren ? ren : "?", gx_gles_scale);
    g_fw = EFB_W * gx_gles_scale;
    g_fh = EFB_H * gx_gles_scale;
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_fw, g_fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, g_es3 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16, g_fw, g_fh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[gles] EFB framebuffer incomplete: 0x%X\n", st);
    glGenBuffers(1, &g_vbo);
    for (int b = 0; b < TEX_BUCKETS; b++) g_bucket[b] = -1;
    glViewport(0, 0, g_fw, g_fh);
    glClearColor(0, 0, 0, 1);
    glClearDepthf(1.0f);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void bind_efb(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, g_fw, g_fh);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
}

static void flush(void) {
    if (!g_nvtx) return;
    Shader *sh = g_cur_shader;
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_nvtx * VTX_FLOATS * 4, g_vtx, GL_STREAM_DRAW);
    const GLsizei stride = VTX_FLOATS * 4;
    glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glVertexAttribPointer(A_C0, 4, GL_FLOAT, GL_FALSE, stride, (void *)16);
    glVertexAttribPointer(A_C1, 4, GL_FLOAT, GL_FALSE, stride, (void *)32);
    glEnableVertexAttribArray(A_POS);
    glEnableVertexAttribArray(A_C0);
    glEnableVertexAttribArray(A_C1);
    for (int i = 0; i < 8; i++) {
        if (sh->tc_used >> i & 1) {
            glVertexAttribPointer(A_T0 + i, 3, GL_FLOAT, GL_FALSE, stride, (void *)(intptr_t)(48 + i * 12));
            glEnableVertexAttribArray(A_T0 + i);
        } else glDisableVertexAttribArray(A_T0 + i);
    }
    glDrawArrays(GL_TRIANGLES, 0, g_nvtx);
    g_nvtx = 0;
}

static GLenum blend_src(u32 f) {
    static const GLenum t[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    return t[f & 7];
}
static GLenum blend_dst(u32 f) {
    static const GLenum t[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    return t[f & 7];
}

/* Make GL match `s` (only what differs from the current state unless !g_cur_valid). */
static void apply_state(const DrawState *s, Shader *sh) {
    int all = !g_cur_valid;
    const DrawState *c = &g_cur;
    if (all || sh != g_cur_shader) glUseProgram(sh->prog);
    glUniform4fv(sh->u_reg, 4, s->reg);
    glUniform4fv(sh->u_konst, 4, s->konst);
    glUniform2f(sh->u_aref, (float)(s->aref & 0xFF), (float)((s->aref >> 8) & 0xFF));
    for (int m = 0; m < 8; m++) {
        if (!s->tex[m]) continue;
        glActiveTexture(GL_TEXTURE0 + m);
        glBindTexture(GL_TEXTURE_2D, s->tex[m]);
    }
    glActiveTexture(GL_TEXTURE0);
    if (all || s->genmode_cull != c->genmode_cull) {
        u32 cull = s->genmode_cull;
        if (!cull) glDisable(GL_CULL_FACE);
        else {
            glEnable(GL_CULL_FACE);
            glCullFace(cull == 1 ? GL_BACK : cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK);
        }
        /* see raster_tri: with the EFB's y flipped into GL, GX front faces are GL clockwise */
        glFrontFace(g_cull_flip ? GL_CW : GL_CCW);
    }
    if (all || s->zmode != c->zmode) {
        u32 zm = s->zmode;
        if (zm & 1) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_NEVER + ((zm >> 1) & 7));
            glDepthMask((zm >> 4) & 1 ? GL_TRUE : GL_FALSE);
        } else {
            glDisable(GL_DEPTH_TEST);   /* GX: no test and no update */
            glDepthMask(GL_FALSE);
        }
    }
    if (all || s->blend != c->blend) {
        u32 bm = s->blend;
        if (bm & 1) {
            glEnable(GL_BLEND);
            glBlendFunc(blend_src((bm >> 8) & 7), blend_dst((bm >> 5) & 7));
            glBlendEquation((bm >> 11) & 1 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
        } else glDisable(GL_BLEND);
        glColorMask((bm >> 3) & 1, (bm >> 3) & 1, (bm >> 3) & 1, (bm >> 4) & 1);
    }
    if (all || s->scissor_tl != c->scissor_tl || s->scissor_br != c->scissor_br) {
        int scl = (int)(s->scissor_tl >> 12 & 0x7FF) - 342, sct = (int)(s->scissor_tl & 0x7FF) - 342;
        int scr = (int)(s->scissor_br >> 12 & 0x7FF) - 342, scb = (int)(s->scissor_br & 0x7FF) - 342;
        if (scr <= scl || scb <= sct) { scl = 0; sct = 0; scr = EFB_W - 1; scb = EFB_H - 1; }
        glEnable(GL_SCISSOR_TEST);
        int S = gx_gles_scale;
        glScissor(scl * S, (EFB_H - 1 - scb) * S, (scr - scl + 1) * S, (scb - sct + 1) * S);
    }
    g_cur = *s;
    g_cur_shader = sh;
    g_cur_valid = 1;
}

static void emit(const SVtx *v, float vpx, float vpy, float vpz, float ox, float oy, float fz) {
    if (g_nvtx == g_vcap) { g_vcap = g_vcap ? g_vcap * 2 : 4096; g_vtx = realloc(g_vtx, (size_t)g_vcap * VTX_FLOATS * 4); }
    float *o = g_vtx + (size_t)g_nvtx++ * VTX_FLOATS;
    /* window position sx = x/w*vpx + ox (and likewise y, z), expressed in clip space so
     * the GPU clips against the near plane instead of rejecting the triangle */
    float w = v->w;
    o[0] = (v->x * vpx + ox * w) * (2.0f / EFB_W) - w;
    o[1] = w - (v->y * vpy + oy * w) * (2.0f / EFB_H);
    o[2] = (v->z * vpz + fz * w) * (2.0f / 16777215.0f) - w;
    o[3] = w;
    memcpy(o + 4, v->col[0], 16);
    memcpy(o + 8, v->col[1], 16);
    for (int i = 0; i < 8; i++) memcpy(o + 12 + i * 3, v->tc[i], 12);
}

static float xff(u32 reg) { float f; memcpy(&f, &gx.xfreg[reg], 4); return f; }

void gles_draw(u8 prim, const SVtx *v, int count) {
    ensure_init();
    if (prim != 0x80 && prim != 0x88 && prim != 0x90 && prim != 0x98 && prim != 0xA0) return;  /* lines/points TODO */
    u32 gen = gx.bp[0x00];
    if (((gen >> 14) & 3) == 3) return;                     /* cull all */
    DrawState s;
    memset(&s, 0, sizeof s);
    s.sk.nstages = ((gen >> 10) & 15) + 1;
    for (u32 i = 0; i < (s.sk.nstages + 1) / 2; i++) {
        u32 o = gx.bp[0x28 + i];
        if (2 * i + 1 >= s.sk.nstages) o &= 0xFFF;           /* odd stage count: upper half unused */
        s.sk.order[i] = o & 0xFFFFFF;
    }
    for (u32 i = 0; i < s.sk.nstages; i++) {
        s.sk.cenv[i] = gx.bp[0xC0 + i * 2] & 0xFFFFFF;
        s.sk.aenv[i] = gx.bp[0xC1 + i * 2] & 0xFFFFFF;
    }
    for (u32 i = 0; i < (s.sk.nstages + 1) / 2; i++) s.sk.ksel[i] = gx.bp[0xF6 + i] & 0xFFFFF0;  /* swap tables unused */
    s.sk.alpha_funcs = (gx.bp[0xF3] >> 16) & 0xFF;
    s.aref = gx.bp[0xF3] & 0xFFFF;
    for (u32 st = 0; st < s.sk.nstages; st++) {
        u32 o = s.sk.order[st / 2] >> ((st & 1) * 12);
        if ((o >> 6) & 1) {
            int map = o & 7;
            if (!s.tex[map]) {
                GTex *t = lookup_texture(map);
                s.tex[map] = t->tex;
            }
        }
    }
    s.genmode_cull = (gen >> 14) & 3;
    s.zmode = gx.bp[0x40] & 0x1F;
    s.blend = gx.bp[0x41] & 0xFFFF;
    s.scissor_tl = gx.bp[0x20] & 0xFFFFFF;
    s.scissor_br = gx.bp[0x21] & 0xFFFFFF;
    memcpy(s.reg, tev_reg, sizeof s.reg);
    memcpy(s.konst, tev_konst, sizeof s.konst);

    if (!g_cur_valid || memcmp(&s, &g_cur, sizeof s)) {
        flush();
        bind_efb();
        Shader *sh = get_shader(&s.sk);
        /* sampler parameters live on the texture objects */
        for (u32 st = 0; st < s.sk.nstages; st++) {
            u32 o = s.sk.order[st / 2] >> ((st & 1) * 12);
            if ((o >> 6) & 1) {
                int map = o & 7;
                for (int i = 0; i < g_tex_count; i++)
                    if (g_texs[i].tex == s.tex[map]) {
                        glActiveTexture(GL_TEXTURE0 + map);
                        glBindTexture(GL_TEXTURE_2D, s.tex[map]);
                        apply_sampler(&g_texs[i], map);
                        break;
                    }
            }
        }
        apply_state(&s, sh);
    }

    float vpx = xff(0x1A), vpy = xff(0x1B), vpz = xff(0x1C), ox = xff(0x1D) - 342, oy = xff(0x1E) - 342, fz = xff(0x1F);
#define TRI(a, b, c) do { emit(&v[a], vpx, vpy, vpz, ox, oy, fz); emit(&v[b], vpx, vpy, vpz, ox, oy, fz); emit(&v[c], vpx, vpy, vpz, ox, oy, fz); } while (0)
    switch (prim) {
    case 0x80: case 0x88:
        for (int i = 0; i + 3 < count; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); }
        break;
    case 0x90:
        for (int i = 0; i + 2 < count; i += 3) TRI(i, i + 1, i + 2);
        break;
    case 0x98:
        for (int i = 0; i + 2 < count; i++) { if (i & 1) TRI(i + 1, i, i + 2); else TRI(i, i + 1, i + 2); }
        break;
    case 0xA0:
        for (int i = 1; i + 1 < count; i++) TRI(0, i, i + 1);
        break;
    }
#undef TRI
}

/* Leave GL in a neutral state for the front end (it draws to the window). */
static void release_state(void) {
    flush();
    for (int i = 0; i < 3 + 8; i++) glDisableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    g_cur_valid = 0;
}

void gles_bp(u8 reg, u32 v) {
    if (reg != 0x52) return;
    ensure_init();
    flush();
    int x0 = gx.bp[0x49] & 0x3FF, y0 = (gx.bp[0x49] >> 10) & 0x3FF;
    int w = (gx.bp[0x4A] & 0x3FF) + 1, h = ((gx.bp[0x4A] >> 10) & 0x3FF) + 1;
    if (x0 + w > EFB_W) w = EFB_W - x0;
    if (y0 + h > EFB_H) h = EFB_H - y0;
    const int S = gx_gles_scale;
    if ((v >> 14) & 1) {                                   /* copy to XFB: a finished frame */
        release_state();
        plat_present_gl(g_color, g_fw, g_fh, x0 * S, y0 * S, w * S, h * S);
        g_frame_no++;
        gx_frames++;
        if ((g_frame_no & 63) == 0) tex_evict_old();
    }
    /* copy to texture (render targets for effects): TODO */
    if ((v >> 11) & 1) {                                   /* clear the copied rectangle */
        bind_efb();
        u32 ar = gx.bp[0x4F], gb = gx.bp[0x50];
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0 * S, (EFB_H - y0 - h) * S, w * S, h * S);
        glColorMask(1, 1, 1, 1);
        glDepthMask(GL_TRUE);
        glClearColor((u8)ar / 255.f, (u8)(gb >> 8) / 255.f, (u8)gb / 255.f, (u8)(ar >> 8) / 255.f);
        glClearDepthf((gx.bp[0x51] & 0xFFFFFF) / 16777215.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        g_cur_valid = 0;                                   /* scissor/masks changed */
    }
}

void gles_read_efb(u8 *out, int x, int y, int w, int h) {
    ensure_init();
    flush();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    u8 *tmp = malloc((size_t)w * h * 4);
    glReadPixels(x, g_fh - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int r = 0; r < h; r++) memcpy(out + (size_t)r * w * 4, tmp + (size_t)(h - 1 - r) * w * 4, (size_t)w * 4);
    free(tmp);
}
