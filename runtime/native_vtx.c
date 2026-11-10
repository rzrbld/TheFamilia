/*
 * native_vtx.c - native replacements for LS3D's software vertex processing:
 * TransformVertices (LS3DF 0x10068360), TransformVerticesBlend (0x10068430)
 * and EnvProcess (0x10067D20, environment-map texture coordinates).
 *
 * Written from the x86 of LS3DF.dll 1.2 (build/logs/<VA>.asm), with the
 * mafia-re decompilation used only as a map of names. The translated original
 * keeps x87 registers as C doubles and rounds to float only where the code
 * stores a dword; this file does the same, operation for operation (built with
 * -ffp-contract=off). FAMILIA_NATIVE_CHECK=1 compares each against its
 * translated original on random input (fam_native_vtx_check); a mismatch
 * switches only these three back to the translated code.
 *
 * Conventions (from the asm): all three take the destination in ecx and the
 * second pointer in edx, the rest on the stack, and pop their stack arguments.
 *   TransformVertices(dst=ecx, src=edx; count, m, mn)              ret 0xC
 *   TransformVerticesBlend(dst=ecx, src=edx; count, m1, mn1,
 *                          weights, m2, mn2)                       ret 0x18
 *   EnvProcess(dst=ecx, dst_stride=edx; src, src_stride, count, cam_pos,
 *              m, scale, u_ofs, v_ofs, mode, frm)                  ret 0x28
 * Source vertices: position (12 bytes) then normal (12); 0x20 bytes apart in
 * the transforms, which write 0x18-byte position+normal records.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

int fam_native_vtx_on = 1;

void recomp_ls3df_10068360_orig(void);
void recomp_ls3df_10068430_orig(void);
void recomp_ls3df_10067D20_orig(void);
void fam_vmm(const float *V, const float *M, float *R);   /* S_vector * S_matrix, bit-exact (native_math.c) */

static inline double ld(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return (double)f; }
static inline void st(uint32_t va, double d) { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }
static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* position * m: ((x*m0 + m12) + y*m4) + z*m8 per column */
static inline double xf_pos(uint32_t p, uint32_t m, int j) {
    return ((ld(p) * ld(m + 4 * j) + ld(m + 0x30 + 4 * j)) + ld(p + 4) * ld(m + 0x10 + 4 * j)) + ld(p + 8) * ld(m + 0x20 + 4 * j);
}
/* normal * m (rotation only): (x*m0 + y*m4) + z*m8 */
static inline double xf_dir(uint32_t n, uint32_t m, int j) {
    return (ld(n) * ld(m + 4 * j) + ld(n + 4) * ld(m + 0x10 + 4 * j)) + ld(n + 8) * ld(m + 0x20 + 4 * j);
}

/* ------------------------------------------------------ TransformVertices */
void fam_native_transform_vertices(void) {
    uint32_t count = MEM32(g_esp + 4), m = MEM32(g_esp + 8), mn = MEM32(g_esp + 12);
    if (!fam_native_vtx_on || !count) { recomp_ls3df_10068360_orig(); return; }
    uint32_t dst = g_ecx, src = g_edx;
    for (uint32_t i = 0; i < count; i++, dst += 0x18, src += 0x20) {
        double p0 = xf_pos(src, m, 0), p1 = xf_pos(src, m, 1), p2 = xf_pos(src, m, 2);
        st(dst, p0); st(dst + 4, p1); st(dst + 8, p2);
        double n0 = xf_dir(src + 12, mn, 0), n1 = xf_dir(src + 12, mn, 1), n2 = xf_dir(src + 12, mn, 2);
        st(dst + 12, n0); st(dst + 16, n1); st(dst + 20, n2);
    }
    g_eax = mn; g_ecx = 0; g_edx = m;
    g_esp += 4 + 12;
}

