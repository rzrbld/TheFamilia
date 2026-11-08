/*
 * native_light.c - native replacements for LS3D's per-object vertex lighting
 * and occlusion culling.
 *
 *   LS3DF 0x10065A70  PrepareLights        (fastcall: ecx dst, edx dst_stride,
 *                                            8 stack args, ret 0x20, bool in al)
 *   LS3DF 0x1003C070  ProcessOccluders     (ecx scene, ret)
 *   LS3DF 0x1003C2E0  CullOcclsEachOther   (ecx sector, ret)
 *   LS3DF 0x1003C480  POIS_EnumProc        (ecx first frame, ret)
 *
 * Written from the x86 of LS3DF.dll 1.2 (names from the mafia-re map). The
 * arithmetic follows the translated original: every x87 value is a C double,
 * rounded to float only where the x86 stores a dword; integer compares on
 * float bits use the stored bits; fistp rounds by g_fpu_cw; `fdivp` by zero
 * leaves the dividend (the translator's rule). fam_native_light_check
 * (FAMILIA_NATIVE_CHECK=1) compares each with its translated original.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

int fam_native_light_on = 1;

void recomp_ls3df_10065A70_orig(void);
void recomp_ls3df_1003C070_orig(void);
void recomp_ls3df_1003C2E0_orig(void);
void recomp_ls3df_1003C480_orig(void);
void fam_vmm(const float *V, const float *M, float *R);

static inline float ldf(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static inline void stf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }
static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline int32_t ibits(float f) { return (int32_t)fbits(f); }

static recomp_func_t guest(uint32_t va) {
    recomp_func_t f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup(va);
    if (!f) { fprintf(stderr, "[native] light: %08X not found\n", va); abort(); }
    return f;
}
static void call_this0(uint32_t va, uint32_t self) { g_ecx = self; PUSH32(g_esp, RECOMP_RETADDR); guest(va)(); }
static void call_std1(uint32_t va, uint32_t a) { PUSH32(g_esp, a); PUSH32(g_esp, RECOMP_RETADDR); guest(va)(); }
static void call_std3(uint32_t va, uint32_t a, uint32_t b, uint32_t c) {
    PUSH32(g_esp, c); PUSH32(g_esp, b); PUSH32(g_esp, a); PUSH32(g_esp, RECOMP_RETADDR); guest(va)();
}
static inline int32_t fistp(double v) { return (int32_t)fp_round_cw(v, g_fpu_cw); }

/* ======================================================== PrepareLights */
/* globals */
#define G_TLINDEX   0x101C4D20u   /* TempLightIndex */
#define G_AMB       0x101C4D30u   /* TempAmbient x,y,z */
#define G_PVERT     0x101C4D40u   /* pVertices */
#define G_TCOLOR    0x101C4D50u   /* TempColor x,y,z (static) */
#define G_TENERGY   0x101C4D5Cu   /* TempLightEnergy */
#define G_PTLIGHT   0x101C4D60u   /* pTempLight */
#define G_TALPHA    0x101C4D70u   /* TempAlpha */
#define G_TCGUARD   0x101C4D74u   /* TempColor's construction guard */
#define G_TLIST     0x101C4D80u   /* TempLightList[8], 0x50 each */
#define G_LCOUNT    0x101C5000u   /* GlobalLightCount */
#define G_SECTOR    0x101C5004u   /* pCurrentSector */
#define G_VLMULT    0x100A9470u   /* vertex_light_mult */
#define G_ONE       0x100A947Cu   /* OneFloat */
/* TempLightList entry */
#define TL_POS 0x00
#define TL_DIR 0x10
#define TL_COL 0x20
#define TL_RN  0x30
#define TL_RF  0x34
#define TL_RF2 0x38
#define TL_RD  0x3C
#define TL_LIGHT 0x40
#define TL_ENERGY 0x44

/* a slot for pTempLight by TempLightEnergy: the next free one, or the first weaker one, or -1 */
static int32_t temp_light_index(void) {
    uint32_t n = MEM32(G_LCOUNT);
    if (n < 8) { MEM32(G_LCOUNT) = n + 1; return (int32_t)n; }
    double e = ldf(G_TENERGY);
    for (uint32_t i = 0; i < n; i++)
        if ((double)ldf(G_TLIST + 0x50 * i + TL_ENERGY) < e) return (int32_t)i;
    return -1;
}

/* the -w_dir rotated into the object, normalized as the original does */
static void light_dir(uint32_t light, uint32_t m, uint32_t e) {
    g_esp -= 0x10;                                     /* the result, in a frame of ours on the guest stack */
    uint32_t tmp = g_esp;
    call_std3(0x1002EA10u, light + 0x184, tmp, m);     /* S_vector::RotateByMatrix, ret 0xc */
    float r[3] = { ldf(tmp), ldf(tmp + 4), ldf(tmp + 8) };
    g_esp += 0x10;
    uint32_t d = G_TLIST + e + TL_DIR;
    stf(d, -r[0]); stf(d + 4, -r[1]); stf(d + 8, -r[2]);
    double x = ldf(d), y = ldf(d + 4), z = ldf(d + 8);
    float sq = (float)((x * x + y * y) + z * z);
    if (fabs((double)sq - 1.0) < 9.99999993922529e-09) return;
    if (!((double)sq < (double)1e-8f)) {
        double inv = 1.0 / sqrt((double)sq);
        stf(d, (float)(inv * x)); stf(d + 4, (float)(inv * y)); stf(d + 8, (float)(inv * z));
        return;
    }
    if (!(x == 0.0)) stf(d, x < 0.0 ? -1.0f : 1.0f);
    else if (!(z == 0.0)) stf(d + 8, z < 0.0 ? -1.0f : 1.0f);
    else stf(d + 4, y < 0.0 ? -1.0f : 1.0f);
}

static void light_color(uint32_t light, uint32_t e) {
    uint32_t c = G_TLIST + e + TL_COL;
    stf(c, (float)((double)ldf(G_TCOLOR) * ldf(light + 0x190)));
    stf(c + 4, (float)((double)ldf(G_TCOLOR + 4) * ldf(light + 0x194)));
    stf(c + 8, (float)((double)ldf(G_TCOLOR + 8) * ldf(light + 0x198)));
}

static void light_pos_range(uint32_t light, uint32_t m, uint32_t e, float scale) {
    float p[3];
    fam_vmm((const float *)ADDR(light + 0x178), (const float *)ADDR(m), p);
    memcpy((void *)ADDR(G_TLIST + e + TL_POS), p, 12);
    uint32_t t = G_TLIST + e;
    float rn = (float)((double)scale * ldf(light + 0x19C));
    stf(t + TL_RN, rn);
    double rf = (double)scale * ldf(light + 0x1D0);
    stf(t + TL_RF, (float)rf);
    double delta = rf - (double)rn;
    stf(t + TL_RD, (float)delta);
    if (delta > 0.0) stf(t + TL_RD, (float)(1.0 / delta));
    light_color(light, e);
    double rff = ldf(t + TL_RF);
    stf(t + TL_RF2, (float)(rff * rff));
}

/* the light reaches the object's sphere: the energy, a slot; returns the slot or -1 */
static int32_t take_light(uint32_t light, double dist2) {
    double s = ((double)ldf(light + 0x198) + ldf(light + 0x194)) + ldf(light + 0x190);
    double d = sqrt(dist2);
    stf(G_TENERGY, (float)(d == 0.0 ? s : s / d));
    int32_t idx = temp_light_index();
    MEM32(G_TLINDEX) = (uint32_t)idx;
    if (idx >= 0) {
        uint32_t e = (uint32_t)idx * 0x50;
        MEM32(G_TLIST + e + TL_LIGHT) = light;
        MEM32(G_TLIST + e + TL_ENERGY) = MEM32(G_TENERGY);
    }
    return idx;
}

