/*
 * native_crt.c - native replacements for hot MSVC C-runtime routines that
 * Game.exe and LS3DF.dll link statically (each module has its own copy).
 *
 * The string routines follow the C-locale fast path of the originals
 * (Game.exe 0x6D87BC / LS3DF 0x101CAA7C: the locale's LC_CTYPE handle at +8
 * is 0); with any other locale the translated original runs. __CIfmod works
 * on the x87 stack as the original does: st1 = x, st0 = y, both popped,
 * fmod(x, y) pushed (exact, as fprem is).
 *
 * fam_native_crt_check (FAMILIA_NATIVE_CHECK=1) compares each with its
 * translated original on random inputs.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

/* the global x87 window (runtime code: synced at every call from lifted code) */
#define XPOP()  fp_pop_impl(g_st, &g_fp_top)
#define XPUSH(v) fp_push_impl(g_st, &g_fp_top, (v))

static inline int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 0x20 : c; }

/* int __cdecl _strcmpi(const char *a, const char *b): -1, 0 or 1 */
static int strcmpi_c(const uint8_t *a, const uint8_t *b) {
    for (;; a++, b++) {
        int ca = *a, cb = *b;
        if (ca == cb) { if (!cb) return 0; continue; }
        int la = lower(ca), lb = lower(cb);
        if (la == lb) continue;
        return la < lb ? -1 : 1;
    }
}
/* int __cdecl _strnicmp(const char *a, const char *b, size_t n): -1, 0 or 1 */
static int strnicmp_c(const uint8_t *a, const uint8_t *b, uint32_t n) {
    if (!n) return 0;
    int ca, cb;
    do {
        ca = *a++; cb = *b++;
        if (!ca || !cb) break;
        ca = lower(ca); cb = lower(cb);
        if (ca != cb) return ca < cb ? -1 : 1;
    } while (--n);
    return ca == cb ? 0 : ca < cb ? -1 : 1;
}

void recomp_game_0061D0B0_orig(void);
void recomp_game_0061D000_orig(void);
void recomp_ls3df_10098830_orig(void);
void recomp_ls3df_1008D9FA_orig(void);

int fam_native_crt_on = 1;
#define C_LOCALE(addr) (fam_native_crt_on && MEM32((addr) + 8) == 0)

void fam_native_strcmpi_game(void) {
    if (!C_LOCALE(0x6D87BCu)) { recomp_game_0061D0B0_orig(); return; }
    g_eax = (uint32_t)strcmpi_c((const uint8_t *)ADDR(MEM32(g_esp + 4)), (const uint8_t *)ADDR(MEM32(g_esp + 8)));
    g_esp += 4;
}
void fam_native_strcmpi_ls3df(void) {
    if (!C_LOCALE(0x101CAA7Cu)) { recomp_ls3df_10098830_orig(); return; }
    g_eax = (uint32_t)strcmpi_c((const uint8_t *)ADDR(MEM32(g_esp + 4)), (const uint8_t *)ADDR(MEM32(g_esp + 8)));
    g_esp += 4;
}
void fam_native_strnicmp_game(void) {
    if (!C_LOCALE(0x6D87BCu)) { recomp_game_0061D000_orig(); return; }
    g_eax = (uint32_t)strnicmp_c((const uint8_t *)ADDR(MEM32(g_esp + 4)), (const uint8_t *)ADDR(MEM32(g_esp + 8)), MEM32(g_esp + 12));
    g_esp += 4;
}

/* __CIfmod (LS3DF 0x1008D9FA): x87 st1 = x, st0 = y -> st0 = fmod(x, y) */
void fam_native_cifmod_ls3df(void) {
    if (!fam_native_crt_on) { recomp_ls3df_1008D9FA_orig(); return; }
    double y = XPOP(), x = XPOP();
    if (!isfinite(x) || !isfinite(y) || y == 0.0) {     /* NaN/Inf/0 classes: let the original decide */
        XPUSH(x); XPUSH(y);
        recomp_ls3df_1008D9FA_orig();
        return;
    }
    XPUSH(fmod(x, y));
    g_esp += 4;
}