/* ------------------------------------------------- TransformVerticesBlend */
/* two transforms blended by the vertex's weight w: a*w + b*(1 - w) */
void fam_native_transform_vertices_blend(void) {
    uint32_t count = MEM32(g_esp + 4), m1 = MEM32(g_esp + 8), mn1 = MEM32(g_esp + 12);
    uint32_t wts = MEM32(g_esp + 16), m2 = MEM32(g_esp + 20), mn2 = MEM32(g_esp + 24);
    if (!fam_native_vtx_on || !count) { recomp_ls3df_10068430_orig(); return; }
    uint32_t dst = g_ecx, src = g_edx;
    const double one = ld(0x100A947Cu);                       /* the engine's 1.0f */
    for (uint32_t i = 0; i < count; i++, dst += 0x18, src += 0x20, wts += 4) {
        double a[3], b[3];
        for (int j = 0; j < 3; j++) a[j] = xf_pos(src, m1, j);
        for (int j = 0; j < 3; j++) b[j] = xf_pos(src, m2, j);
        double w = ld(wts), iw = one - w;
        for (int j = 0; j < 3; j++) st(dst + 4 * j, a[j] * w + b[j] * iw);
        for (int j = 0; j < 3; j++) a[j] = xf_dir(src + 12, mn1, j);
        for (int j = 0; j < 3; j++) b[j] = xf_dir(src + 12, mn2, j);
        w = ld(wts); iw = one - w;
        for (int j = 0; j < 3; j++) st(dst + 12 + 4 * j, a[j] * w + b[j] * iw);
    }
    g_eax = wts; g_ecx = 0; g_edx = mn2;
    g_esp += 4 + 24;
}

/* ------------------------------------------------------------- EnvProcess */
/* guest calls (stdcall/thiscall into translated or hooked code) */
static uint32_t guest_call(uint32_t va) {
    recomp_func_t f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup(va);
    f();
    return g_eax;
}

/* Modes 0 and 1: the view vector from the camera reflected about the normal,
 * normalized, projected into a sphere map around +z (mode 0) or +y (mode 1).
 * Returns whether this vertex went through mode 1's "degenerate" exit, after
 * which the original continues in mode 0's loop. */
static int env_reflect(uint32_t src, uint32_t dst, uint32_t cam, uint32_t m, double scale, double uofs, double vofs, int mode) {
    double cx = ld(cam), cy = ld(cam + 4), cz = ld(cam + 8);
    double dx = cx - xf_pos(src, m, 0);
    double dy = cy - xf_pos(src, m, 1);
    double dz = cz - xf_pos(src, m, 2);
    double N0 = xf_dir(src + 12, m, 0);
    float n0f = (float)N0;
    double s = N0 * dx;
    double N1 = xf_dir(src + 12, m, 1);
    float n1f = (float)N1;
    s = s + N1 * dy;
    double N2 = xf_dir(src + 12, m, 2);
    float n2f = (float)N2;
    s = s + N2 * dz;
    double D = s * ld(0x100A9474u);                              /* 2.0f */
    double r0 = (double)n0f * D - dx;
    double r1 = (double)n1f * D - dy;
    double r2 = (double)n2f * D - dz;
    double L = sqrt((r0 * r0 + r1 * r1) + r2 * r2);
    double one = ld(0x100A947Cu), half = ld(0x100A9478u);
    double inv = one / L;
    r0 *= inv; r1 *= inv; r2 *= inv;
    double w = (mode == 0 ? r2 : r1) + one;
    int32_t wbits = (int32_t)fbits((float)w);
    if (wbits < 0x322BCC77) {                                    /* the sphere map's pole: its centre */
        st(dst, half + uofs);
        st(dst + 4, half + vofs);
        return mode == 1;
    }
    double k = sqrt(w) * scale;
    double ik = one / k;
    if (mode == 0) {
        st(dst, (r0 * ik + half) + uofs);
        st(dst + 4, (r1 * -ik + half) + vofs);
    } else {
        st(dst, (r0 * ik + half) + uofs);
        st(dst + 4, (r2 * ik + half) + vofs);
    }
    return 0;
}