static double sphere_dist2(const float *b, uint32_t lpos) {
    double dx = (double)b[0] - ldf(lpos), dy = (double)b[1] - ldf(lpos + 4), dz = (double)b[2] - ldf(lpos + 8);
    return (dz * dz + dy * dy) + dx * dx;
}

/* one light as the vertex loop sees it */
typedef struct {
    int type;                    /* 1 point, 2 spot, 3 directional, else none */
    double px, py, pz, dx, dy, dz, cx, cy, cz;
    double rf, rd;               /* range_f, range_delta (floats) */
    int32_t rn_bits, rf2_bits;
    uint32_t outer_ubits; int32_t inner_bits; double outer, cscale;
} vlight;

static uint32_t light_vertex(const vlight *L, int n, const float *V, uint32_t alpha, double amb[3], double mult, double one) {
    double ax = amb[0], ay = amb[1], az = amb[2];
    double nx = V[3], ny = V[4], nz = V[5];
    for (int j = 0; j < n; j++) {
        const vlight *l = &L[j];
        double v;
        if (l->type == 3) {
            double d = ((l->dx * nx) + l->dy * ny) + l->dz * nz;
            if (signbit((float)d)) continue;
            v = d;
        } else if (l->type == 1 || l->type == 2) {
            float t0 = (float)(l->px - (double)V[0]), t1 = (float)(l->py - (double)V[1]), t2 = (float)(l->pz - (double)V[2]);
            double d = (((double)t0 * nx) + (double)t1 * ny) + (double)t2 * nz;
            if (signbit((float)d)) continue;
            double dist2 = ((double)t0 * t0 + (double)t1 * t1) + (double)t2 * t2;
            if (l->rf2_bits < ibits((float)dist2)) continue;
            double dist = sqrt(dist2);
            if (!(l->rn_bits >= ibits((float)dist))) d = d * ((l->rf - dist) * l->rd);
            if (l->type == 1) {
                v = dist == 0.0 ? d : d / dist;
            } else {
                float inv = (float)(one / dist);
                double c = ((l->dx * t0 + l->dy * t1) + l->dz * t2) * (double)inv;
                float cf = (float)c;
                if (signbit(cf)) continue;
                if (l->outer_ubits >= fbits(cf)) continue;
                if (!(l->inner_bits <= ibits(cf))) d = d * ((c - l->outer) * l->cscale);
                v = d * (double)inv;
            }
        } else continue;
        if (ibits((float)v) < 0x322BCC77) continue;
        ax = ax + l->cx * v;
        ay = ay + l->cy * v;
        az = az + l->cz * v;
    }
    int32_t b = fistp(az * mult), g = fistp(ay * mult), r = fistp(ax * mult);
    uint32_t ub = (uint32_t)b & 0xFFFFFF00u ? 0xFF : (uint32_t)b;
    uint32_t ug = (uint32_t)g & 0xFFFFFF00u ? 0xFF : (uint32_t)g;
    uint32_t ur = (uint32_t)r & 0xFFFFFF00u ? 0xFF : (uint32_t)r;
    return alpha | ub | ug << 8 | ur << 16;
}

#if defined(__aarch64__)
#include <arm_neon.h>
/* The per-vertex loop in single precision, four vertices at a time. Every
 * test the original makes is kept (sign bits, integer compares on the float
 * bits, round to nearest, the clamp that turns negatives into 255); only the
 * arithmetic is float instead of the translation's double, so a channel can
 * differ by 1 where a sum lands on a rounding boundary. (The original game
 * ran the FPU in single precision.) FAMILIA_LIGHT_EXACT=1 selects the
 * double-precision path. */
typedef struct { float px, py, pz, dx, dy, dz, cx, cy, cz, rf, rd, outer, cscale;
                 int32_t rn_bits, rf2_bits, inner_bits; uint32_t outer_ubits; int type; } flight;
static inline uint32x4_t sign_clear(float32x4_t v) { return vcgeq_s32(vreinterpretq_s32_f32(v), vdupq_n_s32(0)); }
static void light4(const flight *L, int n, const float *V[4], uint32_t alpha, const float amb[3], float mult, float one, uint32_t out[4]) {
    float32x4_t px = { V[0][0], V[1][0], V[2][0], V[3][0] }, py = { V[0][1], V[1][1], V[2][1], V[3][1] }, pz = { V[0][2], V[1][2], V[2][2], V[3][2] };
    float32x4_t nx = { V[0][3], V[1][3], V[2][3], V[3][3] }, ny = { V[0][4], V[1][4], V[2][4], V[3][4] }, nz = { V[0][5], V[1][5], V[2][5], V[3][5] };
    float32x4_t ar = vdupq_n_f32(amb[0]), ag = vdupq_n_f32(amb[1]), ab = vdupq_n_f32(amb[2]), zero = vdupq_n_f32(0.0f);
    for (int j = 0; j < n; j++) {
        const flight *l = &L[j];
        float32x4_t v; uint32x4_t ok;
        if (l->type == 3) {
            float32x4_t d = vaddq_f32(vaddq_f32(vmulq_n_f32(nx, l->dx), vmulq_n_f32(ny, l->dy)), vmulq_n_f32(nz, l->dz));
            ok = sign_clear(d); v = d;
        } else if (l->type == 1 || l->type == 2) {
            float32x4_t t0 = vsubq_f32(vdupq_n_f32(l->px), px), t1 = vsubq_f32(vdupq_n_f32(l->py), py), t2 = vsubq_f32(vdupq_n_f32(l->pz), pz);
            float32x4_t d = vaddq_f32(vaddq_f32(vmulq_f32(t0, nx), vmulq_f32(t1, ny)), vmulq_f32(t2, nz));
            ok = sign_clear(d);
            float32x4_t dist2 = vaddq_f32(vaddq_f32(vmulq_f32(t0, t0), vmulq_f32(t1, t1)), vmulq_f32(t2, t2));
            ok = vandq_u32(ok, vcleq_s32(vreinterpretq_s32_f32(dist2), vdupq_n_s32(l->rf2_bits)));
            float32x4_t dist = vsqrtq_f32(dist2);
            uint32x4_t att = vcgtq_s32(vreinterpretq_s32_f32(dist), vdupq_n_s32(l->rn_bits));
            d = vbslq_f32(att, vmulq_f32(d, vmulq_n_f32(vsubq_f32(vdupq_n_f32(l->rf), dist), l->rd)), d);
            if (l->type == 1) {
                v = vbslq_f32(vceqq_f32(dist, zero), d, vdivq_f32(d, dist));
            } else {
                float32x4_t inv = vdivq_f32(vdupq_n_f32(one), dist);
                float32x4_t c = vmulq_f32(vaddq_f32(vaddq_f32(vmulq_n_f32(t0, l->dx), vmulq_n_f32(t1, l->dy)), vmulq_n_f32(t2, l->dz)), inv);
                ok = vandq_u32(ok, sign_clear(c));
                ok = vandq_u32(ok, vcgtq_u32(vreinterpretq_u32_f32(c), vdupq_n_u32(l->outer_ubits)));
                uint32x4_t in = vcltq_s32(vreinterpretq_s32_f32(c), vdupq_n_s32(l->inner_bits));
                d = vbslq_f32(in, vmulq_f32(d, vmulq_n_f32(vsubq_f32(c, vdupq_n_f32(l->outer)), l->cscale)), d);
                v = vmulq_f32(d, inv);
            }
        } else continue;
        ok = vandq_u32(ok, vcgeq_s32(vreinterpretq_s32_f32(v), vdupq_n_s32(0x322BCC77)));
        v = vbslq_f32(ok, v, zero);
        ar = vaddq_f32(ar, vmulq_n_f32(v, l->cx)); ag = vaddq_f32(ag, vmulq_n_f32(v, l->cy)); ab = vaddq_f32(ab, vmulq_n_f32(v, l->cz));
    }
    uint32x4_t r = vreinterpretq_u32_s32(vcvtnq_s32_f32(vmulq_n_f32(ar, mult)));
    uint32x4_t g = vreinterpretq_u32_s32(vcvtnq_s32_f32(vmulq_n_f32(ag, mult)));
    uint32x4_t b = vreinterpretq_u32_s32(vcvtnq_s32_f32(vmulq_n_f32(ab, mult)));
    uint32x4_t hi = vdupq_n_u32(0xFFFFFF00u), ff = vdupq_n_u32(0xFF);
    r = vbslq_u32(vtstq_u32(r, hi), ff, r); g = vbslq_u32(vtstq_u32(g, hi), ff, g); b = vbslq_u32(vtstq_u32(b, hi), ff, b);
    uint32x4_t c = vorrq_u32(vorrq_u32(vdupq_n_u32(alpha), b), vorrq_u32(vshlq_n_u32(g, 8), vshlq_n_u32(r, 16)));
    vst1q_u32(out, c);
}
static int g_light_exact = -1;
#endif