/* ---------------------------------------------------- differential check */
static uint64_t g_rng = 0xA0761D6478BD642Full;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 33) % n; }

static uint32_t call_cdecl(void (*fn)(void), int nargs, const uint32_t *args, int *esp_ok) {
    uint32_t esp0 = g_esp;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    fn();
    *esp_ok = g_esp == esp0 - 4 * (uint32_t)nargs;
    g_esp = esp0;
    return g_eax;
}

int fam_native_crt_check(void) {
    uint32_t mem = hle_va(hle_heap_alloc(256, 1));
    char *A = (char *)ADDR(mem), *B = (char *)ADDR(mem + 128);
    static const char alpha[] = "aAbBzZ_[`@{0\xc3\xa9";
    int bad = 0;
    for (int n = 0; n < 30000 && bad < 5; n++) {
        int la = (int)rnd(12), lb = rnd(3) ? la : (int)rnd(12);
        for (int i = 0; i < la; i++) A[i] = alpha[rnd(sizeof alpha - 1)];
        A[la] = 0;
        for (int i = 0; i < lb; i++) B[i] = rnd(3) ? (i < la ? (rnd(2) ? A[i] : (char)(A[i] ^ 0x20)) : 'q') : alpha[rnd(sizeof alpha - 1)];
        B[lb] = 0;
        uint32_t args[3] = { mem, mem + 128, rnd(14) };
        int k = (int)rnd(3), o1, o2;
        void (*orig)(void) = k == 0 ? recomp_game_0061D0B0_orig : k == 1 ? recomp_ls3df_10098830_orig : recomp_game_0061D000_orig;
        void (*nat)(void) = k == 0 ? fam_native_strcmpi_game : k == 1 ? fam_native_strcmpi_ls3df : fam_native_strnicmp_game;
        uint32_t r1 = call_cdecl(orig, k == 2 ? 3 : 2, args, &o1), r2 = call_cdecl(nat, k == 2 ? 3 : 2, args, &o2);
        if (r1 != r2 || !o1 || !o2) {
            if (!bad) fprintf(stderr, "[native] %s MISMATCH: '%s' '%s' n=%u -> %d/%d\n",
                              k == 2 ? "_strnicmp" : "_strcmpi", A, B, args[2], (int)r1, (int)r2);
            bad++;
        }
    }
    for (int n = 0; n < 30000 && bad < 5; n++) {
        double x = ldexp((double)(int32_t)(rnd(0x7FFFFFFF) - 0x3FFFFFFF), (int)rnd(40) - 50);
        double y = rnd(4) ? 6.283185307179586 : ldexp((double)(int32_t)(rnd(0x7FFFFFFF) - 0x3FFFFFFF), (int)rnd(40) - 50);
        if (!rnd(50)) y = 0.0;
        double r[2];
        for (int pass = 0; pass < 2; pass++) {
            uint32_t esp0 = g_esp;
            XPUSH(x); XPUSH(y);
            PUSH32(g_esp, RECOMP_RETADDR);
            (pass ? fam_native_cifmod_ls3df : recomp_ls3df_1008D9FA_orig)();
            r[pass] = XPOP();
            if (g_esp != esp0) { bad++; fprintf(stderr, "[native] __CIfmod esp %d\n", pass); }
            g_esp = esp0;
        }
        if (memcmp(&r[0], &r[1], 8) && !(isnan(r[0]) && isnan(r[1]))) {
            if (!bad) fprintf(stderr, "[native] __CIfmod MISMATCH: fmod(%a, %a) = %a / %a\n", x, y, r[0], r[1]);
            bad++;
        }
    }
    fprintf(stderr, "[native] CRT _strcmpi/_strnicmp/__CIfmod: %s against the translated originals\n",
            bad ? "DIFFER (native CRT routines disabled)" : "match");
    hle_heap_free(hle_ptr(mem));
    if (bad) fam_native_crt_on = 0;
    return 0;
}

