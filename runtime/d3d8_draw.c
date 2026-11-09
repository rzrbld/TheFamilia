/*
 * d3d8_draw.c - fixed-function D3D8 drawing on GLES 2.0 shaders.
 *
 * At each draw the relevant D3D state is reduced to a key; each distinct key
 * gets a generated GLSL ES 1.00 program (cached). Matrices go up as stored
 * by D3D (row-major, row vectors): read column-major by GL they are the
 * transpose, so `M * v` in GLSL equals D3D's `v * M`.
 *
 * Conventions bridged here:
 *  - D3D clip z is [0,w], GL's [-w,w]: z' = 2z - w (vertex shader).
 *  - D3D and GL clip space both put +y at the top of the screen, so
 *    transformed geometry needs no flip and a triangle that is clockwise on
 *    screen is clockwise for both: D3D's front (CW) is glFrontFace(GL_CW).
 *    Only pre-transformed screen pixels (y down) are converted to NDC.
 *  - Pre-transformed (XYZRHW) vertices are window pixels whose centres are
 *    at integer coordinates in D3D: +0.5 before converting to clip space.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "gl_funcs.h"
#include "d3d8_slots.h"
#include "d3d8_internal.h"
#include "familia.h"

/* attribute slots */
enum { A_POS, A_NORMAL, A_DIFFUSE, A_SPECULAR, A_WEIGHTS, A_TEX0, A_COUNT = A_TEX0 + 4 };

typedef struct {
    uint32_t fvf;           /* masked: position kind, normal, colors, tex count+sizes */
    uint8_t  rhw, lighting, nblend, specular_en, color_vertex;
    uint8_t  mat_src[4];    /* diffuse, specular, ambient, emissive sources */
    uint8_t  nlights, light_type[FAM_MAX_LIGHTS];
    uint8_t  fog;           /* 0 none, 1 vertex(specular alpha), 2 linear, 3 exp, 4 exp2 */
    uint8_t  fog_range;     /* range-based (vs. z) */
    uint8_t  alpha_test;    /* 0 off, else D3DCMP func */
    uint8_t  nstages;
    uint32_t stage[FAM_MAX_STAGES][7];   /* colorop, arg1, arg2, alphaop, arg1, arg2, tci */
    uint8_t  tex_kind[FAM_MAX_STAGES];   /* 0 rgb(a), 1 alpha-only, 2 luminance */
} draw_key;

typedef struct prog {
    draw_key key;
    GLuint prog;
    GLint u_mvp, u_wv[4], u_proj, u_vpscale, u_tfactor, u_ambient, u_mat[4], u_mat_power;
    GLint u_lpos[FAM_MAX_LIGHTS], u_ldir[FAM_MAX_LIGHTS], u_ldiff[FAM_MAX_LIGHTS], u_lspec[FAM_MAX_LIGHTS],
          u_lamb[FAM_MAX_LIGHTS], u_latt[FAM_MAX_LIGHTS], u_lspot[FAM_MAX_LIGHTS];
    GLint u_fog, u_fogcolor, u_aref;
    uint32_t g_vp, g_xf, g_light, g_lxf, g_lrs, g_rs;   /* state generations last uploaded */
    struct prog *next;
} prog;

#define PROG_BUCKETS 256
static prog *g_progs[PROG_BUCKETS];
static prog *g_cur;
/* State generations: prepare() folds the dirty bits of each draw into these;
 * a program re-uploads a uniform group only when its generation moved. */
static struct { uint32_t vp, xf, light, rs; } g_gen = { 1, 1, 1, 1 };
static int g_nprogs;
fam_draw_stats g_draw_stats;

/* ----------------------------------------------------------------- FVF */
static uint32_t fvf_tex_count(uint32_t fvf) { return (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT; }
static int fvf_tex_size(uint32_t fvf, int i) {     /* floats per set */
    static const int k[4] = { 2, 3, 4, 1 };
    return k[(fvf >> (16 + i * 2)) & 3];
}
static int fvf_pos_floats(uint32_t fvf) {
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: return 3; case D3DFVF_XYZRHW: return 4;
    case D3DFVF_XYZB1: return 4; case D3DFVF_XYZB2: return 5; case D3DFVF_XYZB3: return 6;
    case D3DFVF_XYZB4: return 7; case D3DFVF_XYZB5: return 8;
    }
    return 3;
}
static int fvf_blend_weights(uint32_t fvf) {
    uint32_t p = fvf & D3DFVF_POSITION_MASK;
    if (p >= D3DFVF_XYZB1 && p <= D3DFVF_XYZB5) return (int)((p - D3DFVF_XYZB1) / 2) + 1;
    return 0;
}
typedef struct { int pos, normal, psize, diffuse, specular, tex[8], stride; } fvf_layout;
static void fvf_decode(uint32_t fvf, fvf_layout *l) {
    int o = 0;
    memset(l, -1, sizeof *l);
    l->pos = 0; o = fvf_pos_floats(fvf) * 4;
    if (fvf & D3DFVF_NORMAL)   { l->normal = o; o += 12; }
    if (fvf & D3DFVF_PSIZE)    { l->psize = o; o += 4; }
    if (fvf & D3DFVF_DIFFUSE)  { l->diffuse = o; o += 4; }
    if (fvf & D3DFVF_SPECULAR) { l->specular = o; o += 4; }
    for (uint32_t i = 0; i < fvf_tex_count(fvf) && i < 8; i++) { l->tex[i] = o; o += fvf_tex_size(fvf, (int)i) * 4; }
    l->stride = o;
}

/* --------------------------------------------------------------- shaders */
typedef struct { char *s; size_t n, cap; } sbuf;
static void sb(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void sb(sbuf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < b->cap - b->n) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + 4096; b->s = realloc(b->s, b->cap);
    }
}

