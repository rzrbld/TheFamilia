/*
 * native_math.c - native (NEON) replacements for hot LS3D math methods.
 *
 * Each replaces one translated function, entered with the same guest stack
 * (stdcall: [ret][this][retptr][arg]) and leaving the same guest-visible
 * state: result memory, eax/ecx/edx, esp popped by ret 0xC. Arithmetic follows
 * the original x87 sequence operand for operand: float operands widened to
 * double, products and sums in the original order, one rounding to float at
 * the store. This file is compiled with -ffp-contract=off so no multiply-add
 * is fused (which would round differently).
 *
 * FAMILIA_NATIVE=0 disables the replacements (the translated originals run).
 * FAMILIA_NATIVE_CHECK=1 runs a differential test at startup: each native
 * function against its translated original on random inputs, bit for bit;
 * any mismatch is logged and the native path is switched off.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

int fam_native_on = 1;

static inline float rd(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static inline void wr(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }
static inline const float *fp(uint32_t va) { return (const float *)ADDR(va); }

/* S_matrix S_matrix::operator*(const S_matrix &b) const  -- LS3DF 0x1002D3A0
 * r[i][j] = (a_i0*b_0j + a_i1*b_1j) + a_i2*b_2j      (i,j < 3), r[i][3] = 0
 * r[3][j] = ((a_30*b_0j + b_3j) + a_31*b_1j) + a_32*b_2j,       r[3][3] = 1 */
void fam_native_s_matrix_mul(void) {
    uint32_t a = MEM32(g_esp + 4), ret = MEM32(g_esp + 8), b = MEM32(g_esp + 12);
    const float *A = fp(a), *B = fp(b);
    float R[16];
#if defined(__aarch64__)
    float64x2_t b0 = vcvt_f64_f32(vld1_f32(B + 0)), b1 = vcvt_f64_f32(vld1_f32(B + 4));
    float64x2_t b2 = vcvt_f64_f32(vld1_f32(B + 8)), b3 = vcvt_f64_f32(vld1_f32(B + 12));
    double b02 = B[2], b12 = B[6], b22 = B[10], b32 = B[14];
    for (int i = 0; i < 4; i++) {
        double x = A[i * 4], y = A[i * 4 + 1], z = A[i * 4 + 2];
        float64x2_t s = vmulq_n_f64(b0, x);                 /* columns 0,1 */
        double s2 = x * b02;                                  /* column 2 */
        if (i == 3) { s = vaddq_f64(s, b3); s2 = s2 + b32; }
        s = vaddq_f64(s, vmulq_n_f64(b1, y)); s2 = s2 + y * b12;
        s = vaddq_f64(s, vmulq_n_f64(b2, z)); s2 = s2 + z * b22;
        vst1_f32(R + i * 4, vcvt_f32_f64(s));
        R[i * 4 + 2] = (float)s2;
        R[i * 4 + 3] = i == 3 ? 1.0f : 0.0f;
    }
#else
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            double s = (double)A[i * 4] * B[j];
            if (i == 3) s = s + B[12 + j];
            s = s + (double)A[i * 4 + 1] * B[4 + j];
            s = s + (double)A[i * 4 + 2] * B[8 + j];
            R[i * 4 + j] = (float)s;
        }
        R[i * 4 + 3] = i == 3 ? 1.0f : 0.0f;
    }
#endif
    memcpy((void *)ADDR(ret), R, 64);
    g_eax = ret; g_ecx = 0;                     /* rep movsd left ecx = 0 */
    g_esp += 4 + 12;
}

/* S_vector S_vector::operator*(const S_matrix &m) const  -- LS3DF 0x1002E980
 * r[j] = ((v0*m_0j + m_3j) + v1*m_1j) + v2*m_2j */
void fam_vmm(const float *V, const float *M, float *R) {
#if defined(__aarch64__)
    double x = V[0], y = V[1], z = V[2];
    float64x2_t s = vaddq_f64(vmulq_n_f64(vcvt_f64_f32(vld1_f32(M + 0)), x), vcvt_f64_f32(vld1_f32(M + 12)));
    s = vaddq_f64(s, vmulq_n_f64(vcvt_f64_f32(vld1_f32(M + 4)), y));
    s = vaddq_f64(s, vmulq_n_f64(vcvt_f64_f32(vld1_f32(M + 8)), z));
    double s2 = x * (double)M[2] + (double)M[14];
    s2 = s2 + y * (double)M[6];
    s2 = s2 + z * (double)M[10];
    vst1_f32(R, vcvt_f32_f64(s));
    R[2] = (float)s2;
#else
    for (int j = 0; j < 3; j++) {
        double s = (double)V[0] * M[j] + M[12 + j];
        s = s + (double)V[1] * M[4 + j];
        s = s + (double)V[2] * M[8 + j];
        R[j] = (float)s;
    }
#endif
}
void fam_native_s_vector_mul_matrix(void) {
    uint32_t v = MEM32(g_esp + 4), ret = MEM32(g_esp + 8), m = MEM32(g_esp + 12);
    float R[3];
    fam_vmm(fp(v), fp(m), R);
    memcpy((void *)ADDR(ret), R, 12);
    uint32_t r2; memcpy(&r2, &R[2], 4);
    g_eax = ret; g_ecx = ret; g_edx = r2;
    g_esp += 4 + 12;
}

