/*
 * native_misc.c - native replacement for I3D_singlemesh::UpdateBVolumeProc
 * (LS3DF 0x10054810): the bound volume of a skinned single mesh, the union of
 * each joint's box transformed by the joint's skinning matrix.
 *
 * Exactness model as in the other native_*.c files: the translated original
 * keeps x87 values as doubles and rounds to float at dword stores; this file is
 * compiled with -ffp-contract=off. Calls the original makes to code not
 * replaced here go through the guest (dispatch); S_vector*S_matrix uses the
 * validated fam_vmm.
 *
 * fam_native_misc_check (FAMILIA_NATIVE_CHECK=1): differential test against the
 * translated original on random meshes and joint hierarchies.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

int fam_native_misc_on = 1;
void fam_vmm(const float *V, const float *M, float *R);
void recomp_ls3df_10054810_orig(void);

static inline float ldf(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static inline void stf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

static recomp_func_t lookup(uint32_t va) {
    recomp_func_t f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup(va);
    if (!f) { fprintf(stderr, "[native] %08X not found\n", va); abort(); }
    return f;
}
static void guest_this0(uint32_t fn, uint32_t self) {
    g_ecx = self;
    PUSH32(g_esp, RECOMP_RETADDR);
    lookup(fn)();
}
/* UpdateMatrix (0x1001A6E0): a no-op when both "local matrix" bits are set */
static void update_matrix(uint32_t frm) {
    if ((MEM32(frm + 0xAC) & 0x14) != 0x14) guest_this0(0x1001A6E0u, frm);
}
/* m = m * b (S_matrix::operator*, 0x1002D3A0, ret 0xC), through a guest temporary */
static void mat_mul_into(uint32_t m, uint32_t b) {
    uint32_t esp0 = g_esp;
    g_esp -= 64;
    uint32_t tmp = g_esp;
    PUSH32(g_esp, b); PUSH32(g_esp, tmp); PUSH32(g_esp, m); PUSH32(g_esp, RECOMP_RETADDR);
    lookup(0x1002D3A0u)();
    memmove((void *)ADDR(m), (const void *)ADDR(tmp), 64);
    g_esp = esp0;
}
/* BBoxVertices (0x1006BC00): the 8 corners into its static buffer, in its order */
static uint32_t bbox_vertices(uint32_t box) {
    if (!(MEM8(0x101C5290u) & 1)) { guest_this0(0x1006BC00u, box); return 0x101C5230u; }
    uint32_t out = 0x101C5230u;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            MEM32(out + 0) = MEM32(box + (i ? 0x0 : 0xC));
            MEM32(out + 4) = MEM32(box + (j ? 0x4 : 0x10));
            MEM32(out + 8) = MEM32(box + 0x8);
            MEM32(out + 12) = MEM32(box + (i ? 0x0 : 0xC));
            MEM32(out + 16) = MEM32(box + (j ? 0x4 : 0x10));
            MEM32(out + 20) = MEM32(box + 0x14);
            out += 24;
        }
    return 0x101C5230u;
}
/* the joint-type dispatch of the parent walk: 0 stop, 1 joint, 2 multiply and go on */
static const uint8_t k_walk[10] = { 0, 2, 2, 2, 0, 2, 2, 2, 0, 1 };

static inline uint32_t new_flags(uint32_t self) { return (MEM32(self + 0x1D8) & 0xFFFBFFFFu) | 0x20000u; }