static const char *arg_src(uint32_t a, int alpha, int stage, char *tmp) {
    const char *base;
    switch (a & D3DTA_SELECTMASK) {
    case D3DTA_DIFFUSE: base = "v_diff"; break;
    case D3DTA_CURRENT: base = "cur"; break;
    case D3DTA_TEXTURE: sprintf(tmp + 64, "t%d", stage); base = tmp + 64; break;
    case D3DTA_TFACTOR: base = "u_tfactor"; break;
    case D3DTA_SPECULAR: base = "v_spec"; break;
    case D3DTA_TEMP: base = "temp"; break;
    default: base = "cur";
    }
    if (a & D3DTA_ALPHAREPLICATE) sprintf(tmp, "%s(%s.a)", alpha ? "" : "vec3", base);
    else sprintf(tmp, alpha ? "%s.a" : "%s.rgb", base);
    if (a & D3DTA_COMPLEMENT) { char t2[160]; sprintf(t2, "(1.0 - %s)", tmp); strcpy(tmp, t2); }
    return tmp;
}
static void stage_op(sbuf *b, uint32_t op, uint32_t a1, uint32_t a2, int alpha, int st) {
    char x[192], y[192];
    const char *A = arg_src(a1, alpha, st, x), *B = arg_src(a2, alpha, st, y);
    const char *dst = alpha ? "cur.a" : "cur.rgb", *T = alpha ? "float" : "vec3";
    char ta[16]; sprintf(ta, "t%d.a", st);
    switch (op) {
    case D3DTOP_SELECTARG1: sb(b, "  %s = %s;\n", dst, A); break;
    case D3DTOP_SELECTARG2: sb(b, "  %s = %s;\n", dst, B); break;
    case D3DTOP_MODULATE:   sb(b, "  %s = %s * %s;\n", dst, A, B); break;
    case D3DTOP_MODULATE2X: sb(b, "  %s = min(%s * %s * 2.0, %s(1.0));\n", dst, A, B, T); break;
    case D3DTOP_MODULATE4X: sb(b, "  %s = min(%s * %s * 4.0, %s(1.0));\n", dst, A, B, T); break;
    case D3DTOP_ADD:        sb(b, "  %s = min(%s + %s, %s(1.0));\n", dst, A, B, T); break;
    case D3DTOP_ADDSIGNED:  sb(b, "  %s = clamp(%s + %s - 0.5, 0.0, 1.0);\n", dst, A, B); break;
    case D3DTOP_ADDSIGNED2X:sb(b, "  %s = clamp((%s + %s - 0.5) * 2.0, 0.0, 1.0);\n", dst, A, B); break;
    case D3DTOP_SUBTRACT:   sb(b, "  %s = max(%s - %s, %s(0.0));\n", dst, A, B, T); break;
    case D3DTOP_ADDSMOOTH:  sb(b, "  %s = %s + %s * (1.0 - %s);\n", dst, A, B, A); break;
    case D3DTOP_BLENDDIFFUSEALPHA: sb(b, "  %s = mix(%s, %s, v_diff.a);\n", dst, B, A); break;
    case D3DTOP_BLENDTEXTUREALPHA: sb(b, "  %s = mix(%s, %s, %s);\n", dst, B, A, ta); break;
    case D3DTOP_BLENDFACTORALPHA:  sb(b, "  %s = mix(%s, %s, u_tfactor.a);\n", dst, B, A); break;
    case D3DTOP_BLENDTEXTUREALPHAPM: sb(b, "  %s = min(%s + %s * (1.0 - %s), %s(1.0));\n", dst, A, B, ta, T); break;
    case D3DTOP_BLENDCURRENTALPHA: sb(b, "  %s = mix(%s, %s, cur.a);\n", dst, B, A); break;
    case D3DTOP_MODULATEALPHA_ADDCOLOR: if (!alpha) { char z[192]; sb(b, "  cur.rgb = min(%s + %s * %s, vec3(1.0));\n", A, arg_src(a1 | D3DTA_ALPHAREPLICATE, 0, st, z), B); } else sb(b, "  cur.a = %s;\n", A); break;
    case D3DTOP_MODULATECOLOR_ADDALPHA: if (!alpha) sb(b, "  cur.rgb = %s * %s;\n", A, B); else sb(b, "  cur.a = min(%s + %s, 1.0);\n", A, B); break;
    case D3DTOP_DOTPRODUCT3: sb(b, "  { float d = clamp(4.0 * dot(%s - 0.5, %s - 0.5), 0.0, 1.0); %s = %s(d); }\n",
                                alpha ? "vec3(cur.a)" : A, alpha ? "vec3(cur.a)" : B, dst, T); break;
    case D3DTOP_LERP: sb(b, "  %s = %s;\n", dst, A); break;   /* needs ARG0; LS3D does not set it */
    default: sb(b, "  %s = %s * %s;\n", dst, A, B); break;
    }
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[gl] shader compile failed: %s\n--- source ---\n%s\n", log, src);
        glDeleteShader(s); return 0;
    }
    return s;
}

