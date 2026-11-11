/*
 * native_cline.c - native g_collision::LineInTreeLevel (Game.exe 0x005C66A0,
 * thiscall, two arguments, ret 8): a line against the volumes of one cell of
 * the static collision tree. Every line test of the game (AI sight and paths,
 * the camera, bullets, the cars' probes) walks cells through it; about 0.7% of
 * a Free Ride frame translated, with its callees' share more.
 *
 * Structure from the mafia-re decompilation (collision/g_c_line.cpp, a map);
 * arithmetic from the x86, operation for operation (doubles where the
 * original keeps the x87 stack, float where it stores: the plane distance t,
 * the hit point's offsets and the triangle's edges, u). Volumes that are not
 * faces go to the translated LineInNonface (0x5C60E0), as in the original.
 *
 *   cell (+0 count, +4 volumes); face (+0 flags, +4/+8/+C vertices 0..2,
 *   +10 normal, +1C d); query p (+0 from, +C dir, +1C/+24 min/max x, +20/+28
 *   min/max z, +38 flags2, +40 t, +48 normal, +54 hit).
 *
 * FAMILIA_NATIVE_CHECK=1 (or FAMILIA_CLINE_CHECK=N): the first calls run the
 * translated original, then this on the same query, and compare the query and
 * the result; a difference switches this off.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

void recomp_game_005C66A0_orig(void);
int fam_native_cline_on = 1;

static inline double lf(uint32_t va) { float f; memcpy(&f, (const void *)ADDR(va), 4); return f; }
static inline float ff(double d) { return (float)d; }
static inline uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static recomp_func_t guest(uint32_t va) {
    recomp_func_t f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup(va);
    if (!f) { fprintf(stderr, "[native] cline: %08X not found\n", va); abort(); }
    return f;
}

#define K_EPS  0x0062323Cu      /* 1e-8 */
#define K_ZERO 0x00623230u
#define K_ONE  0x00623238u