/* void I3D_singlemesh::UpdateBVolumeProc()  -- LS3DF 0x10054810, thiscall, ret */
void fam_native_singlemesh_bvol(void) {
    if (!fam_native_misc_on) { recomp_ls3df_10054810_orig(); return; }
    uint32_t self = g_ecx;
    MEM32(self + 0x228) = MEM32(self + 0x224);
    if (MEM32(self + 0x220) == 0 && !(MEM32(self + 0x1D8) & 0x10000)) guest_this0(0x10054C20u, self);
    uint32_t mesh = MEM32(self + 0x1E0);
    if (!mesh) {
        MEM32(self + 0x1B4) = 0x5A0E1BCAu; MEM32(self + 0x1B0) = 0x5A0E1BCAu; MEM32(self + 0x1AC) = 0x5A0E1BCAu;
        MEM32(self + 0x1C0) = 0xDA0E1BCAu; MEM32(self + 0x1BC) = 0xDA0E1BCAu; MEM32(self + 0x1B8) = 0xDA0E1BCAu;
        uint32_t f = new_flags(self);
        MEM32(self + 0x1D0) = 0;
        MEM32(self + 0x1D8) = f;
        g_eax = 0xDA0E1BCAu; g_ecx = f;
        g_esp += 4;
        return;
    }
    uint32_t o = MEM32(self + 0x1E4), x = o ? MEM32(o + 0x30) : 0;
    if (!o || !x) {
        memmove((void *)ADDR(self + 0x1AC), (const void *)ADDR(mesh + 0x34), 40);
        uint32_t f = new_flags(self);
        MEM32(self + 0x1D8) = f;
        if (!o) { g_eax = 0; g_edx = f; } else g_eax = f;
        g_ecx = 0;
        g_esp += 4;
        return;
    }
    float L[6];
    memcpy(L, (const void *)ADDR(x + 0x10), 24);
    uint32_t jl = MEM32(self + 0x220), arr;
    if (jl && (arr = MEM32(x + 8)) != 0 && (int32_t)MEM32(self + 0x21C) > 0) {
        for (int32_t i = 0; i < (int32_t)MEM32(self + 0x21C); i++) {
            uint32_t box = arr + 0x4C + 0x68u * (uint32_t)i, jp = jl + 4u * (uint32_t)i;
            if (!MEM32(jp)) continue;
            if (!((double)ldf(box) <= (double)ldf(box + 0xC))) continue;
            uint32_t v = bbox_vertices(box);
            uint32_t joint = MEM32(jp), m = joint + 0x1C4;
            uint32_t f = MEM32(joint + 0x204);
            if (!(f & 0x10)) {
                MEM32(joint + 0x204) = f | 0x10;
                update_matrix(joint);
                memmove((void *)ADDR(m), (const void *)ADDR(joint + 0x50), 64);
                for (uint32_t p = MEM32(joint + 0x120); p; p = MEM32(p + 0x120)) {
                    uint32_t t = MEM32(p + 0x110) - 1u;
                    uint8_t k = t > 9 ? 2 : k_walk[t];
                    if (k == 0) break;
                    if (k == 1 && (MEM8(p + 0x204) & 0x10)) { mat_mul_into(m, p + 0x1C4); break; }
                    update_matrix(p);
                    mat_mul_into(m, p + 0x50);
                }
            }
            for (int c = 0; c < 8; c++) {
                float r[3];
                fam_vmm((const float *)ADDR(v + 12u * (uint32_t)c), (const float *)ADDR(m), r);
                for (int a = 0; a < 3; a++) {               /* min takes when cur > p, max when cur < p */
                    if ((double)L[a] > (double)r[a]) L[a] = r[a];
                    if ((double)L[3 + a] < (double)r[a]) L[3 + a] = r[a];
                }
            }
        }
    }
    /* the box, its sphere (as the original: z delta and x half rounded to float, y/z halves kept double) */
    memcpy((void *)ADDR(self + 0x1AC), L, 24);
    double dx = (double)L[3] - L[0], dy = (double)L[4] - L[1];
    float dz = (float)((double)L[5] - L[2]);
    float hx = (float)(dx * 0.5);
    double hy = dy * 0.5, hz = (double)dz * 0.5;
    stf(self + 0x1C4, (float)((double)L[0] + hx));
    stf(self + 0x1C8, (float)((double)L[1] + hy));
    float pz = (float)((double)L[2] + hz);
    stf(self + 0x1CC, pz);
    MEM32(self + 0x1D8) = new_flags(self);
    stf(self + 0x1D0, (float)sqrt((hz * hz + hy * hy) + (double)hx * (double)hx));
    /* DiscardHRBoundVol (0x1001CC00), with the registers it leaves */
    uint32_t eax = MEM32(self + 0x110), ecx = self;
    if (eax != 5) {
        MEM32(self + 0xAC) &= 0xFFFFFEBFu;
        for (eax = MEM32(self + 0x120); eax; eax = MEM32(eax + 0x120)) {
            ecx = MEM32(eax + 0xAC);
            if (!(ecx & 0x40) || MEM32(eax + 0x110) == 5) break;
            ecx &= 0xFFFFFEBFu;
            MEM32(eax + 0xAC) = ecx;
        }
    }
    g_eax = eax; g_ecx = ecx; g_edx = 5;
    g_esp += 4;
}

/* ---------------------------------------------------- differential check */
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 33) % n; }
static float rndf(float lo, float hi) {
    uint32_t k = rnd(50);
    if (k == 0) return 0.0f;
    if (k == 1) return -0.0f;
    return lo + (hi - lo) * (float)rnd(1u << 24) / (float)(1u << 24);
}