static prog *build_program(const draw_key *k) {
    sbuf v = {0}, f = {0};
    int ntex = (int)fvf_tex_count(k->fvf);
    int has_n = (k->fvf & D3DFVF_NORMAL) != 0, has_d = (k->fvf & D3DFVF_DIFFUSE) != 0, has_s = (k->fvf & D3DFVF_SPECULAR) != 0;
    /* ---- vertex shader ---- */
    sb(&v, "precision highp float;\nattribute vec4 a_pos;\n");
    if (has_n) sb(&v, "attribute vec3 a_normal;\n");
    if (has_d) sb(&v, "attribute vec4 a_diffuse;\n");
    if (has_s) sb(&v, "attribute vec4 a_specular;\n");
    if (k->nblend) sb(&v, "attribute vec4 a_weights;\n");
    for (int i = 0; i < ntex && i < 4; i++) sb(&v, "attribute vec4 a_tex%d;\n", i);
    sb(&v, "uniform mat4 u_mvp, u_proj;\nuniform mat4 u_wv[4];\nuniform vec4 u_vpscale;\n"
           "varying vec4 v_diff, v_spec;\nvarying float v_fog;\n");
    for (int i = 0; i < k->nstages; i++) sb(&v, "varying vec4 v_tc%d;\n", i);
    if (k->lighting) {
        sb(&v, "uniform vec4 u_ambient, u_mat[4];\nuniform float u_mat_power;\n");
        for (int i = 0; i < k->nlights; i++)
            sb(&v, "uniform vec3 u_lpos%d, u_ldir%d;\nuniform vec4 u_ldiff%d, u_lspec%d, u_lamb%d, u_latt%d, u_lspot%d;\n", i, i, i, i, i, i, i);
    }
    if (k->fog >= 2) sb(&v, "uniform vec4 u_fog;\n");
    sb(&v, "void main() {\n");
    if (k->rhw) {
        /* window pixels -> clip; w = 1/rhw keeps perspective-correct interpolation */
        sb(&v, "  float w = a_pos.w != 0.0 ? 1.0 / a_pos.w : 1.0;\n"
               "  vec2 ndc = (a_pos.xy + 0.5 - u_vpscale.zw) * u_vpscale.xy - 1.0;\n"
               "  gl_Position = vec4(ndc.x * w, -ndc.y * w, (2.0 * a_pos.z - 1.0) * w, w);\n"
               "  vec3 eye = vec3(0.0, 0.0, w);\n");
    } else {
        if (k->nblend) {
            /* D3D vertex blend: nblend-1 explicit weights, the last is 1 - sum */
            sb(&v, "  vec4 wts = a_weights; float last = 1.0;\n");
            for (int i = 0; i < k->nblend - 1; i++) sb(&v, "  last -= wts[%d];\n", i);
            sb(&v, "  wts[%d] = last;\n  vec4 ep = vec4(0.0); vec3 en = vec3(0.0);\n", k->nblend - 1);
            for (int i = 0; i < k->nblend; i++) {
                sb(&v, "  ep += wts[%d] * (u_wv[%d] * vec4(a_pos.xyz, 1.0));\n", i, i);
                if (has_n) sb(&v, "  en += wts[%d] * (mat3(u_wv[%d]) * a_normal);\n", i, i);
            }
            sb(&v, "  vec3 eye = ep.xyz;\n  vec4 cp = u_proj * vec4(eye, 1.0);\n");
        } else {
            sb(&v, "  vec4 cp = u_mvp * vec4(a_pos.xyz, 1.0);\n  vec3 eye = (u_wv[0] * vec4(a_pos.xyz, 1.0)).xyz;\n");
            if (has_n) sb(&v, "  vec3 en = mat3(u_wv[0]) * a_normal;\n");
        }
        sb(&v, "  gl_Position = vec4(cp.x, cp.y, 2.0 * cp.z - cp.w, cp.w);\n");
    }
    const char *vd = has_d ? "a_diffuse.bgra" : "vec4(1.0)", *vs = has_s ? "a_specular.bgra" : "vec4(0.0)";
    if (k->lighting) {
        /* material sources: 0 material, 1 vertex diffuse, 2 vertex specular */
        const char *src[4];
        for (int i = 0; i < 4; i++)
            src[i] = !k->color_vertex || k->mat_src[i] == 0 ? NULL : k->mat_src[i] == 1 ? (has_d ? vd : NULL) : (has_s ? vs : NULL);
        sb(&v, "  vec4 mdiff = %s, mspec = %s, mamb = %s, memis = %s;\n",
           src[0] ? src[0] : "u_mat[0]", src[1] ? src[1] : "u_mat[1]", src[2] ? src[2] : "u_mat[2]", src[3] ? src[3] : "u_mat[3]");
        sb(&v, "  vec3 N = normalize(en); vec3 V = normalize(-eye);\n"
               "  vec3 dsum = vec3(0.0), ssum = vec3(0.0), asum = u_ambient.rgb;\n");
        for (int i = 0; i < k->nlights; i++) {
            int t = k->light_type[i];   /* 1 point, 2 spot, 3 directional */
            if (t == 3) sb(&v, "  { vec3 L = -u_ldir%d; float att = 1.0;\n", i);
            else sb(&v, "  { vec3 dv = u_lpos%d - eye; float d = length(dv); vec3 L = dv / max(d, 1e-6);\n"
                        "    float att = d > u_latt%d.w ? 0.0 : 1.0 / max(u_latt%d.x + u_latt%d.y * d + u_latt%d.z * d * d, 1e-6);\n", i, i, i, i, i);
            if (t == 2) sb(&v, "    float rho = dot(-L, u_ldir%d);\n"
                               "    att *= rho > u_lspot%d.x ? 1.0 : rho <= u_lspot%d.y ? 0.0 : pow(max((rho - u_lspot%d.y) / max(u_lspot%d.x - u_lspot%d.y, 1e-6), 0.0), u_lspot%d.z);\n",
                            i, i, i, i, i, i, i);
            sb(&v, "    asum += u_lamb%d.rgb * att;\n    float nl = max(dot(N, L), 0.0);\n    dsum += u_ldiff%d.rgb * nl * att;\n", i, i);
            if (k->specular_en) sb(&v, "    if (nl > 0.0) ssum += u_lspec%d.rgb * pow(max(dot(N, normalize(L + V)), 0.0), u_mat_power) * att;\n", i);
            sb(&v, "  }\n");
        }
        sb(&v, "  v_diff = vec4(clamp(memis.rgb + mamb.rgb * asum + mdiff.rgb * dsum, 0.0, 1.0), mdiff.a);\n"
               "  v_spec = vec4(clamp(mspec.rgb * ssum, 0.0, 1.0), %s.a);\n", vs);
    } else {
        sb(&v, "  v_diff = %s;\n  v_spec = %s;\n", vd, vs);
    }
    /* fog factor (1 = no fog) */
    if (k->fog == 1) sb(&v, "  v_fog = %s;\n", has_s ? "a_specular.a" : "1.0");
    else if (k->fog >= 2) {
        sb(&v, "  float fd = %s;\n", k->fog_range ? "length(eye)" : "abs(eye.z)");
        if (k->fog == 2) sb(&v, "  v_fog = clamp((u_fog.y - fd) * u_fog.z, 0.0, 1.0);\n");
        else if (k->fog == 3) sb(&v, "  v_fog = clamp(exp(-fd * u_fog.w), 0.0, 1.0);\n");
        else sb(&v, "  v_fog = clamp(exp(-(fd * u_fog.w) * (fd * u_fog.w)), 0.0, 1.0);\n");
    } else sb(&v, "  v_fog = 1.0;\n");
    for (int i = 0; i < k->nstages; i++) {
        uint32_t tci = k->stage[i][6];
        int set = (int)(tci & 0xFFFF);
        if ((tci & 0xFFFF0000u) == D3DTSS_TCI_CAMERASPACENORMAL && !k->rhw) sb(&v, "  v_tc%d = vec4(%s, 1.0);\n", i, has_n ? "normalize(en)" : "vec3(0.0)");
        else if ((tci & 0xFFFF0000u) == D3DTSS_TCI_CAMERASPACEPOSITION && !k->rhw) sb(&v, "  v_tc%d = vec4(eye, 1.0);\n", i);
        else if ((tci & 0xFFFF0000u) == D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR && !k->rhw) sb(&v, "  v_tc%d = vec4(reflect(normalize(eye), %s), 1.0);\n", i, has_n ? "normalize(en)" : "vec3(0.0, 0.0, 1.0)");
        else if (set < ntex && set < 4) sb(&v, "  v_tc%d = a_tex%d;\n", i, set);
        else sb(&v, "  v_tc%d = vec4(0.0);\n", i);
    }
    sb(&v, "}\n");

    /* ---- fragment shader ---- */
    sb(&f, "precision mediump float;\nvarying vec4 v_diff, v_spec;\nvarying float v_fog;\n"
           "uniform vec4 u_tfactor, u_fogcolor;\nuniform float u_aref;\n");
    for (int i = 0; i < k->nstages; i++) sb(&f, "varying vec4 v_tc%d;\nuniform sampler2D s_tex%d;\n", i, i);
    sb(&f, "void main() {\n  vec4 cur = v_diff, temp = vec4(0.0);\n");
    for (int i = 0; i < k->nstages; i++) {
        const uint32_t *s = k->stage[i];
        if (k->tex_kind[i] == 1) sb(&f, "  vec4 t%d = vec4(1.0, 1.0, 1.0, texture2D(s_tex%d, v_tc%d.xy).a);\n", i, i, i);
        else sb(&f, "  vec4 t%d = texture2D(s_tex%d, v_tc%d.xy);\n", i, i, i);
        stage_op(&f, s[0], s[1], s[2], 0, i);
        if (s[3] != D3DTOP_DISABLE) stage_op(&f, s[3], s[4], s[5], 1, i);
    }
    if (k->specular_en) sb(&f, "  cur.rgb = min(cur.rgb + v_spec.rgb, vec3(1.0));\n");
    if (k->alpha_test) {
        static const char *cmp[9] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        int fn = k->alpha_test > 8 ? 8 : k->alpha_test;
        if (fn == 1) sb(&f, "  discard;\n");
        else if (fn != 8) sb(&f, "  if (!(floor(cur.a * 255.0 + 0.5) %s u_aref)) discard;\n", cmp[fn]);
    }
    if (k->fog) sb(&f, "  cur.rgb = mix(u_fogcolor.rgb, cur.rgb, v_fog);\n");
    sb(&f, "  gl_FragColor = cur;\n}\n");

    GLuint vs_ = compile(GL_VERTEX_SHADER, v.s), fs_ = compile(GL_FRAGMENT_SHADER, f.s);
    free(v.s); free(f.s);
    if (!vs_ || !fs_) return NULL;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs_); glAttachShader(p, fs_);
    static const char *const an[A_COUNT] = { "a_pos", "a_normal", "a_diffuse", "a_specular", "a_weights", "a_tex0", "a_tex1", "a_tex2", "a_tex3" };
    for (int i = 0; i < A_COUNT; i++) glBindAttribLocation(p, (GLuint)i, an[i]);
    glLinkProgram(p);
    glDeleteShader(vs_); glDeleteShader(fs_);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "[gl] link failed: %s\n", log); return NULL; }
    prog *r = calloc(1, sizeof *r);
    r->key = *k; r->prog = p;
    glUseProgram(p);
    char nm[32];
    r->u_mvp = glGetUniformLocation(p, "u_mvp"); r->u_proj = glGetUniformLocation(p, "u_proj");
    for (int i = 0; i < 4; i++) { sprintf(nm, "u_wv[%d]", i); r->u_wv[i] = glGetUniformLocation(p, nm); }
    r->u_vpscale = glGetUniformLocation(p, "u_vpscale"); r->u_tfactor = glGetUniformLocation(p, "u_tfactor");
    r->u_ambient = glGetUniformLocation(p, "u_ambient"); r->u_mat_power = glGetUniformLocation(p, "u_mat_power");
    for (int i = 0; i < 4; i++) { sprintf(nm, "u_mat[%d]", i); r->u_mat[i] = glGetUniformLocation(p, nm); }
    for (int i = 0; i < k->nlights; i++) {
        sprintf(nm, "u_lpos%d", i); r->u_lpos[i] = glGetUniformLocation(p, nm);
        sprintf(nm, "u_ldir%d", i); r->u_ldir[i] = glGetUniformLocation(p, nm);
        sprintf(nm, "u_ldiff%d", i); r->u_ldiff[i] = glGetUniformLocation(p, nm);
        sprintf(nm, "u_lspec%d", i); r->u_lspec[i] = glGetUniformLocation(p, nm);
        sprintf(nm, "u_lamb%d", i); r->u_lamb[i] = glGetUniformLocation(p, nm);
        sprintf(nm, "u_latt%d", i); r->u_latt[i] = glGetUniformLocation(p, nm);
        sprintf(nm, "u_lspot%d", i); r->u_lspot[i] = glGetUniformLocation(p, nm);
    }
    r->u_fog = glGetUniformLocation(p, "u_fog"); r->u_fogcolor = glGetUniformLocation(p, "u_fogcolor");
    r->u_aref = glGetUniformLocation(p, "u_aref");
    for (int i = 0; i < k->nstages; i++) { sprintf(nm, "s_tex%d", i); glUniform1i(glGetUniformLocation(p, nm), i); }
    g_nprogs++; g_draw_stats.progs++;
    return r;
}