/* ---- the sector's lights, packed once per render ----
 * A night street sector holds ~800 lights and each PrepareLights call (about
 * 200 per frame) selects ~1.6 of them; reading 800 light structures per object
 * was most of its time. During a render (the driver's `rendering` flag) the
 * lights do not change, so the filter fields are packed once per RenderNumber
 * and the per-object pass reads only this array; the lights that pass are then
 * handled exactly as before, from live memory, in list order. */
#define G_RENDERING  0x101C4D15u
#define G_RENDERNUM  0x100AFC24u
typedef struct { uint32_t light, mode, type; uint8_t on, pzero, wmat, pad; float px, py, pz, range; } lcache_e;
typedef struct {
    uint32_t sector, list, nl, changed, rnum; lcache_e *e; uint32_t cap;
    /* point/spot lights bucketed on the ground plane (x, z) by the square their
     * range covers; everything else is in `always` */
    int gw, gh; float gx0, gz0, cell;
    uint32_t *cstart, *cidx, ccap, icap;     /* cell -> [cstart[c], cstart[c+1]) in cidx */
    uint32_t *always, nalways, acap;
    uint32_t *stamp, scap, gen;
} lcache;
#define LC_GRID 64
#define LC_MARGIN 1.0f
static lcache g_lc[4];
static unsigned g_lc_next;
static void lc_fill(lcache_e *e, uint32_t light) {
    e->light = light;
    e->mode = MEM32(light + 0x150);
    uint8_t f = MEM8(light + 0xAC);
    e->on = f & 1; e->wmat = (f & 0x20) != 0;
    e->pzero = (int32_t)fbits((float)fabs((double)ldf(light + 0x158))) < 0x322BCC77;
    e->type = MEM32(light + 0x154);
    if (e->type == 2) { e->px = ldf(light + 0x1E0); e->py = ldf(light + 0x1E4); e->pz = ldf(light + 0x1E8); e->range = ldf(light + 0x1EC); }
    else if (e->type == 1) { e->px = ldf(light + 0x40); e->py = ldf(light + 0x44); e->pz = ldf(light + 0x48); e->range = ldf(light + 0x1D0); }
}

static int lc_gridable(const lcache_e *e) {
    if (!e->on || e->pzero) return 1;                    /* never selected: in no list at all */
    if (e->type != 1 && e->type != 2) return 0;
    if (e->type == 1 && !e->wmat) return 0;              /* position known only after its matrix update */
    float r = e->range;
    return isfinite(e->px) && isfinite(e->pz) && isfinite(r) && r >= 0.0f && r < 1e6f;
}
static void *grow(void *p, uint32_t *cap, uint32_t need, size_t sz) {
    if (*cap >= need) return p;
    *cap = need + need / 2 + 16;
    return realloc(p, sz * *cap);
}
static void lc_grid(lcache *c) {
    c->gw = 0; c->nalways = 0;
    c->always = grow(c->always, &c->acap, c->nl, 4);
    c->stamp = grow(c->stamp, &c->scap, c->nl, 4);
    memset(c->stamp, 0, 4 * c->nl); c->gen = 0;
    float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    for (uint32_t i = 0; i < c->nl; i++) {
        const lcache_e *e = &c->e[i];
        if (!lc_gridable(e)) { c->always[c->nalways++] = i; continue; }
        if (!e->on || e->pzero) continue;
        float r = e->range + LC_MARGIN;
        if (e->px - r < x0) x0 = e->px - r;
        if (e->px + r > x1) x1 = e->px + r;
        if (e->pz - r < z0) z0 = e->pz - r;
        if (e->pz + r > z1) z1 = e->pz + r;
    }
    if (x1 < x0) return;                                  /* nothing to bucket */
    float span = (x1 - x0 > z1 - z0 ? x1 - x0 : z1 - z0);
    c->cell = span / LC_GRID > 1.0f ? span / LC_GRID : 1.0f;
    c->gx0 = x0; c->gz0 = z0;
    c->gw = (int)((x1 - x0) / c->cell) + 1; c->gh = (int)((z1 - z0) / c->cell) + 1;
    if (c->gw > LC_GRID + 1) c->gw = LC_GRID + 1;
    if (c->gh > LC_GRID + 1) c->gh = LC_GRID + 1;
    uint32_t ncell = (uint32_t)(c->gw * c->gh);
    c->cstart = grow(c->cstart, &c->ccap, ncell + 1, 4);
    memset(c->cstart, 0, 4 * (ncell + 1));
    for (int pass = 0; pass < 2; pass++) {               /* count, then place (counting sort keeps list order) */
        uint32_t total = 0;
        if (pass) { for (uint32_t k = 0; k <= ncell; k++) { uint32_t n = c->cstart[k]; c->cstart[k] = total; total += n; }
                    c->cidx = grow(c->cidx, &c->icap, total, 4); }
        for (uint32_t i = 0; i < c->nl; i++) {
            const lcache_e *e = &c->e[i];
            if (!lc_gridable(e) || !e->on || e->pzero) continue;
            float r = e->range + LC_MARGIN;
            int ax = (int)((e->px - r - c->gx0) / c->cell), bx = (int)((e->px + r - c->gx0) / c->cell);
            int az = (int)((e->pz - r - c->gz0) / c->cell), bz = (int)((e->pz + r - c->gz0) / c->cell);
            if (ax < 0) ax = 0;
            if (az < 0) az = 0;
            if (bx >= c->gw) bx = c->gw - 1;
            if (bz >= c->gh) bz = c->gh - 1;
            for (int z = az; z <= bz; z++)
                for (int x = ax; x <= bx; x++) {
                    uint32_t k = (uint32_t)(z * c->gw + x);
                    if (!pass) c->cstart[k]++; else c->cidx[c->cstart[k]++] = i;
                }
        }
        if (pass) { for (uint32_t k = ncell; k > 0; k--) c->cstart[k] = c->cstart[k - 1]; c->cstart[0] = 0; }
    }
}
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
/* the list positions that can light a sphere at (x, z) with radius r, ascending */
static uint32_t lc_candidates(lcache *c, double x, double z, double r, uint32_t **out) {
    static uint32_t *buf; static uint32_t cap;
    buf = grow(buf, &cap, c->nl + 1, 4);
    uint32_t n = 0;
    if (++c->gen == 0) { memset(c->stamp, 0, 4 * c->nl); c->gen = 1; }
    for (uint32_t k = 0; k < c->nalways; k++) { uint32_t i = c->always[k]; c->stamp[i] = c->gen; buf[n++] = i; }
    if (c->gw && isfinite(x) && isfinite(z) && isfinite(r)) {
        double rr = r + LC_MARGIN;
        int ax = (int)floor((x - rr - c->gx0) / c->cell), bx = (int)floor((x + rr - c->gx0) / c->cell);
        int az = (int)floor((z - rr - c->gz0) / c->cell), bz = (int)floor((z + rr - c->gz0) / c->cell);
        if (ax < 0) ax = 0;
        if (az < 0) az = 0;
        if (bx >= c->gw) bx = c->gw - 1;
        if (bz >= c->gh) bz = c->gh - 1;
        for (int zz = az; zz <= bz; zz++)
            for (int xx = ax; xx <= bx; xx++) {
                uint32_t k = (uint32_t)(zz * c->gw + xx);
                for (uint32_t j = c->cstart[k]; j < c->cstart[k + 1]; j++) {
                    uint32_t i = c->cidx[j];
                    if (c->stamp[i] != c->gen) { c->stamp[i] = c->gen; buf[n++] = i; }
                }
            }
    } else if (c->gw) {                                   /* odd object bounds: every bucketed light */
        for (uint32_t i = 0; i < c->nl; i++) if (c->stamp[i] != c->gen) { c->stamp[i] = c->gen; buf[n++] = i; }
    }
    qsort(buf, n, 4, cmp_u32);
    *out = buf;
    return n;
}
static lcache *lc_get(uint32_t sector, uint32_t list, uint32_t nl) {
    if (!MEM8(G_RENDERING) || !nl) return NULL;
    uint32_t rn = MEM32(G_RENDERNUM), ch = MEM32(sector + 0x140);
    for (int i = 0; i < 4; i++) {
        lcache *c = &g_lc[i];
        if (c->sector == sector && c->list == list && c->nl == nl && c->changed == ch && c->rnum == rn) return c;
    }
    lcache *c = &g_lc[g_lc_next++ & 3];
    if (c->cap < nl) { free(c->e); c->cap = nl + 64; c->e = malloc(sizeof(lcache_e) * c->cap); }
    if (!c->e) { c->cap = 0; return NULL; }
    for (uint32_t i = 0; i < nl; i++) lc_fill(&c->e[i], MEM32(list + 4 * i));
    c->sector = sector; c->list = list; c->nl = nl; c->changed = ch; c->rnum = rn;
    lc_grid(c);
    return c;
}
/* the same sphere distance the exact test computes, from the packed position */
static inline double lc_dist2(const float *b, const lcache_e *e) {
    double dx = (double)b[0] - e->px, dy = (double)b[1] - e->py, dz = (double)b[2] - e->pz;
    return (dz * dz + dy * dy) + dx * dx;
}