/* ------------------------------------------------ software shadow raster */
/* void I3D_driver::DrawShadowTrianglesSW(const int *verts, const ushort *indx, uint num_faces)
 * -- LS3DF 0x100160D0, thiscall, ret 0xC. Integer only: per triangle a span
 * table (256 rows of min,max bytes at 0x100BFDA8) is reset and filled from the
 * three edges in 7-bit fixed point, then rows ymin..ymax-1 of the 256-wide
 * buffer at [0x100A69DC] get (uint8)(max-min) bytes of 0xFF from min. */
#define SPAN 0x100BFDA8u
static inline void span_edge(int32_t xs, int32_t ys, int32_t xe, int32_t ye) {
    int32_t slope = (int32_t)((uint32_t)(xe - xs) << 7) / (ye - ys);
    uint32_t acc = (uint32_t)xs << 7;
    for (int32_t y = ys; y <= ye; y++, acc += (uint32_t)slope) {
        int32_t x = (int32_t)acc >> 7;
        uint32_t row = SPAN + (uint32_t)y * 2u;
        if (x < (int32_t)MEM8(row)) MEM8(row) = (uint8_t)x;
        if (x > (int32_t)MEM8(row + 1)) MEM8(row + 1) = (uint8_t)x;
    }
}
void recomp_ls3df_100160D0_orig(void);
void fam_native_shadow_tris_sw(void) {
    if (!fam_native_crt_on) { recomp_ls3df_100160D0_orig(); return; }
    uint32_t verts = MEM32(g_esp + 4), idx = MEM32(g_esp + 8), n = MEM32(g_esp + 12);
    for (uint32_t f = n; f; f--, idx += 6) {
        uint32_t i0 = MEM16(idx), i1 = MEM16(idx + 2), i2 = MEM16(idx + 4);
        int32_t x0 = (int32_t)MEM32(verts + i0 * 8), y0 = (int32_t)MEM32(verts + i0 * 8 + 4);
        int32_t x1 = (int32_t)MEM32(verts + i1 * 8), y1 = (int32_t)MEM32(verts + i1 * 8 + 4);
        int32_t x2 = (int32_t)MEM32(verts + i2 * 8), y2 = (int32_t)MEM32(verts + i2 * 8 + 4);
        for (uint32_t k = 0; k < 128; k++) MEM32(SPAN + 4 * k) = 0x00FF00FFu;
        int32_t ymin = y0, ymax = y0;
        if (y1 < y0) ymin = y1;
        if (y2 < ymin) ymin = y2;
        if (y1 > y0) ymax = y1;
        if (y2 > ymax) ymax = y2;
        if (y1 != y0) { if (y1 < y0) span_edge(x1, y1, x0, y0); else span_edge(x0, y0, x1, y1); }
        if (y2 != y1) { if (y2 < y1) span_edge(x2, y2, x1, y1); else span_edge(x1, y1, x2, y2); }
        if (y0 != y2) { if (y0 < y2) span_edge(x0, y0, x2, y2); else span_edge(x2, y2, x0, y0); }
        uint32_t rows = (uint32_t)(ymax - ymin);
        uint32_t dst = (uint32_t)ymin * 256u + MEM32(0x100A69DCu), sp = SPAN + (uint32_t)ymin * 2u;
        for (; rows; rows--, dst += 256, sp += 2) {
            uint8_t mn = MEM8(sp), cnt = (uint8_t)(MEM8(sp + 1) - mn);
            if (cnt) memset((void *)ADDR(dst + mn), 0xFF, cnt);
        }
    }
    g_eax = 0;
    if (n) g_ecx = idx;
    g_esp += 4 + 12;
}