/* ------------------------------------------------------------ the key */
static void make_key(draw_key *k, uint32_t fvf) {
    memset(k, 0, sizeof *k);
    const uint32_t *rs = g_dev.rs;
    k->fvf = fvf & (D3DFVF_POSITION_MASK | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEXCOUNT_MASK | 0xFFFF0000u);
    k->rhw = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    int has_n = (fvf & D3DFVF_NORMAL) != 0;
    k->lighting = !k->rhw && rs[D3DRS_LIGHTING] && has_n;
    if (!k->rhw && rs[D3DRS_VERTEXBLEND] && fvf_blend_weights(fvf)) {
        uint32_t vb = rs[D3DRS_VERTEXBLEND];   /* 1..3 weights -> 2..4 matrices */
        k->nblend = (uint8_t)(vb >= 1 && vb <= 3 ? vb + 1 : 0);
    }
    k->specular_en = rs[D3DRS_SPECULARENABLE] != 0;
    if (k->lighting) {
        k->color_vertex = rs[D3DRS_COLORVERTEX] != 0;
        k->mat_src[0] = (uint8_t)rs[D3DRS_DIFFUSEMATERIALSOURCE]; k->mat_src[1] = (uint8_t)rs[D3DRS_SPECULARMATERIALSOURCE];
        k->mat_src[2] = (uint8_t)rs[D3DRS_AMBIENTMATERIALSOURCE]; k->mat_src[3] = (uint8_t)rs[D3DRS_EMISSIVEMATERIALSOURCE];
        for (int i = 0; i < FAM_MAX_LIGHTS; i++)
            if (g_dev.light_enabled[i]) k->light_type[k->nlights++] = (uint8_t)g_dev.light[i].Type;
    }
    if (rs[D3DRS_FOGENABLE]) {
        uint32_t tm = rs[D3DRS_FOGTABLEMODE], vm = rs[D3DRS_FOGVERTEXMODE];
        uint32_t m = k->rhw ? 0 : (tm ? tm : vm);
        k->fog = m == D3DFOG_LINEAR ? 2 : m == D3DFOG_EXP ? 3 : m == D3DFOG_EXP2 ? 4 : 1;
        k->fog_range = !tm && rs[D3DRS_RANGEFOGENABLE];
    }
    if (rs[D3DRS_ALPHATESTENABLE]) k->alpha_test = (uint8_t)rs[D3DRS_ALPHAFUNC];
    for (int s = 0; s < FAM_MAX_STAGES; s++) {
        const uint32_t *t = g_dev.tss[s];
        if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
        fam_texture *tx = fam_texture_of(g_dev.tex[s]);
        k->stage[s][0] = t[D3DTSS_COLOROP]; k->stage[s][1] = t[D3DTSS_COLORARG1]; k->stage[s][2] = t[D3DTSS_COLORARG2];
        k->stage[s][3] = t[D3DTSS_ALPHAOP]; k->stage[s][4] = t[D3DTSS_ALPHAARG1]; k->stage[s][5] = t[D3DTSS_ALPHAARG2];
        k->stage[s][6] = t[D3DTSS_TEXCOORDINDEX];
        /* a stage reading an unbound texture sees opaque white, as D3D does */
        k->tex_kind[s] = tx && tx->format == D3DFMT_A8 ? 1 : 0;
        k->nstages = (uint8_t)(s + 1);
    }
}
static uint32_t key_hash(const draw_key *k) {
    const uint8_t *p = (const uint8_t *)k; uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static prog *program_for(uint32_t fvf) {
    draw_key k;
    make_key(&k, fvf);
    if (g_cur && !memcmp(&g_cur->key, &k, sizeof k)) return g_cur;
    uint32_t h = key_hash(&k) & (PROG_BUCKETS - 1);
    prog *p = g_progs[h];
    while (p && memcmp(&p->key, &k, sizeof k)) p = p->next;
    if (!p) {
        p = build_program(&k);
        if (!p) return NULL;
        p->next = g_progs[h]; g_progs[h] = p;
        { void fam_shader_cache_add(const void *, size_t); fam_shader_cache_add(&k, sizeof k); }
    }
    if (p != g_cur) { glUseProgram(p->prog); g_cur = p; }   /* uniforms are tracked per program */
    return p;
}

/* -------------------------------------------------------- GL fixed state */
static GLenum gl_blend(uint32_t b) {
    switch (b) {
    case D3DBLEND_ZERO: return GL_ZERO; case D3DBLEND_ONE: return GL_ONE;
    case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR; case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA; case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GL_DST_ALPHA; case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GL_DST_COLOR; case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}
static GLenum gl_cmp(uint32_t c) { return c >= 1 && c <= 8 ? GL_NEVER + (c - 1) : GL_ALWAYS; }
static GLenum gl_stencil_op(uint32_t o) {
    static const GLenum m[9] = { GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return o < 9 ? m[o] : GL_KEEP;
}
void fam_apply_viewport(void) {
    const fam_D3DVIEWPORT8 *v = &g_dev.vp;
    int x, y, w, h;
    fam_bb_to_gl((int)v->X, (int)v->Y, (int)v->Width, (int)v->Height, &x, &y, &w, &h);
    glViewport(x, y, w, h);
    glScissor(x, y, w, h);
    glDepthRangef(v->MinZ, v->MaxZ);
}
void fam_apply_write_masks(void) {
    uint32_t cw = g_dev.rs[D3DRS_COLORWRITEENABLE];
    glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    glDepthMask(g_dev.rs[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
    glStencilMask(g_dev.rs[D3DRS_STENCILWRITEMASK]);
}
/* GL state shadow: only this function sets these, so a change is issued only
 * when the value differs from what GL already has (~15 calls per draw before) */
static struct { int depth, blend, cull, stencil, poly; GLenum zfunc, bs, bd, beq, front, face;
                GLenum sfunc, sfail, szfail, spass; GLint sref; GLuint smask; uint32_t zbias; } G = { -1, -1, -1, -1, -1 };
static inline void cap(GLenum c, int on, int *have) { if (*have != on) { if (on) glEnable(c); else glDisable(c); *have = on; } }
static void apply_render_state(void) {
    const uint32_t *rs = g_dev.rs;
    cap(GL_DEPTH_TEST, rs[D3DRS_ZENABLE] != 0, &G.depth);
    if (rs[D3DRS_ZENABLE]) { GLenum f = gl_cmp(rs[D3DRS_ZFUNC]); if (G.zfunc != f) { glDepthFunc(f); G.zfunc = f; } }
    cap(GL_BLEND, rs[D3DRS_ALPHABLENDENABLE] != 0, &G.blend);
    if (rs[D3DRS_ALPHABLENDENABLE]) {
        uint32_t s = rs[D3DRS_SRCBLEND], d = rs[D3DRS_DESTBLEND];
        if (s == D3DBLEND_BOTHSRCALPHA) { s = D3DBLEND_SRCALPHA; d = D3DBLEND_INVSRCALPHA; }
        if (s == D3DBLEND_BOTHINVSRCALPHA) { s = D3DBLEND_INVSRCALPHA; d = D3DBLEND_SRCALPHA; }
        GLenum gs = gl_blend(s), gd = gl_blend(d);
        if (G.bs != gs || G.bd != gd) { glBlendFunc(gs, gd); G.bs = gs; G.bd = gd; }
        uint32_t op = rs[D3DRS_BLENDOP];
        GLenum eq = op == D3DBLENDOP_SUBTRACT ? GL_FUNC_SUBTRACT : op == D3DBLENDOP_REVSUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
        if (G.beq != eq) { glBlendEquation(eq); G.beq = eq; }
    }
    uint32_t cm = rs[D3DRS_CULLMODE];      /* see the header: D3D CW front = GL CW front */
    cap(GL_CULL_FACE, cm != D3DCULL_NONE, &G.cull);
    if (cm != D3DCULL_NONE) {
        if (G.front != GL_CW) { glFrontFace(GL_CW); G.front = GL_CW; }
        GLenum face = cm == D3DCULL_CW ? GL_FRONT : GL_BACK;
        if (G.face != face) { glCullFace(face); G.face = face; }
    }
    cap(GL_STENCIL_TEST, rs[D3DRS_STENCILENABLE] != 0, &G.stencil);
    if (rs[D3DRS_STENCILENABLE]) {
        GLenum f = gl_cmp(rs[D3DRS_STENCILFUNC]); GLint ref = (GLint)rs[D3DRS_STENCILREF]; GLuint msk = rs[D3DRS_STENCILMASK];
        if (G.sfunc != f || G.sref != ref || G.smask != msk) { glStencilFunc(f, ref, msk); G.sfunc = f; G.sref = ref; G.smask = msk; }
        GLenum a1 = gl_stencil_op(rs[D3DRS_STENCILFAIL]), a2 = gl_stencil_op(rs[D3DRS_STENCILZFAIL]), a3 = gl_stencil_op(rs[D3DRS_STENCILPASS]);
        if (G.sfail != a1 || G.szfail != a2 || G.spass != a3) { glStencilOp(a1, a2, a3); G.sfail = a1; G.szfail = a2; G.spass = a3; }
    }
    cap(GL_POLYGON_OFFSET_FILL, rs[D3DRS_ZBIAS] != 0, &G.poly);
    if (rs[D3DRS_ZBIAS] && G.zbias != rs[D3DRS_ZBIAS]) {
        glPolygonOffset(-(float)rs[D3DRS_ZBIAS] * 0.5f, -(float)rs[D3DRS_ZBIAS]); G.zbias = rs[D3DRS_ZBIAS];
    }
    fam_apply_write_masks();
}
static void apply_samplers(int nstages) {
    for (int s = 0; s < nstages; s++) {
        fam_texture *t = fam_texture_of(g_dev.tex[s]);
        fam_texture_bind(t, s);
        if (!t) continue;
        const uint32_t *ts = g_dev.tss[s];
        static const GLenum wrap[6] = { GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT };
        GLenum wu = wrap[ts[D3DTSS_ADDRESSU] < 6 ? ts[D3DTSS_ADDRESSU] : 1], wv = wrap[ts[D3DTSS_ADDRESSV] < 6 ? ts[D3DTSS_ADDRESSV] : 1];
        int lin_mag = ts[D3DTSS_MAGFILTER] >= 2, lin_min = ts[D3DTSS_MINFILTER] >= 2;
        uint32_t mip = t->levels > 1 ? ts[D3DTSS_MIPFILTER] : 0;
        GLenum mn = !mip ? (lin_min ? GL_LINEAR : GL_NEAREST)
                  : mip == 1 ? (lin_min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                             : (lin_min ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
        GLenum mg = lin_mag ? GL_LINEAR : GL_NEAREST;
        if (t->gl_wrap_u != wu) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wu); t->gl_wrap_u = wu; }
        if (t->gl_wrap_v != wv) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wv); t->gl_wrap_v = wv; }
        if (t->gl_min != mn) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)mn); t->gl_min = mn; }
        if (t->gl_mag != mg) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)mg); t->gl_mag = mg; }
    }
}
static void color4(uint32_t c, float *o) { o[0] = ((c >> 16) & 255) / 255.f; o[1] = ((c >> 8) & 255) / 255.f; o[2] = (c & 255) / 255.f; o[3] = (c >> 24) / 255.f; }
static void mul4(float *o, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        r[i*4+j] = a[i*4]*b[j] + a[i*4+1]*b[4+j] + a[i*4+2]*b[8+j] + a[i*4+3]*b[12+j];
    memcpy(o, r, 64);
}
static void apply_uniforms(prog *p) {
    const draw_key *k = &p->key;
    float c[4];
    if (k->rhw) {
        if (p->g_vp != g_gen.vp) {
            const fam_D3DVIEWPORT8 *v = &g_dev.vp;
            glUniform4f(p->u_vpscale, 2.f / (float)v->Width, 2.f / (float)v->Height, (float)v->X, (float)v->Y);
            p->g_vp = g_gen.vp;
        }
    } else {
        const float *view = g_dev.xf[D3DTS_VIEW], *proj = g_dev.xf[D3DTS_PROJECTION];
        if (p->g_xf != g_gen.xf) {
            int nm = k->nblend ? k->nblend : 1;
            for (int i = 0; i < nm; i++) {
                float wv[16]; mul4(wv, g_dev.xf[D3DTS_WORLDMATRIX(i)], view);
                glUniformMatrix4fv(p->u_wv[i], 1, GL_FALSE, wv);
                if (i == 0 && !k->nblend) { float mvp[16]; mul4(mvp, wv, proj); glUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp); }
            }
            if (k->nblend) glUniformMatrix4fv(p->u_proj, 1, GL_FALSE, proj);
            p->g_xf = g_gen.xf;
        }
        /* lights are in view space (depend on transforms) and ambient is a render state */
        if (k->lighting && (p->g_light != g_gen.light || p->g_lxf != g_gen.xf || p->g_lrs != g_gen.rs)) {
            p->g_light = g_gen.light; p->g_lxf = g_gen.xf; p->g_lrs = g_gen.rs;
            color4(g_dev.rs[D3DRS_AMBIENT], c); glUniform4fv(p->u_ambient, 1, c);
            const fam_D3DMATERIAL8 *m = &g_dev.material;
            glUniform4fv(p->u_mat[0], 1, &m->Diffuse.r); glUniform4fv(p->u_mat[1], 1, &m->Specular.r);
            glUniform4fv(p->u_mat[2], 1, &m->Ambient.r); glUniform4fv(p->u_mat[3], 1, &m->Emissive.r);
            glUniform1f(p->u_mat_power, m->Power);
            int n = 0;
            for (int i = 0; i < FAM_MAX_LIGHTS && n < k->nlights; i++) {
                if (!g_dev.light_enabled[i]) continue;
                const fam_D3DLIGHT8 *l = &g_dev.light[i];
                /* world -> view space (row vectors: p * V) */
                float pos[3], dir[3];
                for (int j = 0; j < 3; j++) {
                    pos[j] = l->Position.x * view[j] + l->Position.y * view[4 + j] + l->Position.z * view[8 + j] + view[12 + j];
                    dir[j] = l->Direction.x * view[j] + l->Direction.y * view[4 + j] + l->Direction.z * view[8 + j];
                }
                float dl = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]); if (dl > 0) for (int j = 0; j < 3; j++) dir[j] /= dl;
                glUniform3fv(p->u_lpos[n], 1, pos); glUniform3fv(p->u_ldir[n], 1, dir);
                glUniform4fv(p->u_ldiff[n], 1, &l->Diffuse.r); glUniform4fv(p->u_lspec[n], 1, &l->Specular.r);
                glUniform4fv(p->u_lamb[n], 1, &l->Ambient.r);
                glUniform4f(p->u_latt[n], l->Attenuation0, l->Attenuation1, l->Attenuation2, l->Range);
                glUniform4f(p->u_lspot[n], cosf(l->Theta * 0.5f), cosf(l->Phi * 0.5f), l->Falloff, 0.f);
                n++;
            }
        }
    }
    if (p->g_rs == g_gen.rs) return;
    p->g_rs = g_gen.rs;
    color4(g_dev.rs[D3DRS_TEXTUREFACTOR], c); glUniform4fv(p->u_tfactor, 1, c);
    if (k->fog) {
        color4(g_dev.rs[D3DRS_FOGCOLOR], c); glUniform4fv(p->u_fogcolor, 1, c);
        if (k->fog >= 2) {
            float st, en, de;
            memcpy(&st, &g_dev.rs[D3DRS_FOGSTART], 4); memcpy(&en, &g_dev.rs[D3DRS_FOGEND], 4); memcpy(&de, &g_dev.rs[D3DRS_FOGDENSITY], 4);
            glUniform4f(p->u_fog, st, en, en != st ? 1.f / (en - st) : 0.f, de);
        }
    }
    if (k->alpha_test) glUniform1f(p->u_aref, (float)(g_dev.rs[D3DRS_ALPHAREF] & 255));
}