/* ---- modes 0 and 1 in single precision, four vertices at a time ----------
 * The same steps as env_reflect in float (the game itself ran the x87 in
 * single precision: Direct3D sets it so), with NEON doing four vertices per
 * step. Results differ from the double-precision path in the last bits of the
 * texture coordinates (far below a texel), except right at the sphere map's
 * pole, where a vertex can fall on the other side of the test. Mode 1's quirk
 * (after a vertex at the pole, the rest are done as mode 0) is kept: a group
 * with such a vertex is done one vertex at a time. FAMILIA_ENV_EXACT=1 selects
 * the double-precision path. */
static int g_env_exact = -1;
typedef struct { float M[16], cx, cy, cz, scale, uofs, vofs; } env_ctx;
static int env_one_f(const env_ctx *c, uint32_t src, uint32_t dst, int mode) {
    const float *P = (const float *)ADDR(src), *M = c->M;
    float px = P[0], py = P[1], pz = P[2], nx = P[3], ny = P[4], nz = P[5];
    float dx = c->cx - (((px * M[0] + M[12]) + py * M[4]) + pz * M[8]);
    float dy = c->cy - (((px * M[1] + M[13]) + py * M[5]) + pz * M[9]);
    float dz = c->cz - (((px * M[2] + M[14]) + py * M[6]) + pz * M[10]);
    float n0 = (nx * M[0] + ny * M[4]) + nz * M[8];
    float n1 = (nx * M[1] + ny * M[5]) + nz * M[9];
    float n2 = (nx * M[2] + ny * M[6]) + nz * M[10];
    float D = ((n0 * dx + n1 * dy) + n2 * dz) * 2.0f;
    float r0 = n0 * D - dx, r1 = n1 * D - dy, r2 = n2 * D - dz;
    float inv = 1.0f / sqrtf((r0 * r0 + r1 * r1) + r2 * r2);
    r0 *= inv; r1 *= inv; r2 *= inv;
    float w = (mode == 0 ? r2 : r1) + 1.0f;
    float *out = (float *)ADDR(dst);
    if ((int32_t)fbits(w) < 0x322BCC77) { out[0] = 0.5f + c->uofs; out[1] = 0.5f + c->vofs; return mode == 1; }
    float ik = 1.0f / (sqrtf(w) * c->scale);
    out[0] = (r0 * ik + 0.5f) + c->uofs;
    out[1] = (mode == 0 ? r1 * -ik : r2 * ik) + 0.5f + c->vofs;
    return 0;
}
#if defined(__aarch64__)
#include <arm_neon.h>
static void env_reflect_fast(const env_ctx *c, uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride, uint32_t count, int mode) {
    const float *M = c->M;
    const float32x4_t m0 = vdupq_n_f32(M[0]), m1 = vdupq_n_f32(M[1]), m2 = vdupq_n_f32(M[2]);
    const float32x4_t m4 = vdupq_n_f32(M[4]), m5 = vdupq_n_f32(M[5]), m6 = vdupq_n_f32(M[6]);
    const float32x4_t m8 = vdupq_n_f32(M[8]), m9 = vdupq_n_f32(M[9]), m10 = vdupq_n_f32(M[10]);
    const float32x4_t m12 = vdupq_n_f32(M[12]), m13 = vdupq_n_f32(M[13]), m14 = vdupq_n_f32(M[14]);
    const float32x4_t cx = vdupq_n_f32(c->cx), cy = vdupq_n_f32(c->cy), cz = vdupq_n_f32(c->cz);
    const float32x4_t one = vdupq_n_f32(1.0f), two = vdupq_n_f32(2.0f), half = vdupq_n_f32(0.5f);
    const float32x4_t sc = vdupq_n_f32(c->scale), uo = vdupq_n_f32(c->uofs), vo = vdupq_n_f32(c->vofs);
    const float pu = 0.5f + c->uofs, pv = 0.5f + c->vofs;
    uint32_t i = 0;
    for (; i + 4 <= count; i += 4) {
        float32x4_t px, py, pz, nx, ny, nz;
        float t[6][4];
        for (int l = 0; l < 4; l++) {
            const float *P = (const float *)ADDR(src + (i + (uint32_t)l) * sstride);
            if (l == 0) __builtin_prefetch((const char *)ADDR(src + (i + 8) * sstride));
            t[0][l] = P[0]; t[1][l] = P[1]; t[2][l] = P[2]; t[3][l] = P[3]; t[4][l] = P[4]; t[5][l] = P[5];
        }
        px = vld1q_f32(t[0]); py = vld1q_f32(t[1]); pz = vld1q_f32(t[2]);
        nx = vld1q_f32(t[3]); ny = vld1q_f32(t[4]); nz = vld1q_f32(t[5]);
        float32x4_t dx = vsubq_f32(cx, vaddq_f32(vaddq_f32(vaddq_f32(vmulq_f32(px, m0), m12), vmulq_f32(py, m4)), vmulq_f32(pz, m8)));
        float32x4_t dy = vsubq_f32(cy, vaddq_f32(vaddq_f32(vaddq_f32(vmulq_f32(px, m1), m13), vmulq_f32(py, m5)), vmulq_f32(pz, m9)));
        float32x4_t dz = vsubq_f32(cz, vaddq_f32(vaddq_f32(vaddq_f32(vmulq_f32(px, m2), m14), vmulq_f32(py, m6)), vmulq_f32(pz, m10)));
        float32x4_t n0 = vaddq_f32(vaddq_f32(vmulq_f32(nx, m0), vmulq_f32(ny, m4)), vmulq_f32(nz, m8));
        float32x4_t n1 = vaddq_f32(vaddq_f32(vmulq_f32(nx, m1), vmulq_f32(ny, m5)), vmulq_f32(nz, m9));
        float32x4_t n2 = vaddq_f32(vaddq_f32(vmulq_f32(nx, m2), vmulq_f32(ny, m6)), vmulq_f32(nz, m10));
        float32x4_t D = vmulq_f32(vaddq_f32(vaddq_f32(vmulq_f32(n0, dx), vmulq_f32(n1, dy)), vmulq_f32(n2, dz)), two);
        float32x4_t r0 = vsubq_f32(vmulq_f32(n0, D), dx), r1 = vsubq_f32(vmulq_f32(n1, D), dy), r2 = vsubq_f32(vmulq_f32(n2, D), dz);
        float32x4_t inv = vdivq_f32(one, vsqrtq_f32(vaddq_f32(vaddq_f32(vmulq_f32(r0, r0), vmulq_f32(r1, r1)), vmulq_f32(r2, r2))));
        r0 = vmulq_f32(r0, inv); r1 = vmulq_f32(r1, inv); r2 = vmulq_f32(r2, inv);
        float32x4_t w = vaddq_f32(mode == 0 ? r2 : r1, one);
        uint32x4_t pole = vcltq_s32(vreinterpretq_s32_f32(w), vdupq_n_s32(0x322BCC77));
        if (mode == 1 && vmaxvq_u32(pole)) {                     /* mode 1's switch: one at a time from here */
            for (uint32_t k = i; k < count; k++)
                if (env_one_f(c, src + k * sstride, dst + k * dstride, mode)) mode = 0;
            return;
        }
        float32x4_t ik = vdivq_f32(one, vmulq_f32(vsqrtq_f32(vabsq_f32(w)), sc));
        float32x4_t u = vaddq_f32(vaddq_f32(vmulq_f32(r0, ik), half), uo);
        float32x4_t v = vaddq_f32(vaddq_f32(mode == 0 ? vmulq_f32(r1, vnegq_f32(ik)) : vmulq_f32(r2, ik), half), vo);
        u = vbslq_f32(pole, vdupq_n_f32(pu), u);
        v = vbslq_f32(pole, vdupq_n_f32(pv), v);
        float uu[4], vv[4]; vst1q_f32(uu, u); vst1q_f32(vv, v);
        for (int l = 0; l < 4; l++) { float *o = (float *)ADDR(dst + (i + (uint32_t)l) * dstride); o[0] = uu[l]; o[1] = vv[l]; }
    }
    for (; i < count; i++)
        if (env_one_f(c, src + i * sstride, dst + i * dstride, mode)) mode = 0;
}
#else
static void env_reflect_fast(const env_ctx *c, uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride, uint32_t count, int mode) {
    for (uint32_t i = 0; i < count; i++)
        if (env_one_f(c, src + i * sstride, dst + i * dstride, mode)) mode = 0;
}
#endif
static void env_ctx_load(env_ctx *c, uint32_t cam, uint32_t m, uint32_t a) {
    memcpy(c->M, (const void *)ADDR(m), 64);
    const float *C = (const float *)ADDR(cam); c->cx = C[0]; c->cy = C[1]; c->cz = C[2];
    const float *A = (const float *)ADDR(a + 24); c->scale = A[0]; c->uofs = A[1]; c->vofs = A[2];
}

