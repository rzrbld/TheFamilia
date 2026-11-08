/*
 * native_anim.c - native replacements for the LS3D math the animation code
 * (I3D_interpolator::Update, frame matrices) spends its time in.
 *
 * Exactness model as in native_frame.c: every x87 value is a double, rounded
 * to float only where the original stores a dword; same operation order
 * (compiled with -ffp-contract=off); fsin/fsqrt/fdiv as the translator lifts
 * them (sin, sqrt, plain division). Stores the originals make into their own
 * argument slots (dead to the caller, but guest-visible) are reproduced.
 * fam_native_anim_check (FAMILIA_NATIVE_CHECK=1) compares each with its
 * translated original; a mismatch switches this group off.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

int fam_native_anim_on = 1;
/* per-function switches (a failing self-test turns off only that function) */
enum { M_MULA, M_ROT, M_SETROT, M_NORM, M_SLERP, M_UPDM, M_CTOR, M_DOT, M_MULF, M_ADD, M_COUNT };
static uint8_t g_fn_off[M_COUNT];
#define ON(k) (fam_native_anim_on && !g_fn_off[k])

static inline float ldf(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static inline void stf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }
static inline uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
#define XPUSH(v) fp_push_impl(g_st, &g_fp_top, (v))
#define XPOP()   fp_pop_impl(g_st, &g_fp_top)
#define ONE 0x1009B304u               /* 1.0f */

void recomp_ls3df_1002E920_orig(void);
void recomp_ls3df_1002EA10_orig(void);
void recomp_ls3df_1002E070_orig(void);
void recomp_ls3df_10030140_orig(void);
void recomp_ls3df_1002FEE0_orig(void);
void recomp_ls3df_1001A6E0_orig(void);
void recomp_ls3df_1000DD60_orig(void);
void recomp_ls3df_1000DDB0_orig(void);
void recomp_ls3df_100112A0_orig(void);
void recomp_ls3df_100112D0_orig(void);

static void guest_call(uint32_t fn) {      /* the caller has pushed the arguments */
    recomp_func_t f = recomp_lookup_manual(fn);
    if (!f) f = recomp_lookup(fn);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
}

/* S_vector &__stdcall S_vector::operator*=(const S_matrix &m)  -- 0x1002E920, ret 8 */
void fam_native_vec_mul_assign(void) {
    if (!ON(M_MULA)) { recomp_ls3df_1002E920_orig(); return; }
    uint32_t v = MEM32(g_esp + 4), m = MEM32(g_esp + 8);
    double vx = ldf(v), vy = ldf(v + 4), vz = ldf(v + 8);
    double x = (((double)ldf(m + 0x20) * vz + (double)ldf(m + 0x10) * vy) + (double)ldf(m) * vx) + (double)ldf(m + 0x30);
    double y = (((double)ldf(m + 0x24) * vz + (double)ldf(m + 4) * vx) + (double)ldf(m + 0x14) * vy) + (double)ldf(m + 0x34);
    double z = (((double)ldf(m + 0x28) * vz + (double)ldf(m + 8) * vx) + (double)ldf(m + 0x18) * vy) + (double)ldf(m + 0x38);
    stf(v + 8, (float)z); stf(v, (float)x); stf(v + 4, (float)y);
    g_eax = v; g_ecx = m;
    g_esp += 4 + 8;
}

/* S_vector __stdcall S_vector::RotateByMatrix(const S_matrix &m) const  -- 0x1002EA10, ret 0xC */
void fam_native_vec_rot_by_matrix(void) {
    if (!ON(M_ROT)) { recomp_ls3df_1002EA10_orig(); return; }
    uint32_t v = MEM32(g_esp + 4), ret = MEM32(g_esp + 8), m = MEM32(g_esp + 12);
    double vx = ldf(v), vy = ldf(v + 4), vz = ldf(v + 8);
    float r0 = (float)(((double)vx * ldf(m) + vy * (double)ldf(m + 0x10)) + vz * (double)ldf(m + 0x20));
    float r1 = (float)(((double)vx * ldf(m + 4) + vy * (double)ldf(m + 0x14)) + vz * (double)ldf(m + 0x24));
    float r2 = (float)(((double)vx * ldf(m + 8) + vy * (double)ldf(m + 0x18)) + vz * (double)ldf(m + 0x28));
    stf(ret, r0); stf(ret + 4, r1); stf(ret + 8, r2);
    g_eax = ret; g_ecx = ret; g_edx = fb(r2);
    g_esp += 4 + 12;
}