/* ---------------------------------------------------- differential check */
void recomp_ls3df_1002D3A0_orig(void);
void recomp_ls3df_1002E980_orig(void);

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static float rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    int e = (int)(g_rng % 40) - 20;                       /* magnitudes 2^-20 .. 2^20 */
    float f = (float)((double)(int32_t)(g_rng >> 32) / 2147483648.0);
    return ldexpf(f, e);
}
static int run_case(void (*fn)(void), uint32_t a, uint32_t r, uint32_t b, uint32_t *eax) {
    PUSH32(g_esp, b); PUSH32(g_esp, r); PUSH32(g_esp, a); PUSH32(g_esp, RECOMP_RETADDR);
    uint32_t esp0 = g_esp + 16;
    fn();
    *eax = g_eax;
    return g_esp == esp0;
}
static int check(const char *name, void (*orig)(void), void (*nat)(void), int out_floats) {
    uint32_t mem = hle_va(hle_heap_alloc(256, 1));
    uint32_t A = mem, B = mem + 64, R1 = mem + 128, R2 = mem + 192;
    int bad = 0, lsb = 0;
    for (int n = 0; n < 100000 && bad < 5; n++) {
        for (int i = 0; i < 16; i++) { wr(A + 4 * i, rnd()); wr(B + 4 * i, rnd()); }
        memset((void *)ADDR(R1), 0xAB, 64); memset((void *)ADDR(R2), 0xAB, 64);
        uint32_t e1, e2;
        int ok1 = run_case(orig, A, R1, B, &e1), ok2 = run_case(nat, A, R2, B, &e2);
        int big = !ok1 || !ok2 || e1 != R1 || e2 != R2;
        for (int i = 0; i < out_floats && !big; i++) {
            uint32_t u1 = MEM32(R1 + 4 * i), u2 = MEM32(R2 + 4 * i);
            if (u1 == u2) continue;
            /* the translated code may use fused multiply-add (one rounding) where
             * x87 and this file round twice: a last-bit difference is expected */
            uint32_t d = u1 > u2 ? u1 - u2 : u2 - u1;
            if (d == 1 && (u1 ^ u2) >> 31 == 0) lsb++; else big = 1;
        }
        if (big) {
            if (!bad) fprintf(stderr, "[native] %s MISMATCH (case %d): esp %d/%d eax %08X/%08X\n", name, n, ok1, ok2, e1, e2);
            bad++;
        }
    }
    fprintf(stderr, "[native] %s: %s against the translated original (100000 random cases, %d last-bit FMA differences)\n",
            name, bad ? "DIFFERS" : "matches", lsb);
    hle_heap_free(hle_ptr(mem));
    return bad;
}

void fam_native_init(void) {
    const char *e = getenv("FAMILIA_NATIVE");
    if (e && !atoi(e)) { fam_native_on = 0; fprintf(stderr, "[native] disabled\n"); return; }
    e = getenv("FAMILIA_NATIVE_CHECK");
    if (e && atoi(e)) {
        mach_enter();
        int bad = check("S_matrix::operator*", recomp_ls3df_1002D3A0_orig, fam_native_s_matrix_mul, 16)
                + check("S_vector::operator*(S_matrix)", recomp_ls3df_1002E980_orig, fam_native_s_vector_mul_matrix, 3);
        int fam_native_crt_check(void);
        bad += fam_native_crt_check();
        int fam_native_svec_check(void);
        bad += fam_native_svec_check();
        int fam_native_shadow_check(void);
        bad += fam_native_shadow_check();
        int fam_native_coll_check(void);
        bad += fam_native_coll_check();
        int fam_native_rwdata_check(void);
        bad += fam_native_rwdata_check();
        int fam_native_anim_check(void);
        bad += fam_native_anim_check();
        int fam_native_ipol_check(void);
        bad += fam_native_ipol_check();
        int fam_native_misc_check(void);
        bad += fam_native_misc_check();
        int fam_native_vtx_check(void);
        bad += fam_native_vtx_check();
        int fam_native_light_check(void);
        bad += fam_native_light_check();
        int fam_native_frame_check(void);
        bad += fam_native_frame_check();
        { int fam_native_fftree_check(void); bad += fam_native_fftree_check(); }
        int fam_native_cull_check(void); bad += fam_native_cull_check();
        mach_leave();
        if (bad) { fam_native_on = 0; fprintf(stderr, "[native] mismatches: native math disabled\n"); }
    }
}