void fam_native_prepare_lights(void) {
    uint32_t dst = g_ecx, dst_stride = g_edx;
    uint32_t level = MEM32(g_esp + 4), src = MEM32(g_esp + 8), src_stride = MEM32(g_esp + 12), obj = MEM32(g_esp + 16);
    uint32_t m = MEM32(g_esp + 20), mode = MEM32(g_esp + 24), force = MEM8(g_esp + 28), rec = MEM32(g_esp + 32);
    if (!level) { g_eax &= 0xFFFFFF00u; g_esp += 0x24; return; }
    uint32_t fg = MEM32(level + 0x1C);
    if (!fg) { g_eax = 0; g_esp += 0x24; return; }
    if (!fam_native_light_on || !(MEM8(G_TCGUARD) & 1)                 /* first use registers TempColor */
        || ((MEM8(obj + 0x1D8) & 0x80) && !MEM32(level + 8))) {      /* the average of none divides by 0 */
        recomp_ls3df_10065A70_orig();
        return;
    }
    stf(G_AMB, 0.0f); stf(G_AMB + 4, 0.0f); stf(G_AMB + 8, 0.0f);
    float lightness = ldf(obj + 0x210);
    stf(G_TCOLOR, lightness); stf(G_TCOLOR + 4, lightness); stf(G_TCOLOR + 8, lightness);
    uint32_t mat = MEM32(fg + 4);
    double a = (double)ldf(obj + 0x218) * ldf(obj + 0x214);
    if (mat) a = a * ldf(mat + 0x38);
    float af = (float)(a * 255.0);
    MEM32(G_TALPHA) = (uint32_t)fistp((double)af) << 24;
    uint32_t mf = MEM32(level + 0x3C);
    int keep_diffuse = !(mf & 0x10) || (mf & 0x20);
    if (mat && keep_diffuse)
        for (int k = 0; k < 3; k++) stf(G_TCOLOR + 4 * k, (float)((double)ldf(G_TCOLOR + 4 * k) * ldf(mat + 0x20 + 4 * k)));
    if (!(MEM32(obj + 0x1D8) & 0x40000)) call_std1(0x1005A510u, obj);   /* UpdateWBoundProc */
    float bpos[3] = { ldf(obj + 0x19C), ldf(obj + 0x1A0), ldf(obj + 0x1A4) };
    double brad = ldf(obj + 0x1A8);
    call_std1(0x1002E580u, m);                           /* S_matrix::GetUScale -> st0 */
    float scale = (float)fp_pop_impl(g_st, &g_fp_top);

    uint32_t sector = MEM32(G_SECTOR);
    uint32_t nl = MEM32(sector + 0x14C), list = MEM32(sector + 0x144);
    static int stats = -1; static uint64_t st_calls, st_lights, st_relit, st_verts, st_sel;
    if (stats < 0) { const char *e = getenv("FAMILIA_LIGHT_STATS"); stats = e && atoi(e); }
    if (stats) { st_calls++; st_lights += nl;
                 if (st_calls % 2000 == 0) fprintf(stderr, "[lightstats] per call: %.1f sector lights, %.2f selected, relit %.0f%%, %.0f verts per relight\n",
                     (double)st_lights / st_calls, (double)st_sel / st_calls, 100.0 * st_relit / st_calls, st_relit ? (double)st_verts / st_relit : 0.0); }
    MEM32(G_LCOUNT) = 0;
    uint32_t key = 0;
    uint32_t count = 0;
    lcache *lc = lc_get(sector, list, nl);
    uint32_t *cand = NULL, ncand = nl;
    if (lc) ncand = lc_candidates(lc, bpos[0], bpos[2], brad, &cand);
    for (uint32_t ci = 0; ci < ncand; ci++) {
        uint32_t i = cand ? cand[ci] : ci;
        uint32_t light;
        if (lc) {                                        /* packed filter: reject without touching the light */
            lcache_e *e = &lc->e[i];
            if (!(e->mode & mode) || !e->on || e->pzero) continue;
            light = e->light;
            if (e->type == 1) {
                if (!e->wmat) {                          /* its world matrix first (as the exact path does) */
                    MEM32(G_PTLIGHT) = light;
                    if (!(MEM8(light + 0xAC) & 0x20)) call_this0(0x1001C7D0u, light);
                    lc_fill(e, light);
                }
                double r = brad + e->range;
                if (r * r <= lc_dist2(bpos, e)) continue;
            } else if (e->type == 2) {
                double r = brad + e->range;
                if (r * r <= lc_dist2(bpos, e)) continue;
            } else if (e->type != 3 && e->type != 4) continue;
        } else light = MEM32(list + 4 * i);
        MEM32(G_PTLIGHT) = light;
        if (!(MEM32(light + 0x150) & mode) || !(MEM8(light + 0xAC) & 1)) continue;
        if ((int32_t)fbits((float)fabs((double)ldf(light + 0x158))) < 0x322BCC77) continue;
        uint32_t lt = MEM32(light + 0x154);
        if (lt == 4) {                                   /* ambient */
            double L = lightness;
            double ca = L * ldf(light + 0x190), cb = (double)ldf(light + 0x194) * L;
            float cc = (float)((double)ldf(light + 0x198) * L);
            stf(G_AMB, (float)(ca + ldf(G_AMB)));
            stf(G_AMB + 4, (float)(cb + ldf(G_AMB + 4)));
            stf(G_AMB + 8, (float)((double)cc + ldf(G_AMB + 8)));
            key += MEM32(light + 0x118);
        } else if (lt == 2) {                            /* spot */
            double dist2 = sphere_dist2(bpos, light + 0x1E0);
            double r = brad + ldf(light + 0x1EC);
            if (r * r <= dist2) continue;
            int32_t idx = take_light(light, dist2);
            if (idx < 0) continue;
            uint32_t e = (uint32_t)idx * 0x50;
            light_dir(light, m, e);
            light_pos_range(light, m, e, scale);
        } else if (lt == 1) {                            /* point */
            if (!(MEM8(light + 0xAC) & 0x20)) call_this0(0x1001C7D0u, light);   /* UpdateWMatrixProc */
            double dist2 = sphere_dist2(bpos, light + 0x40);
            double r = brad + ldf(light + 0x1D0);
            if (r * r <= dist2) continue;
            int32_t idx = take_light(light, dist2);
            if (idx < 0) continue;
            light_pos_range(light, m, (uint32_t)idx * 0x50, scale);
        } else if (lt == 3) {                            /* directional */
            stf(G_TENERGY, (float)(((double)ldf(light + 0x198) + ldf(light + 0x194)) + ldf(light + 0x190)));
            int32_t idx = temp_light_index();
            MEM32(G_TLINDEX) = (uint32_t)idx;
            if (idx < 0) continue;
            uint32_t e = (uint32_t)idx * 0x50;
            MEM32(G_TLIST + e + TL_LIGHT) = light;
            MEM32(G_TLIST + e + TL_ENERGY) = MEM32(G_TENERGY);
            light_dir(light, m, e);
            light_color(light, e);
        }
    }
    if (lc) MEM32(G_PTLIGHT) = MEM32(list + 4 * (nl - 1));   /* the loop leaves the last light there */
    count = MEM32(G_LCOUNT);
    if (nl && count) {
        for (uint32_t j = count; j--; ) key += MEM32(MEM32(G_TLIST + 0x50 * j + TL_LIGHT) + 0x118);
        key *= count;
    }
    uint32_t vf = MEM32(obj + 0x1D8);
    uint32_t lod = (vf & 0x10) ? MEM32(obj + 0x1E4) : MEM32(obj + 0x1E4 + 4 * MEM32(obj + 0x20C));
    uint32_t cached = lod + 0xC;
    if (!force || (count == 0 && key > 0)) {
        if (key == MEM32(cached)) { g_eax = cached & 0xFFFFFF00u; g_esp += 0x24; return; }
    }
    MEM32(cached) = key;
    if (mat) {
        if (keep_diffuse)
            for (int k = 0; k < 3; k++) stf(G_AMB + 4 * k, (float)((double)ldf(G_AMB + 4 * k) * ldf(mat + 0x14 + 4 * k)));
        for (int k = 0; k < 3; k++) stf(G_AMB + 4 * k, (float)((double)ldf(G_AMB + 4 * k) + ldf(mat + 0x2C + 4 * k)));
    }

    /* the vertices: the lights as the loop reads them */
    vlight L[8];
    for (uint32_t j = 0; j < count && j < 8; j++) {
        uint32_t t = G_TLIST + 0x50 * j, light = MEM32(t + TL_LIGHT);
        vlight *l = &L[j];
        uint8_t ty = MEM8(light + 0x154);
        l->type = ty == 1 || ty == 2 || ty == 3 ? ty : 0;
        l->px = ldf(t); l->py = ldf(t + 4); l->pz = ldf(t + 8);
        l->dx = ldf(t + 0x10); l->dy = ldf(t + 0x14); l->dz = ldf(t + 0x18);
        l->cx = ldf(t + 0x20); l->cy = ldf(t + 0x24); l->cz = ldf(t + 0x28);
        l->rn_bits = (int32_t)MEM32(t + TL_RN); l->rf = ldf(t + TL_RF);
        l->rf2_bits = (int32_t)MEM32(t + TL_RF2); l->rd = ldf(t + TL_RD);
        l->outer_ubits = MEM32(light + 0x1A0); l->outer = ldf(light + 0x1A0);
        l->inner_bits = (int32_t)MEM32(light + 0x1B0); l->cscale = ldf(light + 0x1C0);
    }
    double amb[3] = { ldf(G_AMB), ldf(G_AMB + 4), ldf(G_AMB + 8) };
    double mult = ldf(G_VLMULT), one = ldf(G_ONE);
    uint32_t alpha = MEM32(G_TALPHA), nv = MEM32(level + 8);
    int n = (int)(count < 8 ? count : 8);
    uint32_t pv = src;
#if defined(__aarch64__)
    if (g_light_exact < 0) { const char *e = getenv("FAMILIA_LIGHT_EXACT"); g_light_exact = e && atoi(e); }
    if (!g_light_exact && nv) {
        flight F[8];
        for (int j = 0; j < n; j++) {
            const vlight *l = &L[j]; flight *f = &F[j];
            f->type = l->type; f->px = (float)l->px; f->py = (float)l->py; f->pz = (float)l->pz;
            f->dx = (float)l->dx; f->dy = (float)l->dy; f->dz = (float)l->dz;
            f->cx = (float)l->cx; f->cy = (float)l->cy; f->cz = (float)l->cz;
            f->rf = (float)l->rf; f->rd = (float)l->rd; f->outer = (float)l->outer; f->cscale = (float)l->cscale;
            f->rn_bits = l->rn_bits; f->rf2_bits = l->rf2_bits; f->inner_bits = l->inner_bits; f->outer_ubits = l->outer_ubits;
        }
        float fa[3] = { (float)amb[0], (float)amb[1], (float)amb[2] };
        uint32_t sr = 0, sg = 0, sb = 0, c4[4];
        int average = (vf & 0x80) != 0;
        for (uint32_t k = 0; k < nv; k += 4) {
            const float *V[4];
            for (int i = 0; i < 4; i++) V[i] = (const float *)ADDR(pv + (k + i < nv ? k + i : nv - 1) * src_stride);
            light4(F, n, V, alpha, fa, (float)mult, (float)one, c4);
            for (int i = 0; i < 4 && k + i < nv; i++) {
                if (average) { sr += (c4[i] >> 16) & 0xFF; sg += (c4[i] >> 8) & 0xFF; sb += c4[i] & 0xFF; }
                else MEM32(dst + (k + i) * dst_stride) = c4[i];
            }
        }
        pv += nv * src_stride;
        if (average) {
            uint32_t rr = sr / nv, gg = sg / nv, bb = sb / nv;
            if (rr > 0xFF) rr = 0xFF;
            if (gg > 0xFF) gg = 0xFF;
            if (bb > 0xFF) bb = 0xFF;
            uint32_t c = alpha | rr << 16 | gg << 8 | bb;
            for (uint32_t k = nv; k; k--, dst += dst_stride) MEM32(dst) = c;
        }
    } else
#endif
    if (vf & 0x80) {                                     /* VISF_AVERAGE_LIGHT */
        uint32_t sr = 0, sg = 0, sb = 0;
        for (uint32_t k = nv; k; k--, pv += src_stride) {
            uint32_t c = light_vertex(L, n, (const float *)ADDR(pv), alpha, amb, mult, one);
            sr += (c >> 16) & 0xFF; sg += (c >> 8) & 0xFF; sb += c & 0xFF;
        }
        uint32_t rr = sr / nv, gg = sg / nv, bb = sb / nv;
        if (rr > 0xFF) rr = 0xFF;
        if (gg > 0xFF) gg = 0xFF;
        if (bb > 0xFF) bb = 0xFF;
        uint32_t c = alpha | rr << 16 | gg << 8 | bb;
        for (uint32_t k = nv; k; k--, dst += dst_stride) MEM32(dst) = c;
    } else {
        for (uint32_t k = nv; k; k--, pv += src_stride, dst += dst_stride)
            MEM32(dst) = light_vertex(L, n, (const float *)ADDR(pv), alpha, amb, mult, one);
    }
    if (stats) { st_relit++; st_verts += nv; st_sel += (uint64_t)n; }
    MEM32(G_PVERT) = pv;
    if (rec) { MEM8(rec + 0x2E) = MEM8(G_TALPHA + 3); MEM8(rec + 0x2D) = 0; }
    g_eax = (rec & 0xFFFFFF00u) | 1;
    g_esp += 0x24;
}