void fam_native_env_process(void) {
    uint32_t a = g_esp;
    uint32_t src = MEM32(a + 4), sstride = MEM32(a + 8), count = MEM32(a + 12), cam = MEM32(a + 16);
    uint32_t m = MEM32(a + 20), mode = MEM32(a + 36), frm = MEM32(a + 40);
    double scale = ld(a + 24), uofs = ld(a + 28), vofs = ld(a + 32);
    uint32_t dst = g_ecx, dstride = g_edx;
    if (!fam_native_vtx_on || (mode <= 1 && !count)) { recomp_ls3df_10067D20_orig(); return; }
    switch (mode) {
    case 0: case 1: {
        if (g_env_exact < 0) { const char *e = getenv("FAMILIA_ENV_EXACT"); g_env_exact = e && atoi(e); }
        if (!g_env_exact) {
            env_ctx c; env_ctx_load(&c, cam, m, a);
            env_reflect_fast(&c, src, sstride, dst, dstride, count, (int)mode);
            break;
        }
        int md = (int)mode;
        for (uint32_t i = 0; i < count; i++, dst += dstride, src += sstride)
            if (env_reflect(src, dst, cam, m, scale, uofs, vofs, md)) md = 0;   /* mode 1's exit jumps into mode 0's loop */
        break;
    }
    case 2: case 3: case 4: {
        /* planar mapping: two coordinates of the transformed position, scaled */
        float f = (float)((ld(0x100BFD98u) * scale) * ld(0x1009D428u));
        int ia = mode == 3 ? 1 : 0, ib = mode == 4 ? 1 : 2;
        for (uint32_t i = 0; i < count; i++, dst += dstride, src += sstride) {
            float r[3];
            fam_vmm((const float *)ADDR(src), (const float *)ADDR(m), r);
            st(dst, (double)r[ia] * (double)f);
            st(dst + 4, (double)r[ib] * (double)f);
        }
        break;
    }
    case 5: {
        /* the position in the frame's space (m * frm's inverse), clamped to [0,1] */
        if (!frm) break;
        g_ecx = frm; PUSH32(g_esp, RECOMP_RETADDR);
        uint32_t inv = guest_call(0x10068840u);                   /* I3D_dummy::GetInvMatrix, thiscall */
        g_esp -= 64; uint32_t mm = g_esp;                          /* a matrix on the guest stack */
        PUSH32(g_esp, inv); PUSH32(g_esp, mm); PUSH32(g_esp, m); PUSH32(g_esp, RECOMP_RETADDR);
        guest_call(0x1002D3A0u);                                   /* S_matrix::operator*, pops its 3 */
        float mmh[16]; memcpy(mmh, (const void *)ADDR(mm), 64);
        g_esp += 64;
        const double zero = ld(0x1009B310u), one = ld(0x1009B304u);
        for (uint32_t i = 0; i < count; i++, dst += dstride, src += sstride) {
            float r[3];
            fam_vmm((const float *)ADDR(src), mmh, r);
            double z = r[2], y = r[1];
            if (z < zero) MEM32(dst) = 0;
            else if (z > one) MEM32(dst) = 0x3F800000u;
            else MEM32(dst) = fbits(r[2]);
            if (y < zero) MEM32(dst + 4) = 0x3F800000u;
            else if (y > one) MEM32(dst + 4) = 0;
            else st(dst + 4, one - y);
        }
        break;
    }
    default: break;
    }
    g_esp += 4 + 0x28;
}