int fam_native_shadow_check(void) {
    enum { NV = 16, NI = 3 * 24 };
    uint32_t mem = hle_va(hle_heap_alloc(2 * 0x10000 + NV * 8 + NI * 2 + 64, 1));
    uint32_t bufA = mem, bufB = mem + 0x10000, verts = mem + 0x20000, idx = verts + NV * 8;
    uint32_t saved_ptr = MEM32(0x100A69DCu);
    uint8_t span_save[512]; memcpy(span_save, (void *)ADDR(SPAN), 512);
    int bad = 0;
    for (int n = 0; n < 4000 && bad < 3; n++) {
        for (int i = 0; i < NV; i++) {
            int out = !rnd(20);
            MEM32(verts + i * 8) = (uint32_t)(out ? (int)rnd(300) - 20 : (int)rnd(128));
            MEM32(verts + i * 8 + 4) = (uint32_t)(int)rnd(126);
        }
        uint32_t nf = rnd(24);
        for (uint32_t i = 0; i < nf * 3; i++) MEM16(idx + 2 * i) = (uint16_t)rnd(NV);
        memset((void *)ADDR(bufA), 0x11, 0x10000); memcpy((void *)ADDR(bufB), (void *)ADDR(bufA), 0x10000);
        uint32_t r[2], ecxr[2], spans[2][128];
        for (int pass = 0; pass < 2; pass++) {
            MEM32(0x100A69DCu) = pass ? bufB : bufA;
            uint32_t esp0 = g_esp;
            PUSH32(g_esp, nf); PUSH32(g_esp, idx); PUSH32(g_esp, verts); PUSH32(g_esp, RECOMP_RETADDR);
            g_ecx = 0x12345678;
            (pass ? fam_native_shadow_tris_sw : recomp_ls3df_100160D0_orig)();
            if (g_esp != esp0) bad++;
            g_esp = esp0; r[pass] = g_eax; ecxr[pass] = g_ecx;
            memcpy(spans[pass], (void *)ADDR(SPAN), 512);
        }
        if (memcmp((void *)ADDR(bufA), (void *)ADDR(bufB), 0x10000) || r[0] != r[1] || ecxr[0] != ecxr[1] || memcmp(spans[0], spans[1], 512)) {
            if (!bad) fprintf(stderr, "[native] DrawShadowTrianglesSW MISMATCH (case %d, %u faces): eax %X/%X ecx %X/%X\n", n, nf, r[0], r[1], ecxr[0], ecxr[1]);
            bad++;
        }
    }
    MEM32(0x100A69DCu) = saved_ptr; memcpy((void *)ADDR(SPAN), span_save, 512);
    fprintf(stderr, "[native] DrawShadowTrianglesSW: %s against the translated original (4000 random meshes)\n", bad ? "DIFFERS (disabled)" : "matches");
    hle_heap_free(hle_ptr(mem));
    if (bad) fam_native_crt_on = 0;
    return 0;
}