/* ===================================================== occlusion culling */
#define G_RS_OCCL   0x100BFD3Cu   /* renderState[RS_USE_OCCLUSION] */
#define G_POISECT   0x101B43C0u   /* pPOISector */
#define G_RENDERNUM 0x100AFC24u   /* RenderNumber */
/* I3D_occluder */
#define OC_PL2N  0x39C            /* planes (16 bytes) at 0x16C */
#define OC_PL1N  0x5B0            /* planes at 0x3A0 */
#define OC_PLANE 0x5B4            /* the front plane */
#define OC_FLAGS 0x5C4            /* bit 1: active */
#define OC_SPH   0x5CC            /* own sphere: pos, radius 0x5D8 */
#define OC_NEXT  0x5E0
#define OC_SECT  0x5E4
#define OC_DIST  0x5E8

/* whether the sphere (x,y,z, radius r) is inside occluder o's volume */
static int occl_hides(uint32_t o, double x, double y, double z, double r) {
    double d = ((z * ldf(o + OC_PLANE + 8) + y * ldf(o + OC_PLANE + 4)) + x * ldf(o + OC_PLANE)) + ldf(o + OC_PLANE + 12);
    if (!(d > r)) {
        uint32_t n = MEM32(o + OC_PL1N), p = o + 0x3A0;
        for (; n; n--, p += 16) {
            double v = ((z * ldf(p + 8) + y * ldf(p + 4)) + x * ldf(p)) + ldf(p + 12);
            if (v < r) return 0;
        }
    }
    uint32_t n = MEM32(o + OC_PL2N), p = o + 0x16C;
    for (; n; n--, p += 16) {
        double v = ((z * ldf(p + 8) + y * ldf(p + 4)) + x * ldf(p)) + ldf(p + 12);
        if (v < r) return 0;
    }
    return 1;
}