/* ---------------------------------------------------- differential check */
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 33) % n; }
static float rndf(void) {
    if (!rnd(40)) return rnd(2) ? 0.0f : -0.0f;
    float f = (float)((double)(int32_t)(rnd(0x7FFFFFFF) - 0x3FFFFFFF) / 1073741824.0);
    return ldexpf(f, (int)rnd(12) - 4);
}
static void wf(uint32_t va, float f) { MEM32(va) = fbits(f); }

#define AREA 0x4000                         /* dst area per copy */
static int report(const char *name, int n, uint32_t A, uint32_t B, int ok1, int ok2, const char *extra) {
    int shown = 0, nd = 0;
    for (uint32_t i = 0; i < AREA; i += 4) if (MEM32(A + i) != MEM32(B + i)) nd++;
    fprintf(stderr, "[native] %s MISMATCH (case %d): %d words differ, esp %d/%d %s\n", name, n, nd, ok1, ok2, extra);
    for (uint32_t i = 0; i < AREA && shown < 3; i += 4)
        if (MEM32(A + i) != MEM32(B + i)) { fprintf(stderr, "    +%04X: %08X / %08X\n", i, MEM32(A + i), MEM32(B + i)); shown++; }
    return 1;
}

/* The single-precision EnvProcess against the double-precision one on
 * realistic input (rotation + translation matrices, unit normals, cameras
 * metres away): largest difference of the texture coordinates, leaving out
 * the vertices right at the pole (|u| or |v| beyond 4: there the coordinates
 * swing over many texture widths in any precision) and pole-test flips. */