/* void __stdcall S_matrix::SetRot3(const S_quat &q)  -- 0x1002E070, ret 8 (q: s, x, y, z) */
static void setrot3(uint32_t mat, uint32_t q, uint32_t argslot) {
    double s = ldf(q), x = ldf(q + 4), y = ldf(q + 8), z = ldf(q + 12);
    if (!(fabs(s) < MEMD(0x1009BF90u))) {                /* |s| >= ~1 (or NaN... see below): identity rows */
        if (isnan(fabs(s))) goto compute;               /* unordered sets C0: the compute path */
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) MEM32(mat + 0x10 * r + 4 * k) = MEM32(0x1009BF48u + 0x10 * r + 4 * k);
        g_eax = mat + 0x20; g_ecx = MEM32(0x1009BF70u); g_edx = MEM32(0x1009BF6Cu);
        return;
    }
compute:;
    double X2 = x + x, Y2 = y + y;
    float Z2f = (float)(z + z);
    if (argslot) stf(argslot, Z2f);
    float xx = (float)(X2 * x), xy = (float)(Y2 * x), xz = (float)((double)Z2f * x);
    float yy = (float)(Y2 * y), yz = (float)((double)Z2f * y), zz = (float)((double)Z2f * z);
    float xs = (float)(X2 * s);
    double ys = Y2 * s, zs = (double)Z2f * s, one = ldf(ONE);
    stf(mat, (float)(one - ((double)zz + yy)));
    stf(mat + 4, (float)((double)xy - zs));
    stf(mat + 8, (float)(ys + xz));
    stf(mat + 0x10, (float)(zs + xy));
    stf(mat + 0x14, (float)(one - ((double)zz + xx)));
    stf(mat + 0x18, (float)((double)yz - xs));
    stf(mat + 0x20, (float)((double)xz - ys));
    stf(mat + 0x24, (float)((double)xs + yz));
    stf(mat + 0x28, (float)(one - ((double)yy + xx)));
    g_eax = mat; g_ecx = q;
}
void fam_native_setrot3(void) {
    if (!ON(M_SETROT)) { recomp_ls3df_1002E070_orig(); return; }
    setrot3(MEM32(g_esp + 4), MEM32(g_esp + 8), g_esp + 8);
    g_esp += 4 + 8;
}

/* void __stdcall S_quat::Normalize()  -- 0x10030140, ret 4 */
void fam_native_quat_normalize(void) {
    if (!ON(M_NORM)) { recomp_ls3df_10030140_orig(); return; }
    uint32_t q = MEM32(g_esp + 4);
    double q0 = ldf(q), q1 = ldf(q + 4), q2 = ldf(q + 8), q3 = ldf(q + 12);
    float sq = (float)(((q0 * q0 + q1 * q1) + q2 * q2) + q3 * q3);
    stf(g_esp + 4, sq);
    double c0 = ldf(0x1009B310u), one = ldf(ONE);
    g_ecx = q;
    g_esp += 8;
    if ((double)sq > (double)ldf(0x1009B308u)) {          /* not degenerate */
        if (fabs((double)sq - one) < MEMD(0x1009B4C8u)) return;   /* already unit */
        double inv = one / sqrt((double)sq);
        stf(q + 4, (float)(inv * ldf(q + 4)));
        stf(q + 8, (float)(inv * ldf(q + 8)));
        stf(q + 12, (float)(inv * ldf(q + 12)));
        stf(q, (float)(inv * ldf(q)));
        return;
    }
    /* degenerate: a unit quaternion on the first non-zero axis of x, z, y */
    uint32_t slot;
    if (!((double)ldf(q + 4) == c0)) slot = q + 4;
    else if (!((double)ldf(q + 12) == c0)) slot = q + 12;
    else slot = q + 8;
    int neg = (double)ldf(slot) < c0;
    MEM32(q) = 0;
    MEM32(slot) = MEM32(neg ? 0x1009B314u : ONE);
}