/* --------------------------------------------------------------- draws */
static GLenum prim(uint32_t t, uint32_t n, uint32_t *count) {
    switch (t) {
    case D3DPT_POINTLIST: *count = n; return GL_POINTS;
    case D3DPT_LINELIST: *count = n * 2; return GL_LINES;
    case D3DPT_LINESTRIP: *count = n + 1; return GL_LINE_STRIP;
    case D3DPT_TRIANGLELIST: *count = n * 3; return GL_TRIANGLES;
    case D3DPT_TRIANGLESTRIP: *count = n + 2; return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN: *count = n + 2; return GL_TRIANGLE_FAN;
    }
    *count = 0; return GL_TRIANGLES;
}

/* Bind attributes for vertices at `base` (a GL buffer offset when vbo, else a host pointer). */
/* attribute pointers: re-specified only when buffer, format, stride or offset change */
static struct { GLuint buf; GLint size; GLenum type; GLsizei stride; const void *ptr; } g_attr[A_COUNT];
extern unsigned fam_bound_array_buffer(void);
void fam_attr_forget(unsigned b) { for (int i = 0; i < A_COUNT; i++) if (g_attr[i].buf == b) g_attr[i].buf = ~0u; }
static inline void attr(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr) {
    GLuint buf = fam_bound_array_buffer();
    if (g_attr[i].buf == buf && g_attr[i].ptr == ptr && g_attr[i].stride == stride && g_attr[i].size == size && g_attr[i].type == type && buf) return;
    glVertexAttribPointer(i, size, type, norm, stride, ptr);
    g_attr[i].buf = buf; g_attr[i].size = size; g_attr[i].type = type; g_attr[i].stride = stride; g_attr[i].ptr = ptr;
}
/* Bind attributes for vertices at `base` (a GL buffer offset when vbo, else a host pointer). */
static uint32_t g_enabled_attrs;
static void bind_attribs(const prog *p, uint32_t fvf, uint32_t stride, const uint8_t *base) {
    fvf_layout l; fvf_decode(fvf, &l);
    if (!stride) stride = (uint32_t)l.stride;
#define enabled g_enabled_attrs
    uint32_t want = 1u << A_POS;
    attr(A_POS, p->key.rhw ? 4 : 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, base + l.pos);
    if (l.normal >= 0 && (p->key.lighting || p->key.nstages)) { attr(A_NORMAL, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, base + l.normal); want |= 1u << A_NORMAL; }
    if (l.diffuse >= 0) { attr(A_DIFFUSE, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, base + l.diffuse); want |= 1u << A_DIFFUSE; }
    if (l.specular >= 0) { attr(A_SPECULAR, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, base + l.specular); want |= 1u << A_SPECULAR; }
    if (p->key.nblend) { attr(A_WEIGHTS, fvf_blend_weights(fvf) > 4 ? 4 : fvf_blend_weights(fvf), GL_FLOAT, GL_FALSE, (GLsizei)stride, base + 12); want |= 1u << A_WEIGHTS; }
    for (int i = 0; i < 4 && i < (int)fvf_tex_count(fvf); i++) {
        attr((GLuint)(A_TEX0 + i), fvf_tex_size(fvf, i), GL_FLOAT, GL_FALSE, (GLsizei)stride, base + l.tex[i]);
        want |= 1u << (A_TEX0 + i);
    }
    for (int i = 0; i < A_COUNT; i++) {
        uint32_t b = 1u << i;
        if ((want & b) && !(enabled & b)) glEnableVertexAttribArray((GLuint)i);
        if (!(want & b) && (enabled & b)) glDisableVertexAttribArray((GLuint)i);
    }
    enabled = want;
#undef enabled
}