static float urnd(float lo, float hi) { return lo + (hi - lo) * (float)rnd(1000001) / 1000000.0f; }
static int env_fast_check(uint32_t A, uint32_t B, uint32_t in) {
    uint32_t m = in, cam = in + 0x40, src = in + 0x600;
    double maxd = 0; int flips = 0, nv = 0, near = 0;
    for (int n = 0; n < 3000; n++) {
        float q[4]; double ql = 0; for (int k = 0; k < 4; k++) { q[k] = urnd(-1, 1); ql += q[k] * q[k]; }
        ql = sqrt(ql) + 1e-9; for (int k = 0; k < 4; k++) q[k] = (float)(q[k] / ql);
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float R[9] = { 1 - 2*(y*y + z*z), 2*(x*y + z*w), 2*(x*z - y*w),  2*(x*y - z*w), 1 - 2*(x*x + z*z), 2*(y*z + x*w),
                       2*(x*z + y*w), 2*(y*z - x*w), 1 - 2*(x*x + y*y) };
        for (int r = 0; r < 3; r++) { for (int k = 0; k < 3; k++) wf(m + 16 * r + 4 * k, R[3 * r + k]); wf(m + 16 * r + 12, 0); }
        for (int k = 0; k < 3; k++) wf(m + 0x30 + 4 * k, urnd(-200, 200));
        wf(m + 0x3C, 1);
        for (int k = 0; k < 3; k++) wf(cam + 4 * k, ld(m + 0x30 + 4 * k) + urnd(-30, 30));
        uint32_t count = 1 + rnd(64), mode = rnd(2), sstride = 0x18 + 4 * rnd(3), dstride = 8 + 4 * rnd(4);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = src + i * sstride;
            float nx = urnd(-1, 1), ny = urnd(-1, 1), nz = urnd(-1, 1), nl = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-6f;
            wf(v, urnd(-2.5f, 2.5f)); wf(v + 4, urnd(-1, 1.5f)); wf(v + 8, urnd(-1.2f, 1.2f));
            wf(v + 12, nx / nl); wf(v + 16, ny / nl); wf(v + 20, nz / nl);
        }
        float scale = urnd(0.5f, 2), uo = urnd(0, 1), vo = urnd(0, 1);
        uint32_t esp0 = g_esp;
        for (int pass = 0; pass < 2; pass++) {
            g_env_exact = pass == 0;
            PUSH32(g_esp, 0); PUSH32(g_esp, mode); PUSH32(g_esp, fbits(vo)); PUSH32(g_esp, fbits(uo));
            PUSH32(g_esp, fbits(scale)); PUSH32(g_esp, m); PUSH32(g_esp, cam); PUSH32(g_esp, count);
            PUSH32(g_esp, sstride); PUSH32(g_esp, src); PUSH32(g_esp, RECOMP_RETADDR);
            g_ecx = pass ? B : A; g_edx = dstride;
            fam_native_env_process();
            g_esp = esp0;
        }
        for (uint32_t i = 0; i < count; i++) {
            const float *a = (const float *)ADDR(A + i * dstride), *b = (const float *)ADDR(B + i * dstride);
            int pa = a[0] == 0.5f + uo && a[1] == 0.5f + vo, pb = b[0] == 0.5f + uo && b[1] == 0.5f + vo;
            nv++;
            if (pa != pb) { flips++; continue; }
            if (fabsf(a[0]) > 4 || fabsf(a[1]) > 4) { near++; continue; }
            double d = fabs((double)a[0] - b[0]); if (fabs((double)a[1] - b[1]) > d) d = fabs((double)a[1] - b[1]);
            if (d > maxd) maxd = d;
        }
    }
    g_env_exact = -1;
    int ok = maxd < 1e-3 && flips * 1000 <= nv;
    fprintf(stderr, "[native] EnvProcess single precision: %s (%d vertices: largest difference %.2g of the texture, %d at the pole flipped, %d next to it)%s\n",
            ok ? "within tolerance" : "OUT OF TOLERANCE", nv, maxd, flips, near, ok ? "" : ": double precision kept");
    return ok;
}

