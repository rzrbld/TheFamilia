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

void fam_native_env_process(void) {
    uint32_t a = g_esp;
    uint32_t src = MEM32(a + 4), sstride = MEM32(a + 8), count = MEM32(a + 12), cam = MEM32(a + 16);
    uint32_t m = MEM32(a + 20), mode = MEM32(a + 36), frm = MEM32(a + 40);
    double scale = ld(a + 24), uofs = ld(a + 28), vofs = ld(a + 32);
    uint32_t dst = g_ecx, dstride = g_edx;
    if (!fam_native_vtx_on || (mode <= 1 && !count)) { recomp_ls3df_10067D20_orig(); return; }
    switch (mode) {
    case 0: case 1: {
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

int fam_native_vtx_check(void) {
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
    hle_heap_free(hle_ptr(mem));
    if (bad) fam_native_vtx_on = 0;
    return 0;
}