static uint8_t line_in_tree_level(uint32_t self, uint32_t cell, uint32_t p, uint32_t *eax_out) {
    int32_t n = (int32_t)MEM32(cell);
    uint32_t vols = MEM32(cell + 4);
    uint8_t hit = 0;
    if (n <= 0) { *eax_out = vols & 0xFFFFFF00u; return 0; }
    const double eps = lf(K_EPS), zero = lf(K_ZERO), one = lf(K_ONE);
    for (; n > 0; n--, vols += 4) {
        uint32_t f = MEM32(vols);
        uint32_t flags = MEM32(f);
        if (MEM32(p + 0x38) & flags) continue;
        if (flags & 0xA0) {                                          /* not a face, or disabled */
            uint32_t esp0 = g_esp;
            g_ecx = self;
            PUSH32(g_esp, p); PUSH32(g_esp, f); PUSH32(g_esp, RECOMP_RETADDR);
            guest(0x005C60E0u)();                                    /* LineInNonface, ret 8 */
            g_esp = esp0;
            hit |= (uint8_t)g_eax;
            continue;
        }
        uint32_t a = MEM32(f + 4), b = MEM32(f + 8), c = MEM32(f + 0xC);
        if (lf(c) < lf(p + 0x1C)) continue;                          /* beyond the line's x bounds */
        double ax = lf(a);
        if (ax > lf(p + 0x24)) break;                                  /* sorted by x: none further (not when unordered) */
        /* the face's z range: st = the lower, hi = the higher (a float slot) */
        double lo = lf(a + 8); float hi = (float)lo;
        double z = lf(c + 8);
        if (z < lo) lo = z;
        else if (z > (double)hi) hi = (float)z;
        z = lf(b + 8);
        if (z < lo) lo = z;
        else if (z > (double)hi) hi = (float)z;
        if (!(lo <= lf(p + 0x28))) continue;
        if (!((double)hi >= lf(p + 0x20))) continue;                  /* unordered skips too */
        double d = ((double)lf(p + 0x14) * lf(f + 0x18) + (double)lf(p + 0x10) * lf(f + 0x14)) + (double)lf(p + 0xC) * lf(f + 0x10);
        if (d < eps) continue;
        double num = (((double)lf(f + 0x14) * lf(p + 4) + (double)lf(f + 0x18) * lf(p + 8)) + (double)lf(f + 0x10) * lf(p)) + lf(f + 0x1C);
        float t = ff(-(num / d));
        if ((double)t < zero) continue;
        double pt_t = lf(p + 0x40);
        if (pt_t < (double)t || pt_t == (double)t) continue;
        /* the place on the plane, relative to vertex 0, and the edges */
        float tdz = ff((double)t * lf(p + 0x14));
        float ptx = ff((double)t * lf(p + 0xC) + lf(p));
        double pty = (double)t * lf(p + 0x10) + lf(p + 4);
        double ptz = (double)tdz + lf(p + 8);
        float px = ff((double)ptx - lf(a)), pz = ff(ptz - lf(a + 8)), py = ff(pty - lf(a + 4));
        float e1x = ff((double)lf(b) - lf(a)), e2x = ff((double)lf(c) - lf(a));
        float e1z = ff((double)lf(b + 8) - lf(a + 8)), e2z = ff((double)lf(c + 8) - lf(a + 8));
        float e1y = ff((double)lf(b + 4) - lf(a + 4)), e2y = ff((double)lf(c + 4) - lf(a + 4));
        double ud; float u; double v;
        switch ((uint8_t)flags) {
        case 1: ud = (double)py / e2y; goto tail_z;                                            /* X_LEVEL */
        case 3: ud = (double)px / e2x; goto tail_z;                                            /* Y_ALONG_Z */
        case 2: ud = ((double)e1y * pz - (double)e1z * py) / ((double)e1y * e2z - (double)e2y * e1z); goto tail_y;   /* X */
        case 6: ud = (double)px / e2x; goto tail_y;                                            /* Z_UPRIGHT */
        case 4: ud = (double)pz / e2z; goto tail_x;                                            /* Y_ALONG_X */
        case 5: ud = ((double)e1x * pz - (double)e1z * px) / ((double)e2z * e1x - (double)e1z * e2x); goto tail_x;   /* Y */
        case 7: ud = ((double)e1x * py - (double)e1y * px) / ((double)e2y * e1x - (double)e1y * e2x); goto tail_x;   /* Z */
        default: continue;
        }
    tail_z:  u = (float)ud; if (ud < zero || (double)u > one) continue; v = ((double)pz - (double)e2z * u) / e1z; goto tail_v;
    tail_y:  u = (float)ud; if (ud < zero || (double)u > one) continue; v = ((double)py - (double)e2y * u) / e1y; goto tail_v;
    tail_x:  u = (float)ud; if (ud < zero || (double)u > one) continue; v = ((double)px - (double)e2x * u) / e1x; goto tail_v;
    tail_v:
        if (v < zero) continue;
        if ((double)u + v > one) continue;
        MEM32(p + 0x40) = fb(t);
        MEM32(p + 0x48) = MEM32(f + 0x10); MEM32(p + 0x4C) = MEM32(f + 0x14); MEM32(p + 0x50) = MEM32(f + 0x18);
        MEM32(p + 0x54) = f;
        hit = 1;
    }
    *eax_out = 0;
    return hit;
}

void fam_native_line_in_tree_level(void) {
    uint32_t self = g_ecx, cell = MEM32(g_esp + 4), p = MEM32(g_esp + 8), eh;
    uint8_t r = line_in_tree_level(self, cell, p, &eh);
    g_eax = eh | r;
    g_esp += 4 + 8;
}