/* CullOcclsEachOther: from the nearest active occluder outwards, each turns
 * off the farther ones inside its volume */
static void cull_occls(uint32_t sct) {
    double A = 0.0;
    for (;;) {
        uint32_t list = MEM32(sct + 0x228), best = 0;
        double B = 999999995904.0;
        for (uint32_t o = list; o; o = MEM32(o + OC_NEXT)) {
            if (!(MEM8(o + OC_FLAGS) & 2)) continue;
            double d = ldf(o + OC_DIST);
            if (!(d > A)) continue;
            if (!(B > d)) continue;
            best = o; B = d;
        }
        float bf = (float)B;
        if (!best) return;
        A = bf;
        for (uint32_t o = list; o; o = MEM32(o + OC_NEXT)) {
            uint32_t f = MEM32(o + OC_FLAGS);
            if (!(f & 2)) continue;
            if (!((double)ldf(o + OC_DIST) > (double)bf)) continue;
            double x = ldf(o + OC_SPH), y = ldf(o + OC_SPH + 4), z = ldf(o + OC_SPH + 8);
            if (occl_hides(best, x, y, z, ldf(o + OC_SPH + 12))) MEM32(o + OC_FLAGS) = f & 0xFFFFFFFDu;
        }
    }
}

/* POIS_EnumProc: the visuals rendered this time that an active occluder of
 * pPOISector hides get the previous render number */
static void pois(uint32_t frm) {
    if (!MEM32(MEM32(G_POISECT) + 0x228) || !frm) return;
    for (; frm; frm = MEM32(frm + 0x130)) {
        uint32_t rn = MEM32(G_RENDERNUM);
        if (MEM32(frm + 0x13C) != rn) continue;
        int vis = 1;
        for (uint32_t o = MEM32(MEM32(G_POISECT) + 0x228); o; o = MEM32(o + OC_NEXT)) {
            if (!vis) break;
            if ((MEM32(o + OC_FLAGS) & 2) != 2) continue;
            if (!(MEM8(frm + 0xAD) & 1)) call_std1(0x1001CF90u, frm);   /* UpdateHRBoundVolW */
            double x = ldf(frm + 0xF0), y = ldf(frm + 0xF4), z = ldf(frm + 0xF8);
            vis = !occl_hides(o, x, y, z, ldf(frm + 0xFC));
        }
        if (!vis) MEM32(frm + 0x13C) = rn - 1;
    }
}

static void occl_off_sector_hulls(uint32_t sct) {
    for (uint32_t o = MEM32(sct + 0x228); o; o = MEM32(o + OC_NEXT)) {
        uint32_t s = MEM32(o + OC_SECT);
        if (s && (MEM8(s + 0x1D0) & 0x80)) MEM32(o + OC_FLAGS) &= 0xFFFFFFFDu;
    }
}

void fam_native_process_occluders(void) {
    if (!fam_native_light_on) { recomp_ls3df_1003C070_orig(); return; }
    uint32_t scene = g_ecx;
    if (MEM32(G_RS_OCCL) && scene) {
        uint32_t n = MEM32(scene + 0x260), s = MEM32(scene + 0x264);
        for (; n; n--, s = MEM32(s + 0x220)) {
            if (!(MEM8(s + 0x1D0) & 1) || !MEM32(s + 0x228)) continue;
            occl_off_sector_hulls(s);
            cull_occls(s);
            MEM32(G_POISECT) = s;
            pois(s);
        }
        uint32_t p = MEM32(scene + 0x210);
        if (MEM8(p + 0x1D0) & 1) {
            occl_off_sector_hulls(p);
            cull_occls(p);
            MEM32(G_POISECT) = p;
            pois(p);
        }
    }
    g_esp += 4;
}
void fam_native_cull_occls(void) {
    if (!fam_native_light_on) { recomp_ls3df_1003C2E0_orig(); return; }
    cull_occls(g_ecx);
    g_esp += 4;
}
void fam_native_pois_enum(void) {
    if (!fam_native_light_on) { recomp_ls3df_1003C480_orig(); return; }
    pois(g_ecx);
    g_esp += 4;
}

/* ================================================== differential check */
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 33) % n; }
static float rf(float lo, float hi) { return lo + (hi - lo) * (float)rnd(1000001) / 1000000.0f; }
static float rfe(float lo, float hi) {               /* with exact zeros and ties now and then */
    uint32_t k = rnd(40);
    if (k == 0) return 0.0f;
    if (k == 1) return -0.0f;
    if (k == 2) return 1.0f;
    return rf(lo, hi);
}

typedef struct { uint8_t *mem; uint32_t va, size; } region;
static void snap(const region *r, int n, uint8_t *buf) { for (int i = 0; i < n; i++) { memcpy(buf, (void *)ADDR(r[i].va), r[i].size); buf += r[i].size; } }
static void restore(const region *r, int n, const uint8_t *buf) { for (int i = 0; i < n; i++) { memcpy((void *)ADDR(r[i].va), buf, r[i].size); buf += r[i].size; } }
static int first_diffs(const char *name, const region *r, int n, const uint8_t *a, const uint8_t *b, int report) {
    int nd = 0;
    for (int i = 0; i < n; i++) {
        for (uint32_t o = 0; o + 4 <= r[i].size; o += 4) {
            uint32_t x, y; memcpy(&x, a + o, 4); memcpy(&y, b + o, 4);
            if (x != y) { if (report && nd < 3) fprintf(stderr, "[native]   %s: %08X: %08X / %08X\n", name, r[i].va + o, x, y); nd++; }
        }
        a += r[i].size; b += r[i].size;
    }
    return nd;
}