int fam_native_vtx_check(void) {
    g_env_exact = 1;                    /* the bit-exact comparison is of the double-precision path */
    uint32_t mem = hle_va(hle_heap_alloc(2 * AREA + 0x4000, 1));
    uint32_t A = mem, B = mem + AREA, in = mem + 2 * AREA;       /* in: sources, matrices, weights, frame */
    uint32_t mA = in, mB = in + 0x40, mC = in + 0x80, mD = in + 0xC0, cam = in + 0x100, wts = in + 0x110;
    uint32_t frm = in + 0x300, src = in + 0x600;                 /* wts: 100 floats, frm: 0x200 bytes, src: up to 0x3A00 */
    int bad = 0, ncase = 0;
    for (int n = 0; n < 20000 && bad < 3; n++) {
        for (int i = 0; i < 0x44; i++) wf(in + 4 * i, rndf());    /* the four matrices and the camera */
        for (int i = 0; i < 100; i++) wf(wts + 4 * i, rnd(4) ? (float)rnd(1001) / 1000.0f : rndf());
        for (int i = 0; i < 0x3A00 / 4; i++) wf(src + 4 * i, rndf());
        int which = (int)rnd(3);
        uint32_t count = 1 + rnd(which == 2 ? 40 : 100);
        memset((void *)ADDR(A), 0xCD, AREA);
        memset((void *)ADDR(B), 0xCD, AREA);
        uint32_t esp0 = g_esp, eA, eB, cA, cB, dA, dB;
        int ok1, ok2;
        char extra[96] = "";
        if (which == 0 || which == 1) {
            for (int pass = 0; pass < 2; pass++) {
                if (which == 0) { PUSH32(g_esp, mB); PUSH32(g_esp, mA); PUSH32(g_esp, count); }
                else { PUSH32(g_esp, mD); PUSH32(g_esp, mC); PUSH32(g_esp, wts); PUSH32(g_esp, mB); PUSH32(g_esp, mA); PUSH32(g_esp, count); }
                PUSH32(g_esp, RECOMP_RETADDR);
                g_ecx = pass ? B : A; g_edx = src;
                if (which == 0) (pass ? fam_native_transform_vertices : recomp_ls3df_10068360_orig)();
                else (pass ? fam_native_transform_vertices_blend : recomp_ls3df_10068430_orig)();
                if (pass) { ok2 = g_esp == esp0; eB = g_eax; cB = g_ecx; dB = g_edx; }
                else { ok1 = g_esp == esp0; eA = g_eax; cA = g_ecx; dA = g_edx; }
                g_esp = esp0;
            }
            if (eA != eB || cA != cB || dA != dB) snprintf(extra, sizeof extra, "eax %08X/%08X ecx %08X/%08X edx %08X/%08X", eA, eB, cA, cB, dA, dB);
        } else {
            uint32_t mode = rnd(7), dstride = 8 + 4 * rnd(6), sstride = 0x18 + 4 * rnd(4);
            if (dstride * count > AREA) count = AREA / dstride;
            if (sstride * count > 0x3A00) count = 0x3A00 / sstride;
            if (mode <= 1 && rnd(4) == 0) {                      /* steer some vertices into the pole */
                memset((void *)ADDR(mA), 0, 64); wf(mA, 1); wf(mA + 0x14, 1); wf(mA + 0x28, 1); wf(mA + 0x3C, 1);
                memset((void *)ADDR(cam), 0, 12); wf(cam + (mode ? 4 : 8), -(float)(1 + rnd(5)));
                for (uint32_t i = 0; i < count; i++) if (rnd(2)) {
                    uint32_t v = src + i * sstride;
                    memset((void *)ADDR(v), 0, 24); wf(v + 12 + (mode ? 4 : 8), 1.0f);
                }
            }
            uint32_t f = rnd(5) ? frm : 0;
            for (int i = 0; i < 0x80; i++) wf(frm + 4 * i, rndf());
            MEM32(frm + 0x198) |= 0x10;                          /* inverse matrix valid: GetInvMatrix returns +0x158 */
            float scale = rndf(), uo = rndf(), vo = rndf();
            for (int pass = 0; pass < 2; pass++) {
                PUSH32(g_esp, f); PUSH32(g_esp, mode); PUSH32(g_esp, fbits(vo)); PUSH32(g_esp, fbits(uo));
                PUSH32(g_esp, fbits(scale)); PUSH32(g_esp, mA); PUSH32(g_esp, cam); PUSH32(g_esp, count);
                PUSH32(g_esp, sstride); PUSH32(g_esp, src); PUSH32(g_esp, RECOMP_RETADDR);
                g_ecx = pass ? B : A; g_edx = dstride;
                (pass ? fam_native_env_process : recomp_ls3df_10067D20_orig)();
                if (pass) ok2 = g_esp == esp0; else ok1 = g_esp == esp0;
                g_esp = esp0;
            }
            snprintf(extra, sizeof extra, "(mode %u, %u vertices, strides %u/%u)", mode, count, dstride, sstride);
        }
        ncase++;
        if (!ok1 || !ok2 || memcmp((void *)ADDR(A), (void *)ADDR(B), AREA) || extra[0] == 'e') {
            static const char *nm[3] = { "TransformVertices", "TransformVerticesBlend", "EnvProcess" };
            bad += report(nm[which], n, A, B, ok1, ok2, extra);
        }
    }
    fprintf(stderr, "[native] TransformVertices/Blend/EnvProcess: %s against the translated originals (%d random cases)\n",
            bad ? "DIFFER: native vertex processing disabled" : "match", ncase);
    int fast_ok = bad ? 1 : env_fast_check(A, B, in);
    hle_heap_free(hle_ptr(mem));
    if (bad) fam_native_vtx_on = 0;
    {   const char *e = getenv("FAMILIA_ENV_EXACT"); g_env_exact = (e && atoi(e)) || !fast_ok; }
    return 0;
}