/* ---- the hook: native, or the first calls checked against the original ---- */
static int g_check = -1, g_left = 3000, g_left0;
static uint64_t g_hits;
void fam_native_line_in_tree_level_hook(void) {
    if (!fam_native_cline_on) { recomp_game_005C66A0_orig(); return; }
    if (g_check < 0) {
        const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_check = e && atoi(e);
        e = getenv("FAMILIA_CLINE_CHECK"); if (e && atoi(e) > 0) { g_check = 1; g_left = atoi(e); }
        g_left0 = g_left;
    }
    if (!g_check || g_left <= 0) { fam_native_line_in_tree_level(); return; }
    uint32_t p = MEM32(g_esp + 8), esp0 = g_esp, ecx0 = g_ecx;
    uint32_t a1 = MEM32(esp0 + 4), a2 = MEM32(esp0 + 8);     /* the original keeps temporaries in its argument slots */
    uint8_t pre[0x58], orig[0x58];
    memcpy(pre, (const void *)ADDR(p), sizeof pre);
    recomp_game_005C66A0_orig();
    uint32_t eax1 = g_eax, esp1 = g_esp;
    memcpy(orig, (const void *)ADDR(p), sizeof orig);
    memcpy((void *)ADDR(p), pre, sizeof pre);
    g_esp = esp0; g_ecx = ecx0;
    MEM32(esp0 + 4) = a1; MEM32(esp0 + 8) = a2;
    fam_native_line_in_tree_level();
    int bad = memcmp(orig, (const void *)ADDR(p), sizeof orig) != 0 || g_eax != eax1 || g_esp != esp1;
    if (bad) {
        int k = 0; while (k < (int)sizeof orig && orig[k] == ((const uint8_t *)ADDR(p))[k]) k++;
        fprintf(stderr, "[native] LineInTreeLevel MISMATCH (query byte %d, eax %08X/%08X): the translated one from now on\n", k, eax1, g_eax);
        memcpy((void *)ADDR(p), orig, sizeof orig); g_eax = eax1;     /* the original's result stands */
        fam_native_cline_on = 0;
        return;
    }
    g_hits += (uint8_t)g_eax;
    if (--g_left == 0)
        fprintf(stderr, "[native] LineInTreeLevel: matches the translated original (%d calls, %llu with a hit)\n", g_left0, (unsigned long long)g_hits);
}

/* ===========================================================================
 * g_collision::LineInAABox (0x005C5B20, (min, max, p), ret 0xC): the line
 * against an axis-aligned box from outside, an axis at a time (y, x, z).
 * Every caller only tests al, so a miss keeps eax's upper bits as they were
 * (the original leaves x87 status-word bits there).
 * ======================================================================== */