/* Someone else drew with GL (the movie player): forget every cached piece of
 * GL state so the next draw sets all of it again. */
void fam_resource_invalidate_gl(void);
void fam_bind_invalidate(void);
void fam_gl_invalidate_caches(void) {
    memset(&G, 0, sizeof G);
    G.depth = G.blend = G.cull = G.stencil = G.poly = -1;
    G.zfunc = G.bs = G.bd = G.beq = G.front = G.face = G.sfunc = G.sfail = G.szfail = G.spass = ~0u; G.sref = -1; G.smask = ~0u; G.zbias = ~0u;
    for (int i = 0; i < A_COUNT; i++) g_attr[i].buf = ~0u;
    for (int i = 0; i < A_COUNT; i++) glDisableVertexAttribArray((GLuint)i);
    g_enabled_attrs = 0;
    g_cur = NULL;
    fam_resource_invalidate_gl();
    fam_bind_invalidate();
    g_dev.state_dirty = ~0u;
    g_gen.xf++; g_gen.light++; g_gen.rs++; g_gen.vp++;
}

static prog *prepare(uint32_t fvf) {
    prog *p = program_for(fvf);
    if (!p) return NULL;
    uint32_t d = g_dev.state_dirty;
    if (d & FAM_DIRTY_XF) g_gen.xf++;
    if (d & (FAM_DIRTY_LIGHT | FAM_DIRTY_MAT)) g_gen.light++;
    if (d & FAM_DIRTY_RS) g_gen.rs++;
    if (d & FAM_DIRTY_VP) g_gen.vp++;
    if (g_dev.state_dirty & (FAM_DIRTY_RS)) apply_render_state();
    if (g_dev.state_dirty & FAM_DIRTY_VP) fam_apply_viewport();
    apply_samplers(p->key.nstages);
    apply_uniforms(p);
    g_dev.state_dirty = 0;
    g_draw_stats.draws++;
    return p;
}