/* S_quat __stdcall S_quat::Slerp(const S_quat &q, float t, ...)  -- 0x1002FEE0, ret 0x14 */
void fam_native_quat_slerp(void) {
    if (!ON(M_SLERP)) { recomp_ls3df_1002FEE0_orig(); return; }
    uint32_t a = g_esp;                                   /* [a+4] this, [a+8] ret, [a+0xC] q, [a+0x10] t */
    uint32_t p = MEM32(a + 4), ret = MEM32(a + 8), q = MEM32(a + 12);
    double cosom = (((double)ldf(q + 8) * ldf(p + 8) + (double)ldf(q + 4) * ldf(p + 4)) + (double)ldf(q) * ldf(p))
                 + (double)ldf(p + 12) * ldf(q + 12);
    stf(a + 12, (float)cosom);
    float t1, t2, t3, t0;                                 /* q, negated when cos < 0 */
    if (cosom < (double)ldf(0x1009B310u)) {
        stf(a + 12, -ldf(a + 12));
        t1 = -ldf(q + 4); t2 = -ldf(q + 8); t3 = -ldf(q + 12); t0 = -ldf(q);
    } else { t1 = ldf(q + 4); t2 = ldf(q + 8); t3 = ldf(q + 12); t0 = ldf(q); }
    double one = ldf(ONE), t = ldf(a + 16), s0, s1;
    double d = one - (double)ldf(a + 12);
    if (d > MEMD(0x1009BF98u)) {
        XPUSH((double)ldf(a + 12));
        g_esp = a - 0x24;                                 /* below the original's frame */
        guest_call(0x1008D920u);                          /* __CIacos: st0 -> st0 */
        g_esp = a;
        double omega = XPOP();
        stf(a + 12, (float)omega);
        double sn = sin(omega);
        stf(a + 4, (float)(one / sn));
        s0 = sin((one - t) * (double)ldf(a + 12)) * (double)ldf(a + 4);
        s1 = sin((double)ldf(a + 12) * t) * (double)ldf(a + 4);
    } else { s0 = one - t; s1 = t; }
    float r1 = (float)((double)t1 * s1 + s0 * (double)ldf(p + 4));
    float r2 = (float)((double)t2 * s1 + s0 * (double)ldf(p + 8));
    float r3 = (float)(s0 * (double)ldf(p + 12) + s1 * (double)t3);
    float r0 = (float)(s0 * (double)ldf(p) + s1 * (double)t0);
    stf(ret, r0); stf(ret + 4, r1); stf(ret + 8, r2); stf(ret + 12, r3);
    g_eax = ret; g_edx = ret; g_ecx = fb(r3);
    g_esp = a + 4 + 0x14;
}