void recomp_game_005C5B20_orig(void);
static int aabox(uint32_t mn, uint32_t mx, uint32_t p, uint32_t *zbits) {
    const double zero = lf(K_ZERO), one = lf(K_ONE);
    double eps; { uint64_t q; memcpy(&q, (const void *)ADDR(0x006232E8u), 8); memcpy(&eps, &q, 8); }
    /* y */
    double dy = lf(p + 0x10);
    if (fabs(dy) > eps) {
        float t = ff(((dy > zero ? (double)lf(mn + 4) : (double)lf(mx + 4)) - lf(p + 4)) / dy);
        if ((double)t > one) return 0;
        if ((double)t < lf(p + 0x40) && (double)t > zero) {
            float vx = ff((double)t * lf(p + 0xC) + lf(p)), vz = ff((double)t * lf(p + 0x14) + lf(p + 8));
            if ((double)vx >= lf(mn) && ((double)vx < lf(mx) || (double)vx == lf(mx))
                && (double)vz >= lf(mn + 8) && ((double)vz < lf(mx + 8) || (double)vz == lf(mx + 8))) {
                MEM32(p + 0x48) = 0; MEM32(p + 0x4C) = dy > zero ? 0x3F800000u : 0xBF800000u; MEM32(p + 0x50) = 0;
                MEM32(p + 0x40) = fb(t); *zbits = 0;
                return 1;
            }
        }
    }
    /* x */
    double dx = lf(p + 0xC);
    if (fabs(dx) > eps) {
        float t = ff(((dx > zero ? (double)lf(mn) : (double)lf(mx)) - lf(p)) / dx);
        if ((double)t > one) return 0;
        if ((double)t < lf(p + 0x40) && (double)t > zero) {
            float vy = ff((double)t * lf(p + 0x10) + lf(p + 4)), vz = ff((double)t * lf(p + 0x14) + lf(p + 8));
            if ((double)vy >= lf(mn + 4) && ((double)vy < lf(mx + 4) || (double)vy == lf(mx + 4))
                && (double)vz >= lf(mn + 8) && ((double)vz < lf(mx + 8) || (double)vz == lf(mx + 8))) {
                MEM32(p + 0x48) = dx > zero ? 0x3F800000u : 0xBF800000u; MEM32(p + 0x4C) = 0; MEM32(p + 0x50) = 0;
                MEM32(p + 0x40) = fb(t); *zbits = 0;
                return 1;
            }
        }
    }
    /* z: a plane at or beyond the current hit ends the test */
    double dz = lf(p + 0x14);
    if (!(fabs(dz) > eps)) return 0;
    float t = ff(((dz > zero ? (double)lf(mn + 8) : (double)lf(mx + 8)) - lf(p + 8)) / dz);
    if ((double)t > one) return 0;
    if (!((double)t < lf(p + 0x40) || t != t || lf(p + 0x40) != lf(p + 0x40))) return 0;   /* unordered goes on */
    if (!((double)t > zero)) return 0;
    float vx = ff((double)t * lf(p + 0xC) + lf(p));
    double vy = (double)t * lf(p + 0x10) + lf(p + 4);                   /* compared unrounded */
    if (!(vy >= lf(mn + 4))) return 0;
    if (!(vy < lf(mx + 4) || vy == lf(mx + 4))) return 0;
    if (!((double)vx >= lf(mn))) return 0;
    if (!((double)vx < lf(mx) || (double)vx == lf(mx))) return 0;
    uint32_t nz = dz > zero ? 0x3F800000u : 0xBF800000u;
    MEM32(p + 0x48) = 0; MEM32(p + 0x4C) = 0; MEM32(p + 0x50) = nz;
    MEM32(p + 0x40) = fb(t); *zbits = nz;
    return 1;
}
void fam_native_line_in_aabox(void) {
    uint32_t mn = MEM32(g_esp + 4), mx = MEM32(g_esp + 8), p = MEM32(g_esp + 12), zb = 0;
    if (aabox(mn, mx, p, &zb)) g_eax = (zb & 0xFFFFFF00u) | 1u;
    else g_eax &= 0xFFFFFF00u;
    g_esp += 4 + 12;
}
static int g_ab_check = -1, g_ab_left = 5000, g_ab_left0;
static uint64_t g_ab_hits;
void fam_native_line_in_aabox_hook(void) {
    if (!fam_native_cline_on) { recomp_game_005C5B20_orig(); return; }
    if (g_ab_check < 0) {
        const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_ab_check = e && atoi(e);
        e = getenv("FAMILIA_CLINE_CHECK"); if (e && atoi(e) > 0) { g_ab_check = 1; g_ab_left = atoi(e); }
        g_ab_left0 = g_ab_left;
    }
    if (!g_ab_check || g_ab_left <= 0) { fam_native_line_in_aabox(); return; }
    uint32_t p = MEM32(g_esp + 12), esp0 = g_esp, eax0 = g_eax;
    uint32_t a1 = MEM32(esp0 + 4), a2 = MEM32(esp0 + 8), a3 = MEM32(esp0 + 12);   /* t and the axis go there */
    uint8_t pre[0x58], orig[0x58];
    memcpy(pre, (const void *)ADDR(p), sizeof pre);
    recomp_game_005C5B20_orig();
    uint32_t al1 = g_eax & 0xFF, esp1 = g_esp;
    memcpy(orig, (const void *)ADDR(p), sizeof orig);
    memcpy((void *)ADDR(p), pre, sizeof pre);
    g_esp = esp0; g_eax = eax0;
    MEM32(esp0 + 4) = a1; MEM32(esp0 + 8) = a2; MEM32(esp0 + 12) = a3;
    fam_native_line_in_aabox();
    if (memcmp(orig, (const void *)ADDR(p), sizeof orig) || (g_eax & 0xFF) != al1 || g_esp != esp1) {
        int k = 0; while (k < (int)sizeof orig && orig[k] == ((const uint8_t *)ADDR(p))[k]) k++;
        fprintf(stderr, "[native] LineInAABox MISMATCH (query byte %d, al %u/%u): the translated ones from now on\n", k, al1, g_eax & 0xFF);
        memcpy((void *)ADDR(p), orig, sizeof orig); g_eax = (g_eax & 0xFFFFFF00u) | al1;
        fam_native_cline_on = 0;
        return;
    }
    g_ab_hits += al1;
    if (--g_ab_left == 0)
        fprintf(stderr, "[native] LineInAABox: matches the translated original (%d calls, %llu with a hit)\n", g_ab_left0, (unsigned long long)g_ab_hits);
}