/* streaming (d3d8_stream.c) */
int fam_stream_on(void); long fam_stream_put(const void *src, uint32_t n, uint32_t align); GLuint fam_stream_buffer(void);
void fam_stream_need(uint32_t n);
typedef void (GL_APIENTRY *PFN_rangebv)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint);
extern PFN_rangebv fam_glDrawRangeElementsBaseVertex;

/* vertices [first, first+count) of guest data at `src` with `stride`: drawn
 * from the ring when it is on (returns the ring's base vertex for them, the
 * attributes bound at offset 0), else from the client pointer (-1) */
static long stream_vertices(const prog *p, uint32_t fvf, uint32_t stride, const uint8_t *src, uint32_t first, uint32_t count) {
    if (!stride) { fvf_layout l; fvf_decode(fvf, &l); stride = (uint32_t)l.stride; }
    if (fam_stream_on() && stride && count) {
        long off = fam_stream_put(src + (size_t)first * stride, count * stride, stride);
        if (off >= 0) {
            fam_bind_array(fam_stream_buffer());
            bind_attribs(p, fvf, stride, (const uint8_t *)0);
            return off / stride;
        }
    }
    fam_bind_array(0);
    bind_attribs(p, fvf, stride, src);
    return -1;
}

static void d_DrawPrimitive(void) {                  /* (this, type, startVertex, primCount) */
    uint32_t n; GLenum m = prim(A32(1), A32(3), &n);
    fam_buffer *vb = fam_buffer_of(g_dev.stream_vb);
    prog *p = vb ? prepare(fam_vs_fvf(g_dev.fvf)) : NULL;
    if (p && n) {
        uint32_t fvf = fam_vs_fvf(g_dev.fvf), gl = fam_buffer_gl(vb, GL_ARRAY_BUFFER);
        if (gl) {
            fam_bind_array(gl);
            bind_attribs(p, fvf, g_dev.stream_stride, (const uint8_t *)0 + (size_t)A32(2) * g_dev.stream_stride);
            glDrawArrays(m, 0, (GLsizei)n);
        } else {
            long bv = stream_vertices(p, fvf, g_dev.stream_stride, (const uint8_t *)hle_ptr(vb->mem), A32(2), n);
            glDrawArrays(m, bv >= 0 ? (GLint)bv : (GLint)A32(2), (GLsizei)n);
            if (bv < 0) { /* client path drew from the start of the buffer */ }
        }
        g_draw_stats.prims += A32(3);
    }
    RET(D3D_OK, 4);
}

/* indices: a GL buffer, the ring, or a client pointer; returns the offset/pointer argument */
static const void *stream_indices(fam_buffer *ib, uint32_t start, uint32_t n, int i32) {
    uint32_t igl = fam_buffer_gl(ib, GL_ELEMENT_ARRAY_BUFFER);
    uint32_t isz = i32 ? 4 : 2;
    if (igl) { fam_bind_element(igl); return (const uint8_t *)0 + (size_t)start * isz; }
    const uint8_t *src = (const uint8_t *)hle_ptr(ib->mem) + (size_t)start * isz;
    if (fam_stream_on()) {
        long off = fam_stream_put(src, n * isz, 4);
        if (off >= 0) { fam_bind_element(fam_stream_buffer()); return (const uint8_t *)0 + off; }
    }
    fam_bind_element(0);
    return src;
}