/* --------------------------------------------- Game.exe's tiny vector ops */
static inline float ldf32(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static inline void stf32(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }
void recomp_game_00402A30_orig(void);
void recomp_game_00407570_orig(void);
void recomp_game_0040ADD0_orig(void);
/* S_vector::S_vector(float x, float y, float z)  -- 0x402A30, thiscall, ret 0xC */
void fam_native_svec_ctor(void) {
    if (!fam_native_crt_on) { recomp_game_00402A30_orig(); return; }
    uint32_t t = g_ecx, a1 = MEM32(g_esp + 4), a2 = MEM32(g_esp + 8), a3 = MEM32(g_esp + 12);
    MEM32(t) = a1; MEM32(t + 4) = a2; MEM32(t + 8) = a3;
    g_eax = t; g_ecx = a3; g_edx = a2;
    g_esp += 4 + 12;
}
/* float S_vector::Dot(const S_vector &v) const  -- 0x407570, thiscall, ret 4: st0 = (z*z' + y*y') + x*x' */
void fam_native_svec_dot(void) {
    if (!fam_native_crt_on) { recomp_game_00407570_orig(); return; }
    uint32_t t = g_ecx, v = MEM32(g_esp + 4);
    double r = (double)ldf32(v + 8) * (double)ldf32(t + 8);
    r = r + (double)ldf32(v + 4) * (double)ldf32(t + 4);
    r = r + (double)ldf32(v) * (double)ldf32(t);
    XPUSH(r);
    g_eax = v;
    g_esp += 4 + 4;
}
/* S_vector S_vector::operator*(const float &f) const  -- 0x40ADD0, thiscall, ret 8 */
void fam_native_svec_mul_f(void) {
    if (!fam_native_crt_on) { recomp_game_0040ADD0_orig(); return; }
    uint32_t t = g_ecx, ret = MEM32(g_esp + 4), fp = MEM32(g_esp + 8);
    double f = ldf32(fp);
    float z = (float)((double)ldf32(t + 8) * f), y = (float)((double)ldf32(t + 4) * f), x = (float)(f * (double)ldf32(t));
    stf32(ret, x); stf32(ret + 4, y); stf32(ret + 8, z);
    g_eax = ret;
    g_esp += 4 + 8;
}
int fam_native_svec_check(void) {
    uint32_t mem = hle_va(hle_heap_alloc(64, 1));
    int bad = 0;
    for (int n = 0; n < 30000 && bad < 3; n++) {
        for (int i = 0; i < 16; i++) { float f = ldexpf((float)((int)rnd(2000001) - 1000000) / 1e6f, (int)rnd(40) - 20); stf32(mem + 4 * i, f); }
        int k = (int)rnd(3);
        uint32_t retp = rnd(2) ? mem : mem + 40;      /* the result may alias the source */
        uint32_t snap[16], r1[16], e1 = 0, e2 = 0; double d1 = 0, d2 = 0; int ok1, ok2;
        memcpy(snap, (void *)ADDR(mem), 64);
        for (int pass = 0; pass < 2; pass++) {
            memcpy((void *)ADDR(mem), snap, 64);
            uint32_t esp0 = g_esp;
            if (k == 0) { PUSH32(g_esp, MEM32(mem + 24)); PUSH32(g_esp, MEM32(mem + 20)); PUSH32(g_esp, MEM32(mem + 16)); }
            else if (k == 1) PUSH32(g_esp, mem + 16);
            else { PUSH32(g_esp, mem + 32); PUSH32(g_esp, retp); }
            PUSH32(g_esp, RECOMP_RETADDR);
            g_ecx = mem;
            void (*fn)(void) = k == 0 ? (pass ? fam_native_svec_ctor : recomp_game_00402A30_orig)
                             : k == 1 ? (pass ? fam_native_svec_dot : recomp_game_00407570_orig)
                                      : (pass ? fam_native_svec_mul_f : recomp_game_0040ADD0_orig);
            fn();
            if (pass) ok2 = g_esp == esp0; else ok1 = g_esp == esp0;
            g_esp = esp0;
            if (k == 1) { double d = XPOP(); if (pass) d2 = d; else d1 = d; }
            if (pass) e2 = g_eax; else { e1 = g_eax; memcpy(r1, (void *)ADDR(mem), 64); }
        }
        if (memcmp(r1, (void *)ADDR(mem), 64) || e1 != e2 || !ok1 || !ok2 || memcmp(&d1, &d2, 8)) {
            if (!bad) {
                fprintf(stderr, "[native] S_vector op %d MISMATCH (case %d): eax %X/%X esp %d/%d\n", k, n, e1, e2, ok1, ok2);
                for (int i = 0; i < 16; i++) if (r1[i] != MEM32(mem + 4 * i)) fprintf(stderr, "    word %d: %08X/%08X (input %08X)\n", i, r1[i], MEM32(mem + 4 * i), snap[i]);
            }
            bad++;
        }
    }
    fprintf(stderr, "[native] Game S_vector ctor/Dot/operator*(f): %s against the translated originals\n", bad ? "DIFFER (disabled)" : "match");
    hle_heap_free(hle_ptr(mem));
    if (bad) fam_native_crt_on = 0;
    return 0;
}