#define MS_SIZE 0x6000
int fam_native_misc_check(void) {
    uint32_t A = hle_va(hle_heap_alloc(MS_SIZE, 1));
    enum { NJ = 12, JS = 0x240 };
    uint32_t self = A, mesh = A + 0x240, o = A + 0x300, x = A + 0x340, arr = A + 0x380;
    uint32_t jl = A + 0x900, joints = A + 0xA00;                  /* NJ joints of JS bytes */
    uint8_t *s0 = malloc(MS_SIZE), *r1 = malloc(MS_SIZE);
    uint8_t bb_saved[96], bb1[96];
    memcpy(bb_saved, (void *)ADDR(0x101C5230u), 96);
    int bad = 0, n;
    for (n = 0; n < 3000 && bad < 5; n++) {
        memset((void *)ADDR(A), 0, MS_SIZE);
        for (int k = 0; k < 10; k++) stf(mesh + 0x34 + 4 * k, rndf(-50, 50));
        MEM32(self + 0x1E0) = rnd(20) ? mesh : 0;
        MEM32(self + 0x1E4) = rnd(20) ? o : 0;
        MEM32(o + 0x30) = rnd(20) ? x : 0;
        MEM32(self + 0x1D8) = rnd(0x40000) | 0x10000 | (rnd(2) << 18);
        MEM32(self + 0x224) = rnd(1000); MEM32(self + 0x110) = rnd(4) ? 1 : 5;
        MEM32(self + 0xAC) = rnd(0x200); MEM32(self + 0x120) = rnd(2) ? joints + (NJ - 1) * JS : 0;
        uint32_t nj = rnd(NJ);
        MEM32(self + 0x21C) = rnd(15) ? nj : (uint32_t)-(int)rnd(3);
        MEM32(self + 0x220) = rnd(15) ? jl : 0;
        MEM32(x + 8) = rnd(15) ? arr : 0;
        for (int k = 0; k < 3; k++) { float a = rndf(-30, 30), b = rndf(-30, 30);
                                      if (rnd(10) && a > b) { float t = a; a = b; b = t; }
                                      stf(x + 0x10 + 4 * k, a); stf(x + 0x1C + 4 * k, b); }
        for (uint32_t i = 0; i < NJ; i++) {
            uint32_t b = arr + 0x4C + 0x68 * i;
            for (int k = 0; k < 3; k++) { float a = rndf(-5, 5), c = rndf(-5, 5);
                                          if (rnd(8) && a > c) { float t = a; a = c; c = t; }
                                          stf(b + 4 * k, a); stf(b + 0xC + 4 * k, c); }
            uint32_t j = joints + i * JS;
            MEM32(jl + 4 * i) = rnd(10) ? j : 0;
            for (int k = 0; k < 16; k++) { stf(j + 0x50 + 4 * k, rndf(-2, 2)); stf(j + 0x1C4 + 4 * k, rndf(-2, 2)); stf(j + 0x10 + 4 * k, rndf(-2, 2)); }
            for (int k = 0; k < 7; k++) stf(j + 0x90 + 4 * k, rndf(-1.5f, 1.5f));      /* scale, rotation */
            MEM32(j + 0xAC) = rnd(6) ? 0x14 | rnd(0x100) : rnd(0x100);
            MEM32(j + 0x204) = rnd(4) ? 0 : 0x10 | rnd(0x20);
            static const uint32_t types[] = { 10, 10, 10, 6, 1, 5, 9, 2, 0, 12 };
            MEM32(j + 0x110) = types[rnd(10)];
            MEM32(j + 0x120) = i + 1 < NJ && rnd(5) ? joints + (i + 1 + rnd(NJ - i - 1)) * JS : 0;   /* parents later in the array: no cycles */
        }
        memcpy(s0, (void *)ADDR(A), MS_SIZE);
        uint32_t e[2], d[2], esps[2]; int stb[2];
        for (int pass = 0; pass < 2; pass++) {
            memcpy((void *)ADDR(A), s0, MS_SIZE);
            memcpy((void *)ADDR(0x101C5230u), bb_saved, 96);
            uint32_t esp0 = g_esp;
            int st0 = g_st_base;
            PUSH32(g_esp, RECOMP_RETADDR);
            g_ecx = self; g_eax = 0x11111111; g_edx = 0x22222222;
            (pass ? fam_native_singlemesh_bvol : recomp_ls3df_10054810_orig)();
            e[pass] = g_eax; d[pass] = g_edx; esps[pass] = g_esp - esp0; stb[pass] = g_st_base - st0;
            g_esp = esp0;
            if (!pass) { memcpy(r1, (void *)ADDR(A), MS_SIZE); memcpy(bb1, (void *)ADDR(0x101C5230u), 96); }
        }
        int nd = 0;
        for (uint32_t off = 0; off < MS_SIZE; off += 4) {
            uint32_t a, b; memcpy(&a, r1 + off, 4); b = MEM32(A + off);
            if (a != b) { if (!bad && nd < 3) fprintf(stderr, "[native]   singlemesh +%04X: %08X / %08X\n", off, a, b); nd++; }
        }
        if (memcmp(bb1, (void *)ADDR(0x101C5230u), 96)) nd++;
        if (nd || e[0] != e[1] || d[0] != d[1] || esps[0] != esps[1] || stb[0] || stb[1]) {
            if (!bad) fprintf(stderr, "[native] I3D_singlemesh::UpdateBVolumeProc MISMATCH (case %d): %d words, eax %08X/%08X edx %08X/%08X esp %d/%d x87 %d/%d\n",
                              n, nd, e[0], e[1], d[0], d[1], (int)esps[0], (int)esps[1], stb[0], stb[1]);
            bad++;
        }
    }
    memcpy((void *)ADDR(0x101C5230u), bb_saved, 96);
    fprintf(stderr, "[native] I3D_singlemesh::UpdateBVolumeProc: %s against the translated original (%d random meshes)\n",
            bad ? "DIFFERS (disabled)" : "matches", n);
    free(s0); free(r1);
    hle_heap_free(hle_ptr(A));
    if (bad) fam_native_misc_on = 0;
    return 0;
}