/* void I3D_frame::UpdateMatrix()  -- 0x1001A6E0, thiscall: the local matrix from rotation and scale */
static void scale_rows(uint32_t f) {
    for (int k = 0; k < 3; k++) stf(f + 0x50 + 4 * k, (float)((double)ldf(f + 0x50 + 4 * k) * ldf(f + 0x90)));
    for (int k = 0; k < 3; k++) stf(f + 0x60 + 4 * k, (float)((double)ldf(f + 0x60 + 4 * k) * ldf(f + 0x94)));
    for (int k = 0; k < 3; k++) stf(f + 0x70 + 4 * k, (float)((double)ldf(f + 0x70 + 4 * k) * ldf(f + 0x98)));
}
void fam_native_update_matrix(void) {
    if (!ON(M_UPDM)) { recomp_ls3df_1001A6E0_orig(); return; }
    uint32_t f = g_ecx, fl = MEM32(f + 0xAC);
    if (!(fl & 0x10)) {
        MEM32(f + 0xAC) = fl | 0x14;
        if (ON(M_SETROT)) setrot3(f + 0x50, f + 0x9C, 0);
        else { PUSH32(g_esp, f + 0x9C); PUSH32(g_esp, f + 0x50); PUSH32(g_esp, RECOMP_RETADDR); recomp_ls3df_1002E070_orig(); }
        scale_rows(f);
    } else if (!(fl & 4)) {
        if (!(fl & 8)) { PUSH32(g_esp, f + 0x50); PUSH32(g_esp, f + 0x9C); guest_call(0x1002EEF0u); }   /* S_quat::Make */
        MEM32(f + 0xAC) |= 0xC;
        scale_rows(f);
    }
    g_ecx = f;
    g_esp += 4;
}

/* small S_vector helpers (thiscall) */
void fam_native_vec_ctor(void) {             /* 0x1000DD60: S_vector(x, y, z), ret 0xC */
    if (!ON(M_CTOR)) { recomp_ls3df_1000DD60_orig(); return; }
    uint32_t t = g_ecx, a1 = MEM32(g_esp + 4), a2 = MEM32(g_esp + 8), a3 = MEM32(g_esp + 12);
    MEM32(t) = a1; MEM32(t + 4) = a2; MEM32(t + 8) = a3;
    g_eax = t; g_ecx = a3; g_edx = a2;
    g_esp += 16;
}
void fam_native_vec_dot(void) {              /* 0x1000DDB0: float Dot(const S_vector &v), ret 4 */
    if (!ON(M_DOT)) { recomp_ls3df_1000DDB0_orig(); return; }
    uint32_t t = g_ecx, v = MEM32(g_esp + 4);
    double r = (double)ldf(v + 8) * ldf(t + 8);
    r = r + (double)ldf(v + 4) * ldf(t + 4);
    r = r + (double)ldf(v) * ldf(t);
    XPUSH(r);
    g_eax = v;
    g_esp += 8;
}
void fam_native_vec_mul_f(void) {            /* 0x100112A0: S_vector operator*(const float &f), ret 8 */
    if (!ON(M_MULF)) { recomp_ls3df_100112A0_orig(); return; }
    uint32_t t = g_ecx, ret = MEM32(g_esp + 4), fp = MEM32(g_esp + 8);
    double f = ldf(fp);
    float z = (float)((double)ldf(t + 8) * f), y = (float)((double)ldf(t + 4) * f), x = (float)(f * (double)ldf(t));
    stf(ret, x); stf(ret + 4, y); stf(ret + 8, z);
    g_eax = ret;
    g_esp += 12;
}
void fam_native_vec_add(void) {              /* 0x100112D0: S_vector operator+(const S_vector &v), ret 8 */
    if (!ON(M_ADD)) { recomp_ls3df_100112D0_orig(); return; }
    uint32_t t = g_ecx, ret = MEM32(g_esp + 4), o = MEM32(g_esp + 8);
    float z = (float)((double)ldf(o + 8) + ldf(t + 8)), y = (float)((double)ldf(o + 4) + ldf(t + 4)), x = (float)((double)ldf(o) + ldf(t));
    stf(ret, x); stf(ret + 4, y); stf(ret + 8, z);
    g_eax = ret;
    g_esp += 12;
}

/* ---------------------------------------------------- differential check */
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 33) % n; }
static float rndf(float lo, float hi) {
    switch (rnd(30)) { case 0: return 0.0f; case 1: return -0.0f; case 2: return 1.0f; case 3: return -1.0f; }
    return lo + (hi - lo) * (float)rnd(1000001) / 1000000.0f;
}