#define PL_SIZE 0x5000
static int g_pl_tolerant;
/* fast lighting path: vertex colours within 1 per channel of the original count as equal */
static void tolerate_colours(uint8_t *r1, uint8_t *r2, uint32_t off, uint32_t len) {
    for (uint32_t o = off; o + 4 <= off + len; o += 4) {
        int ok = 1;
        for (int c = 0; c < 4; c++) { int d = (int)r1[o + c] - (int)r2[o + c]; if (d < -1 || d > 1 || (c == 3 && d)) ok = 0; }
        if (ok) memcpy(r2 + o, r1 + o, 4);
    }
}
static int check_prepare_lights(void) {
    uint32_t B = hle_va(hle_heap_alloc(PL_SIZE, 1));
    region R[3] = { { 0, B, PL_SIZE }, { 0, 0x101C4D20u, 0x101C5008u - 0x101C4D20u }, { 0, 0x100A9470u, 16 } };
    uint32_t tot = PL_SIZE + R[1].size + R[2].size;
    uint8_t saved_rendering = MEM8(G_RENDERING); uint32_t saved_rnum = MEM32(G_RENDERNUM);
    uint8_t *saved = malloc(tot), *s0 = malloc(tot), *r1 = malloc(tot), *r2 = malloc(tot);
    snap(&R[1], 2, saved);                               /* the real globals */
    uint32_t level = B, fg = B + 0x40, mat = B + 0x80, obj = B + 0x100, lod = B + 0x400, m = B + 0x480;
    uint32_t sect = B + 0x500, arr = B + 0x700, lights = B + 0x800, src = B + 0x2800, dstb = B + 0x3800;
    int bad = 0, cases = 0, trues = 0;
    for (int n = 0; n < 3000 && bad < 5; n++) {
        memset((void *)ADDR(B), 0, PL_SIZE);
        MEM8(G_TCGUARD) |= 1;
        MEM32(G_SECTOR) = sect;
        stf(G_VLMULT, rnd(2) ? 128.0f : 255.0f); stf(G_ONE, 1.0f);
        MEM32(level + 8) = rnd(30) + (rnd(8) ? 1 : 0);
        MEM32(level + 0x1C) = rnd(30) ? fg : 0;
        MEM32(level + 0x3C) = rnd(64);
        MEM32(fg + 4) = rnd(4) ? mat : 0;
        for (int k = 0; k < 15; k++) stf(mat + 0x14 + 4 * k, rf(0, 1.2f));
        stf(obj + 0x210, rf(0, 2)); stf(obj + 0x214, rf(0, 1)); stf(obj + 0x218, rf(0, 1));
        MEM32(obj + 0x1D8) = 0x40000 | (rnd(2) ? 0x10 : 0) | (rnd(3) ? 0 : 0x80);
        if (!MEM32(level + 8)) MEM32(obj + 0x1D8) &= ~0x80u;
        stf(obj + 0x19C, rf(-30, 30)); stf(obj + 0x1A0, rf(-30, 30)); stf(obj + 0x1A4, rf(-30, 30)); stf(obj + 0x1A8, rf(0, 10));
        for (int k = 0; k < 10; k++) MEM32(obj + 0x1E4 + 4 * k) = lod + 0x0;
        MEM32(obj + 0x20C) = rnd(10);
        MEM32(lod + 0xC) = rnd(4) ? rnd(100) : 0;
        for (int k = 0; k < 16; k++) stf(m + 4 * k, rf(-2, 2));
        uint32_t nl = rnd(14);
        MEM32(sect + 0x144) = arr; MEM32(sect + 0x14C) = nl;
        for (uint32_t i = 0; i < nl; i++) {
            uint32_t l = lights + 0x200 * (i % 10);
            MEM32(arr + 4 * i) = l;
            if (i >= 10) continue;
            MEM32(l + 0x150) = rnd(4);
            MEM32(l + 0xAC) = (rnd(6) ? 1 : 0) | (rnd(4) ? 0x20 : 0);
            stf(l + 0x158, rnd(10) ? rf(-2, 2) : 0.0f);
            MEM32(l + 0x154) = rnd(10) ? 1 + rnd(4) : rnd(8);
            for (int k = 0; k < 3; k++) { stf(l + 0x190 + 4 * k, rf(0, 2)); stf(l + 0x178 + 4 * k, rf(-40, 40)); stf(l + 0x184 + 4 * k, rfe(-1, 1));
                                          stf(l + 0x1E0 + 4 * k, rf(-40, 40)); stf(l + 0x40 + 4 * k, rf(-40, 40)); }
            for (int k = 0; k < 12; k++) stf(l + 0x50 + 4 * k, rf(-1, 1));       /* local matrix */
            stf(l + 0x7C, 1.0f); stf(l + 0x90, 1.0f); stf(l + 0x94, 1.0f); stf(l + 0x98, 1.0f);
            stf(l + 0x19C, rf(0, 20)); stf(l + 0x1D0, rnd(8) ? rf(0, 40) : 0.0f); stf(l + 0x1EC, rf(0, 30));
            stf(l + 0x1A0, rf(-0.2f, 1)); stf(l + 0x1B0, rf(0, 1)); stf(l + 0x1C0, rf(0, 5));
            MEM32(l + 0x118) = rnd(1000);
        }
        uint32_t stride = rnd(2) ? 24 : 32;
        for (uint32_t v = 0; v < 32; v++)
            for (int k = 0; k < 6; k++) stf(src + v * stride + 4 * k, k < 3 ? rf(-40, 40) : rfe(-1, 1));
        uint32_t args[8] = { level, src, stride, obj, m, rnd(4) | (rnd(2) ? 0xC : 0), rnd(2), rnd(3) ? lod : 0 };
        uint32_t dst_stride = 4 * (1 + rnd(4));
        MEM8(G_RENDERING) = (uint8_t)(rnd(3) != 0);      /* mostly the packed-light path */
        MEM32(G_RENDERNUM) = 0x7F000000u + (uint32_t)n;
        snap(R, 3, s0);
        uint32_t eaxs[2], esps[2], stb[2];
        for (int pass = 0; pass < 2; pass++) {
            restore(R, 3, s0);
            uint32_t esp0 = g_esp;
            for (int i = 7; i >= 0; i--) PUSH32(g_esp, args[i]);
            PUSH32(g_esp, RECOMP_RETADDR);
            g_ecx = dstb; g_edx = dst_stride; g_eax = 0x12345678u;
            int st0 = g_st_base;
            (pass ? fam_native_prepare_lights : recomp_ls3df_10065A70_orig)();
            eaxs[pass] = g_eax; esps[pass] = g_esp - esp0; stb[pass] = (uint32_t)(g_st_base - st0);
            g_esp = esp0;
            snap(R, 3, pass ? r2 : r1);
        }
        cases++; trues += (eaxs[0] & 0xFF) != 0;
        if (g_pl_tolerant) tolerate_colours(r1, r2, dstb - B, PL_SIZE - (dstb - B));
        int nd = first_diffs("PrepareLights", R, 3, r1, r2, 0);
        if (nd || eaxs[0] != eaxs[1] || esps[0] != esps[1] || stb[0] || stb[1]) {
            if (!bad) {
                fprintf(stderr, "[native] PrepareLights MISMATCH (case %d): %d words, eax %08X/%08X esp %d/%d x87 %d/%d\n",
                        n, nd, eaxs[0], eaxs[1], (int)esps[0], (int)esps[1], (int)stb[0], (int)stb[1]);
                first_diffs("PrepareLights", R, 3, r1, r2, 1);
            }
            bad++;
        }
    }
    restore(&R[1], 2, saved);
    fprintf(stderr, "[native] PrepareLights: %s against the translated original (%d random cases, %d lit)\n",
            bad ? "DIFFERS" : "matches", cases, trues);
    MEM8(G_RENDERING) = saved_rendering; MEM32(G_RENDERNUM) = saved_rnum;
    free(saved); free(s0); free(r1); free(r2);
    hle_heap_free(hle_ptr(B));
    return bad;
}