static void d_DrawIndexedPrimitive(void) {           /* (this, type, minIndex, numVertices, startIndex, primCount) */
    uint32_t n; GLenum m = prim(A32(1), A32(5), &n);
    fam_buffer *vb = fam_buffer_of(g_dev.stream_vb), *ib = fam_buffer_of(g_dev.ib);
    prog *p = vb && ib ? prepare(fam_vs_fvf(g_dev.fvf)) : NULL;
    if (p && n) {
        int i32 = ib->fvf_or_format == D3DFMT_INDEX32;
        GLenum it = i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        uint32_t fvf = fam_vs_fvf(g_dev.fvf), gl = fam_buffer_gl(vb, GL_ARRAY_BUFFER);
        uint32_t minI = A32(2), numV = A32(3);
        if (gl) {
            fam_bind_array(gl);
            bind_attribs(p, fvf, g_dev.stream_stride, (const uint8_t *)0 + (size_t)g_dev.base_vertex * g_dev.stream_stride);
            const void *ix = stream_indices(ib, A32(4), n, i32);
            if (fam_glDrawRangeElementsBaseVertex && numV)
                fam_glDrawRangeElementsBaseVertex(m, minI, minI + numV - 1, (GLsizei)n, it, ix, 0);
            else glDrawElements(m, (GLsizei)n, it, ix);
        } else {
            const uint8_t *vsrc = (const uint8_t *)hle_ptr(vb->mem) + (size_t)g_dev.base_vertex * g_dev.stream_stride;
            if (fam_stream_on()) fam_stream_need(numV * (g_dev.stream_stride ? g_dev.stream_stride : 64) + n * (i32 ? 4 : 2) + 2 * g_dev.stream_stride);
            long bv = -1;
            /* LS3D draws each face group of a mesh with the whole vertex range of its
             * LOD: a range copied into the ring earlier in this segment, from the same
             * unchanged buffer, is drawn again from there instead of copied again */
            static struct { const uint8_t *src; const fam_buffer *vb; uint32_t ver, first, count, stride, epoch; long bv; } vc[8];
            static unsigned vc_next;
            extern uint32_t fam_stream_epoch;
            uint32_t stride = g_dev.stream_stride;
            if (numV && fam_stream_on() && stride) {
                for (int k = 0; k < 8; k++)
                    if (vc[k].src == vsrc && vc[k].vb == vb && vc[k].ver == vb->version && vc[k].stride == stride &&
                        vc[k].epoch == fam_stream_epoch && minI >= vc[k].first && minI + numV <= vc[k].first + vc[k].count) {
                        bv = vc[k].bv + (long)(minI - vc[k].first);
                        fam_bind_array(fam_stream_buffer());
                        bind_attribs(p, fvf, stride, (const uint8_t *)0);
                        g_draw_stats.vreuse++;
                        break;
                    }
            }
            if (bv < 0 && numV) {
                bv = stream_vertices(p, fvf, stride, vsrc, minI, numV);
                if (bv >= 0) { unsigned k = vc_next++ & 7; vc[k].src = vsrc; vc[k].vb = vb; vc[k].ver = vb->version; vc[k].first = minI;
                               vc[k].count = numV; vc[k].stride = stride; vc[k].epoch = fam_stream_epoch; vc[k].bv = bv; }
            }
            if (!numV) { fam_bind_array(0); bind_attribs(p, fvf, g_dev.stream_stride, vsrc); }
            const void *ix = stream_indices(ib, A32(4), n, i32);
            if (bv >= 0) fam_glDrawRangeElementsBaseVertex(m, minI, minI + numV - 1, (GLsizei)n, it, ix, (GLint)(bv - (long)minI));
            else glDrawElements(m, (GLsizei)n, it, ix);
        }
        g_draw_stats.prims += A32(5);
    }
    RET(D3D_OK, 6);
}
static void d_DrawPrimitiveUP(void) {                /* (this, type, primCount, data, stride) */
    uint32_t n; GLenum m = prim(A32(1), A32(2), &n);
    prog *p = prepare(fam_vs_fvf(g_dev.fvf));
    if (p && n) {
        long bv = stream_vertices(p, fam_vs_fvf(g_dev.fvf), A32(4), (const uint8_t *)hle_ptr(A32(3)), 0, n);
        glDrawArrays(m, bv >= 0 ? (GLint)bv : 0, (GLsizei)n);
        g_draw_stats.prims += A32(2);
    }
    g_dev.stream_vb = 0;     /* D3D8 resets stream 0 after a UP draw */
    RET(D3D_OK, 5);
}
static void d_DrawIndexedPrimitiveUP(void) {  /* (this, type, minIndex, numVerts, primCount, idx, idxFmt, data, stride) */
    uint32_t n; GLenum m = prim(A32(1), A32(4), &n);
    prog *p = prepare(fam_vs_fvf(g_dev.fvf));
    if (p && n) {
        int i32 = A32(6) == D3DFMT_INDEX32;
        GLenum it = i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        uint32_t minI = A32(2), numV = A32(3);
        const uint8_t *vsrc = (const uint8_t *)hle_ptr(A32(7));
        if (fam_stream_on()) fam_stream_need(numV * (A32(8) ? A32(8) : 64) + n * (i32 ? 4 : 2) + 2 * A32(8));
        long bv = numV ? stream_vertices(p, fam_vs_fvf(g_dev.fvf), A32(8), vsrc, minI, numV) : -1;
        if (bv < 0 && !numV) { fam_bind_array(0); bind_attribs(p, fam_vs_fvf(g_dev.fvf), A32(8), vsrc); }
        const uint8_t *isrc = (const uint8_t *)hle_ptr(A32(5));
        long ioff = bv >= 0 ? fam_stream_put(isrc, n * (i32 ? 4 : 2), 4) : -1;
        if (bv >= 0 && ioff >= 0) {
            fam_bind_element(fam_stream_buffer());
            fam_glDrawRangeElementsBaseVertex(m, minI, minI + numV - 1, (GLsizei)n, it, (const uint8_t *)0 + ioff, (GLint)(bv - (long)minI));
        } else {
            if (bv >= 0) { fam_bind_array(0); bind_attribs(p, fam_vs_fvf(g_dev.fvf), A32(8), vsrc); }
            fam_bind_element(0);
            glDrawElements(m, (GLsizei)n, it, isrc);
        }
        g_draw_stats.prims += A32(4);
    }
    g_dev.stream_vb = 0; g_dev.ib = 0;
    RET(D3D_OK, 9);
}
static void d_SetVertexShader(void) {                /* (this, handle): FVF codes only */
    if (fam_sb_rec_fvf(A32(1))) RET(D3D_OK, 2);
    g_dev.fvf = A32(1);
    RET(D3D_OK, 2);
}
static void d_GetVertexShader(void) { MEM32(A32(1)) = g_dev.fvf; RET(D3D_OK, 2); }
static void d_SetPixelShader(void) { RET(A32(1) ? D3DERR_INVALIDCALL : D3D_OK, 2); }

/* ---- shader variants seen before, built at start ----
 * A variant is compiled the first time its render state appears; mid-game
 * that is a visible hitch (16 at once took ~0.4 s). Each new key is appended
 * to conf/shaders.bin and every known key is built when the device starts. */
#define SHADER_CACHE_MAGIC 0x46534831u        /* "FSH1" */
static char g_sc_path[1024];
void fam_shader_cache_add(const void *key, size_t n) {
    if (!g_sc_path[0]) return;
    FILE *f = fopen(g_sc_path, "ab");
    if (!f) return;
    fwrite(key, 1, n, f);
    fclose(f);
}
static void shader_cache_load(void) {
    snprintf(g_sc_path, sizeof g_sc_path, "%s/shaders.bin", fam_user_dir());
    uint32_t hdr[2] = { SHADER_CACHE_MAGIC, (uint32_t)sizeof(draw_key) }, got[2];
    FILE *f = fopen(g_sc_path, "rb");
    if (!f) {                                      /* first start: the variants the port's own test runs met */
        FILE *seed = fopen("shaders.seed", "rb"), *out = seed ? fopen(g_sc_path, "wb") : NULL;
        if (seed && out) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, seed)) > 0) fwrite(buf, 1, n, out); }
        if (seed) fclose(seed);
        if (out) fclose(out);
        f = fopen(g_sc_path, "rb");
    }
    int ok = f && fread(got, sizeof got, 1, f) == 1 && got[0] == hdr[0] && got[1] == hdr[1];
    if (!ok) {                                     /* none yet, or another layout: start over */
        if (f) fclose(f);
        f = fopen(g_sc_path, "wb");
        if (f) { fwrite(hdr, sizeof hdr, 1, f); fclose(f); }
        return;
    }
    draw_key k; int n = 0;
    uint64_t t0 = SDL_GetPerformanceCounter();
    while (fread(&k, sizeof k, 1, f) == 1) {
        uint32_t h = key_hash(&k) & (PROG_BUCKETS - 1);
        prog *p = g_progs[h];
        while (p && memcmp(&p->key, &k, sizeof k)) p = p->next;
        if (p) continue;
        p = build_program(&k);
        if (!p) continue;
        p->next = g_progs[h]; g_progs[h] = p; n++;
    }
    fclose(f);
    fprintf(stderr, "[d3d8] %d shader variants prebuilt from %s (%.0f ms)\n", n, g_sc_path,
            (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
}

void fam_draw_init(void) {
    glDisable(GL_DITHER);
    { void fam_stream_init(void); fam_stream_init(); }
    { static int once; if (!once) { once = 1; shader_cache_load(); } }
    g_dev.state_dirty = ~0u;
}

const fam_method fam_dev_draw_methods[] = {
    FAM_DEV(DrawPrimitive, d_DrawPrimitive) FAM_DEV(DrawIndexedPrimitive, d_DrawIndexedPrimitive)
    FAM_DEV(DrawPrimitiveUP, d_DrawPrimitiveUP) FAM_DEV(DrawIndexedPrimitiveUP, d_DrawIndexedPrimitiveUP)
    FAM_DEV(SetVertexShader, d_SetVertexShader) FAM_DEV(GetVertexShader, d_GetVertexShader)
    FAM_DEV(SetPixelShader, d_SetPixelShader)
    { 0, 0 }
};