static const char *const k_names[M_COUNT] = { "S_vector::operator*=", "S_vector::RotateByMatrix", "S_matrix::SetRot3",
    "S_quat::Normalize", "S_quat::Slerp", "I3D_frame::UpdateMatrix", "S_vector::S_vector", "S_vector::Dot",
    "S_vector::operator*(float)", "S_vector::operator+" };

int fam_native_anim_check(void) {
    enum { SZ = 0x400 };
    uint32_t mem = hle_va(hle_heap_alloc(SZ, 1));
    uint8_t snap[SZ], r1[SZ];
    int bad = 0, cases[M_COUNT] = { 0 }, fbad[M_COUNT] = { 0 };
    for (int n = 0; n < 40000; n++) {
        int k = (int)rnd(M_COUNT);
        if (fbad[k] >= 3) continue;
        /* random floats everywhere; quaternions sometimes unit, sometimes tiny or axis-aligned */
        for (uint32_t o = 0; o < SZ; o += 4) stf(mem + o, rndf(-3, 3));
        uint32_t A = mem, B = mem + 0x40, R = mem + 0x80, F = mem + 0x100, T = mem + 0xF0;
        if (k == M_NORM || k == M_SETROT || k == M_SLERP) {
            int mode = (int)rnd(4);
            for (int j = 0; j < 4; j++) {
                if (mode == 1) stf(A + 4 * j, rndf(-1e-5f, 1e-5f));
                if (mode == 2) stf(A + 4 * j, j == (int)rnd(4) ? rndf(-1, 1) : 0.0f);
            }
            if (mode == 3) { double l = 0; for (int j = 0; j < 4; j++) l += (double)ldf(A + 4 * j) * ldf(A + 4 * j);
                             if (l > 0) for (int j = 0; j < 4; j++) stf(A + 4 * j, (float)(ldf(A + 4 * j) / sqrt(l))); }
            if (k == M_SLERP && rnd(3) == 0) memcpy((void *)ADDR(B), (void *)ADDR(A), 16);
        }
        if (k == M_UPDM) MEM32(F + 0xAC) = rnd(32);
        uint32_t extra = rnd(2);                          /* Slerp's 5th argument: the same for both runs */
        stf(T, rndf(0, 1));
        memcpy(snap, (void *)ADDR(mem), SZ);
        uint32_t eax[2], ecx[2], edx[2], dsp[2], args[2][6]; int dst[2]; double st0[2] = { 0, 0 };
        for (int pass = 0; pass < 2; pass++) {
            memcpy((void *)ADDR(mem), snap, SZ);
            uint32_t esp0 = g_esp; int sb0 = g_st_base;
            switch (k) {                                  /* arguments, pushed right to left */
            case M_MULA: PUSH32(g_esp, B); PUSH32(g_esp, A); break;
            case M_ROT: PUSH32(g_esp, B); PUSH32(g_esp, R); PUSH32(g_esp, A); break;
            case M_SETROT: PUSH32(g_esp, A); PUSH32(g_esp, R); break;
            case M_NORM: PUSH32(g_esp, A); break;
            case M_SLERP: PUSH32(g_esp, extra); PUSH32(g_esp, MEM32(T)); PUSH32(g_esp, B); PUSH32(g_esp, R); PUSH32(g_esp, A); break;
            case M_UPDM: g_ecx = F; break;
            case M_CTOR: PUSH32(g_esp, MEM32(B + 8)); PUSH32(g_esp, MEM32(B + 4)); PUSH32(g_esp, MEM32(B)); g_ecx = A; break;
            case M_DOT: PUSH32(g_esp, B); g_ecx = A; break;
            case M_MULF: PUSH32(g_esp, T); PUSH32(g_esp, R); g_ecx = A; break;
            case M_ADD: PUSH32(g_esp, B); PUSH32(g_esp, R); g_ecx = A; break;
            }
            uint32_t argtop = g_esp;
            PUSH32(g_esp, RECOMP_RETADDR);
            g_edx = 0x5A5A5A5Au;
            static void (*const orig[M_COUNT])(void) = { recomp_ls3df_1002E920_orig, recomp_ls3df_1002EA10_orig, recomp_ls3df_1002E070_orig,
                recomp_ls3df_10030140_orig, recomp_ls3df_1002FEE0_orig, recomp_ls3df_1001A6E0_orig, recomp_ls3df_1000DD60_orig,
                recomp_ls3df_1000DDB0_orig, recomp_ls3df_100112A0_orig, recomp_ls3df_100112D0_orig };
            static void (*const nat[M_COUNT])(void) = { fam_native_vec_mul_assign, fam_native_vec_rot_by_matrix, fam_native_setrot3,
                fam_native_quat_normalize, fam_native_quat_slerp, fam_native_update_matrix, fam_native_vec_ctor,
                fam_native_vec_dot, fam_native_vec_mul_f, fam_native_vec_add };
            (pass ? nat : orig)[k]();
            eax[pass] = g_eax; ecx[pass] = g_ecx; edx[pass] = g_edx;
            dsp[pass] = g_esp - argtop;
            for (int i = 0; i < 6; i++) args[pass][i] = MEM32(argtop + 4 * i);
            dst[pass] = g_st_base - sb0;
            if (dst[pass] < 0) st0[pass] = XPOP();
            g_esp = esp0;
            if (!pass) memcpy(r1, (void *)ADDR(mem), SZ);
        }
        cases[k]++;
        /* registers the callers can see: eax for those returning a pointer, ecx/edx where defined */
        int regs_ok = dsp[0] == dsp[1] && dst[0] == dst[1] && memcmp(&st0[0], &st0[1], 8) == 0;
        if (k == M_MULA || k == M_ROT || k == M_SLERP || k == M_CTOR || k == M_DOT || k == M_MULF || k == M_ADD) regs_ok &= eax[0] == eax[1];
        if (k == M_ROT || k == M_SLERP || k == M_CTOR) regs_ok &= ecx[0] == ecx[1] && edx[0] == edx[1];
        int nd = 0;
        for (uint32_t o = 0; o < SZ; o += 4) if (memcmp(r1 + o, (void *)ADDR(mem + o), 4)) nd++;
        int argd = memcmp(args[0], args[1], sizeof args[0]) != 0;
        if (nd || !regs_ok || argd) {
            if (!fbad[k]) {
                fprintf(stderr, "[native] %s MISMATCH (case %d): %d words, args %d, eax %08X/%08X ecx %08X/%08X edx %08X/%08X esp %d/%d x87 %d/%d\n",
                        k_names[k], n, nd, argd, eax[0], eax[1], ecx[0], ecx[1], edx[0], edx[1], (int)dsp[0], (int)dsp[1], dst[0], dst[1]);
                int shown = 0;
                for (uint32_t o = 0; o < SZ && shown < 3; o += 4) {
                    uint32_t x, y; memcpy(&x, r1 + o, 4); y = MEM32(mem + o);
                    if (x != y) { fprintf(stderr, "[native]   +%03X: %08X / %08X (input %08X)\n", o, x, y, *(uint32_t *)(snap + o)); shown++; }
                }
                for (int i = 0; i < 6; i++)
                    if (args[0][i] != args[1][i]) fprintf(stderr, "[native]   arg slot [esp+%d]: %08X / %08X\n", 4 + 4 * i, args[0][i], args[1][i]);
            }
            fbad[k]++; bad++;
        }
    }
    for (int k = 0; k < M_COUNT; k++)
        if (fbad[k]) { g_fn_off[k] = 1; fprintf(stderr, "[native] %s: DIFFERS (%d of %d cases) - this one disabled\n", k_names[k], fbad[k], cases[k]); }
    fprintf(stderr, "[native] animation math (operator*=, RotateByMatrix, SetRot3, Normalize, Slerp, UpdateMatrix, vector helpers): %s against the translated originals (40000 random cases)\n",
            bad ? "partly differ (see above)" : "match");
    hle_heap_free(hle_ptr(mem));
    return 0;
}