#define OC_SIZE 0x9000
static int check_occluders(void) {
    uint32_t B = hle_va(hle_heap_alloc(OC_SIZE, 1));
    region R[2] = { { 0, B, OC_SIZE }, { 0, G_POISECT, 4 } };
    region G[2] = { { 0, G_RS_OCCL, 4 }, { 0, G_RENDERNUM, 4 } };
    uint32_t tot = OC_SIZE + 4;
    uint8_t *saved = malloc(16), *s0 = malloc(tot), *r1 = malloc(tot), *r2 = malloc(tot);
    uint32_t savedpois = MEM32(G_POISECT);
    snap(G, 2, saved);
    uint32_t scene = B, sects = B + 0x300, occl = B + 0xA00, vis = B + 0x3A00;   /* 3 sectors 0x240, 8 occluders 0x600, 32 visuals 0x140 */
    int bad = 0, cases = 0, hidden = 0;
    for (int n = 0; n < 3000 && bad < 5; n++) {
        memset((void *)ADDR(B), 0, OC_SIZE);
        MEM32(G_RS_OCCL) = rnd(10) ? 1 : 0;
        uint32_t RN = 100 + rnd(5);
        MEM32(G_RENDERNUM) = RN;
        MEM32(scene + 0x260) = rnd(3);
        MEM32(scene + 0x264) = sects + 0x240;
        MEM32(scene + 0x210) = sects;
        for (int s = 0; s < 3; s++) {
            uint32_t S = sects + 0x240 * s;
            MEM32(S + 0x1D0) = (rnd(5) ? 1 : 0) | (rnd(4) ? 0 : 0x80);
            MEM32(S + 0x220) = sects + 0x240 * (s == 2 ? 1 : 2);
            MEM32(S + 0x13C) = RN;
        }
        for (int i = 0; i < 8; i++) {                    /* occluders into random sectors */
            uint32_t o = occl + 0x600 * i;
            if (!rnd(6)) continue;
            uint32_t S = sects + 0x240 * rnd(3);
            MEM32(o + OC_NEXT) = MEM32(S + 0x228); MEM32(S + 0x228) = o;
            MEM32(o + OC_FLAGS) = rnd(5) ? 2 : rnd(8);
            MEM32(o + OC_SECT) = rnd(3) ? 0 : sects + 0x240 * rnd(3);
            stf(o + OC_DIST, rnd(10) ? rf(0, 50) : 10.0f);
            for (int k = 0; k < 3; k++) stf(o + OC_SPH + 4 * k, rf(-20, 20));
            stf(o + OC_SPH + 12, rf(0, 5));
            for (int k = 0; k < 4; k++) stf(o + OC_PLANE + 4 * k, k < 3 ? rf(-1, 1) : rf(-10, 30));
            MEM32(o + OC_PL1N) = rnd(5); MEM32(o + OC_PL2N) = rnd(5);
            for (int k = 0; k < 4 * 33; k++) stf(o + 0x3A0 + 4 * k, (k & 3) == 3 ? rf(0, 40) : rf(-1, 1));
            for (int k = 0; k < 4 * 35; k++) stf(o + 0x16C + 4 * k, (k & 3) == 3 ? rf(0, 40) : rf(-1, 1));
        }
        for (int i = 0; i < 32; i++) {                   /* visuals hung on the sectors' visual lists */
            uint32_t v = vis + 0x140 * i, S = sects + 0x240 * rnd(3);
            MEM32(v + 0x130) = MEM32(S + 0x130); MEM32(S + 0x130) = v;
            MEM32(v + 0x13C) = rnd(4) ? RN : RN - 1;
            MEM32(v + 0xAC) = 0x100;
            for (int k = 0; k < 3; k++) stf(v + 0xF0 + 4 * k, rf(-20, 20));
            stf(v + 0xFC, rf(0, 4));
        }
        int which = rnd(3);
        uint32_t arg = which == 0 ? (rnd(20) ? scene : 0) : sects + 0x240 * rnd(3);
        if (which == 2) MEM32(G_POISECT) = sects + 0x240 * rnd(3);
        snap(R, 2, s0);
        uint32_t esps[2], stb[2];
        for (int pass = 0; pass < 2; pass++) {
            restore(R, 2, s0);
            uint32_t esp0 = g_esp;
            PUSH32(g_esp, RECOMP_RETADDR);
            g_ecx = arg;
            int st0 = g_st_base;
            void (*f)(void) = which == 0 ? (pass ? fam_native_process_occluders : recomp_ls3df_1003C070_orig)
                            : which == 1 ? (pass ? fam_native_cull_occls : recomp_ls3df_1003C2E0_orig)
                                         : (pass ? fam_native_pois_enum : recomp_ls3df_1003C480_orig);
            f();
            esps[pass] = g_esp - esp0; stb[pass] = (uint32_t)(g_st_base - st0);
            g_esp = esp0;
            snap(R, 2, pass ? r2 : r1);
        }
        cases++;
        for (int i = 0; i < 32; i++) { uint32_t o = (vis - B) + 0x140 * i + 0x13C, a, b; memcpy(&a, s0 + o, 4); memcpy(&b, r1 + o, 4); hidden += a != b; }
        int nd = first_diffs("occluders", R, 2, r1, r2, 0);
        if (nd || esps[0] != esps[1] || stb[0] || stb[1]) {
            if (!bad) {
                fprintf(stderr, "[native] %s MISMATCH (case %d): %d words, esp %d/%d x87 %d/%d\n",
                        which == 0 ? "ProcessOccluders" : which == 1 ? "CullOcclsEachOther" : "POIS_EnumProc",
                        n, nd, (int)esps[0], (int)esps[1], (int)stb[0], (int)stb[1]);
                first_diffs("occluders", R, 2, r1, r2, 1);
            }
            bad++;
        }
    }
    restore(G, 2, saved);
    MEM32(G_POISECT) = savedpois;
    fprintf(stderr, "[native] ProcessOccluders/CullOcclsEachOther/POIS_EnumProc: %s against the translated originals (%d random cases, %d visuals hidden)\n",
            bad ? "DIFFER" : "match", cases, hidden);
    free(saved); free(s0); free(r1); free(r2);
    hle_heap_free(hle_ptr(B));
    return bad;
}

int fam_native_light_check(void) {
#if defined(__aarch64__)
    int mode = g_light_exact < 0 ? ({ const char *e = getenv("FAMILIA_LIGHT_EXACT"); e && atoi(e); }) : g_light_exact;
    g_light_exact = 1; g_pl_tolerant = 0;                /* the double path, bit for bit */
    int bad = check_prepare_lights();
    if (!mode) {                                         /* the NEON path: colours within 1 */
        g_light_exact = 0; g_pl_tolerant = 1;
        fprintf(stderr, "[native] PrepareLights fast path (single precision, colours within 1):\n");
        bad += check_prepare_lights();
        g_pl_tolerant = 0;
    }
    g_light_exact = mode;
    bad += check_occluders();
#else
    int bad = check_prepare_lights() + check_occluders();
#endif
    if (bad) { fam_native_light_on = 0; fprintf(stderr, "[native] lighting/occlusion natives disabled\n"); }
    return 0;
}
