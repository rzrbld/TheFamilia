/*
 * native_cull.c - native replacement for the engine's first culling pass over
 * a frame's children (LS3DF 0x10041DF0, mafia-re's CTP_EnumProcFirst).
 *
 * Written from the x86 of LS3DF.dll 1.2 (build/logs/ctp_first.asm), with the
 * mafia-re decompilation used only as a map of names and offsets. The
 * translated original keeps x87 registers as C doubles and rounds to float
 * only where the code stores a dword; this file does the same (and is built
 * with -ffp-contract=off), so every decision and every stored value is bit
 * for bit the original's. FAMILIA_NATIVE_CHECK=1 compares it against the
 * translated original on random frame trees (fam_native_cull_check).
 *
 * Per child of the root: clear the clip / in-view flags; skip frames that are
 * off or never culled; test the world bounding sphere against the near and
 * far planes (setting the clip flag when it crosses one) and, without a
 * portal, against the four side planes; a sphere that crosses side planes has
 * its world box tested against them, corner by corner; a frame that survives
 * gets the clip flag for crossed planes, the render number, a place in the
 * current sector's list of visuals (visuals only), and its own children
 * culled the same way. A sector root then runs its occluder pass: that part
 * is the translated original's own code, entered with the root's child list
 * hidden so it skips straight to it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

/* I3D_frame */
#define FRM_FLAGS   0xAC
#define FRM_BOXMIN  0xD8                /* world box min, max (+0xE4), sphere pos (+0xF0), radius (+0xFC) */
#define FRM_TYPE    0x110
#define FRM_CHILD   0x124
#define FRM_NEXT    0x12C
#define FRM_NEXTVIS 0x130
#define FRM_RENDERN 0x13C
#define VIS_SORT    0x1DC
#define SCT_LASTVIS 0x230

#define FF_ON        0x1u
#define FF_HRBW_OK   0x100u
#define FF_IN_VIEW   0x10000u
#define FF_CLIP      0x200000u
#define FF_NO_CULL   0x8000000u

/* the culling pass's globals */
#define G_NEAR      0x101B43ECu
#define G_DS        0x101B43F0u          /* 5 plane distances */
#define G_PORTAL    0x101B4408u
#define G_FAR       0x101B440Cu
#define G_PMODE     0x101B45A0u
#define G_NORMALS   0x101B45A4u          /* 5 plane normals (x, y, z) */
#define G_SECTOR    0x101B45E0u
#define G_CAMERA    0x101C4CF8u
#define G_RENDERNUM 0x100AFC24u
#define K_SORT_OFS  0x1009B7C0u          /* constants in .rdata */
#define K_ZERO      0x1009B310u
#define K_SORT_MUL  0x1009B688u

void recomp_ls3df_1001CF90(void);       /* I3D_frame::UpdateHRBoundVolW, stdcall (this) */
void recomp_ls3df_10041DF0_orig(void);  /* the translated original */

static inline double F(uint32_t va) { uint32_t u = MEM32(va); float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void update_bound_w(uint32_t frm) {
    PUSH32(g_esp, frm);
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_ls3df_1001CF90();
}

/* A stored float the code tests by its bits: below zero (and not -0) means behind. */
static inline int behind(double s) { return bits_of((float)s) > 0x80000000u; }

/* Side plane k (1..4) against the box: 1 when all eight corners are behind it.
 * The corners are walked as the original does, the sum updated by differences. */
static int box_behind(int k, const float mn[3], const float mx[3]) {
    uint32_t n = G_NORMALS + 12u * (uint32_t)k;
    double a = F(n) * (double)mn[0], b = F(n + 4) * (double)mn[1], c = F(n + 8) * (double)mn[2];
    double s = ((F(G_DS + 4u * (uint32_t)k) + a) + b) + c;
    if (!behind(s)) return 0;
    double dc = F(n + 8) * (double)mx[2] - c;
    s = s + dc;  if (!behind(s)) return 0;
    double db = F(n + 4) * (double)mx[1] - b;
    s = s + db;  if (!behind(s)) return 0;
    s = s - dc;  if (!behind(s)) return 0;
    double da = F(n) * (double)mx[0] - a;
    s = s + da;  if (!behind(s)) return 0;
    s = s + dc;  if (!behind(s)) return 0;
    s = s - db;  if (!behind(s)) return 0;
    s = s - dc;  if (!behind(s)) return 0;
    return 1;
}

/* the distance of point (x, y, z) to plane k: ((nz*z + ny*y) + nx*x) + d */
static inline double plane_dist(int k, double x, double y, double z) {
    uint32_t n = G_NORMALS + 12u * (uint32_t)k;
    return ((F(n + 8) * z + F(n + 4) * y) + F(n) * x) + F(G_DS + 4u * (uint32_t)k);
}

static void enum_first(uint32_t root);

static void cull_children(uint32_t root) {
    /* a second pointer runs 8 siblings ahead along the list, prefetching each
     * frame's lines (flags, sphere, links, sort key): the misses overlap with
     * the culling instead of coming one after another */
    uint32_t ahead = MEM32(root + FRM_CHILD);
    for (int k = 0; k < 8 && ahead; k++) {
        const char *p = (const char *)ADDR(ahead);
        __builtin_prefetch(p + 0xAC); __builtin_prefetch(p + 0xF0); __builtin_prefetch(p + 0x110); __builtin_prefetch(p + 0x1DC);
        ahead = MEM32(ahead + FRM_NEXT);
    }
    for (uint32_t frm = MEM32(root + FRM_CHILD); frm; frm = MEM32(frm + FRM_NEXT)) {
        if (ahead) { const char *p = (const char *)ADDR(ahead);
                     __builtin_prefetch(p + 0xAC); __builtin_prefetch(p + 0xF0); __builtin_prefetch(p + 0x110); __builtin_prefetch(p + 0x1DC);
                     ahead = MEM32(ahead + FRM_NEXT); }
        uint32_t fl = MEM32(frm + FRM_FLAGS) & 0xFFDEFFFFu;
        MEM32(frm + FRM_FLAGS) = fl;
        if ((fl & FF_NO_CULL) || !(fl & FF_ON)) continue;
        if (!(fl & FF_HRBW_OK)) update_bound_w(frm);
        double x = F(frm + 0xF0), y = F(frm + 0xF4), z = F(frm + 0xF8), r = F(frm + 0xFC);
        float d = (float)plane_dist(0, x, y, z);                      /* depth, stored */
        if (MEM32(frm + FRM_TYPE) == 1) {                              /* a visual's sort key */
            double f = (double)d + F(K_SORT_OFS);
            if (f < F(K_ZERO)) MEM32(frm + VIS_SORT) = 0;
            else { float s = (float)(f * F(K_SORT_MUL)); MEM32(frm + VIS_SORT) = bits_of(s); }
        }
        if ((double)d + r < F(G_NEAR)) continue;
        if ((double)d - F(G_NEAR) <= r) MEM32(frm + FRM_FLAGS) |= FF_CLIP;
        if ((double)d - r > F(G_FAR)) continue;
        if (F(G_FAR) - (double)d <= r) MEM32(frm + FRM_FLAGS) |= FF_CLIP;
        uint32_t add;
        if (MEM32(G_PORTAL)) {
            MEM32(frm + FRM_FLAGS) |= FF_IN_VIEW;
            goto children;
        }
        {
            double neg_r = (double)(float)-r;
            double d1 = plane_dist(1, x, y, z); float f1 = (float)d1; if (!(d1 > neg_r)) continue;
            double d2 = plane_dist(2, x, y, z); float f2 = (float)d2; if (!(d2 > neg_r)) continue;
            double d3 = plane_dist(3, x, y, z); float f3 = (float)d3; if (!(d3 > neg_r)) continue;
            double d4 = plane_dist(4, x, y, z); float f4 = (float)d4; if (!(d4 > neg_r)) continue;
            uint32_t crossed = (r > f1 ? 1u : 0) | (r > f2 ? 2u : 0) | (r > f3 ? 4u : 0) | (r > f4 ? 8u : 0);
            if (crossed) {
                float mn[3], mx[3];
                if (!(MEM32(frm + FRM_FLAGS) & FF_HRBW_OK)) update_bound_w(frm);
                memcpy(mn, (const void *)ADDR(frm + FRM_BOXMIN), 12);
                if (!(MEM32(frm + FRM_FLAGS) & FF_HRBW_OK)) update_bound_w(frm);
                memcpy(mx, (const void *)ADDR(frm + FRM_BOXMIN + 12), 12);
                for (int k = 1; k <= 4; k++)
                    if ((crossed & (1u << (k - 1))) && !box_behind(k, mn, mx)) crossed ^= 1u << (k - 1);
                if (crossed) continue;                                 /* a side plane hides the box */
            }
            if (!MEM32(G_PORTAL)) {
                add = (r > f1 || r > f2 || r > f3 || r > f4) ? FF_CLIP : 0;
            } else {
                /* (reachable only if a bound update set a portal) the camera's portal planes */
                uint32_t mode = MEM32(G_PMODE), cam = MEM32(G_CAMERA), pn, pd;
                if (mode == 0x1000) { pn = cam + 0x454; pd = cam + 0x490; }
                else if (mode == 0x4000) { pn = cam + 0x4A4; pd = cam + 0x4E0; }
                else { pn = cam + 0x404; pd = cam + 0x440; }
                add = 0;
                static const uint32_t no[4] = { 0x3C, 0x78, 0xB4, 0xF0 }, dof[4] = { 0x14, 0x28, 0x3C, 0x50 };
                for (int i = 0; i < 4; i++) {
                    double t = ((z * F(pn + no[i] + 8) + y * F(pn + no[i] + 4)) + x * F(pn + no[i])) + F(pd + dof[i]);
                    if (t < r) add = FF_CLIP;
                }
            }
        }
        MEM32(frm + FRM_FLAGS) |= add;
        MEM32(frm + FRM_RENDERN) = MEM32(G_RENDERNUM);
        if (MEM32(frm + FRM_TYPE) == 1) {                              /* onto the sector's list of visuals */
            MEM32(MEM32(MEM32(G_SECTOR) + SCT_LASTVIS) + FRM_NEXTVIS) = frm;
            MEM32(frm + FRM_NEXTVIS) = 0;
            MEM32(MEM32(G_SECTOR) + SCT_LASTVIS) = frm;
        }
    children:
        if (MEM32(frm + FRM_CHILD)) enum_first(frm);
    }
}

static void enum_first(uint32_t root) {
    cull_children(root);
    if (MEM32(root + FRM_TYPE) == 5) {
        uint32_t c = MEM32(root + FRM_CHILD);
        MEM32(root + FRM_CHILD) = 0;                 /* the original then finds no children */
        g_ecx = root;
        PUSH32(g_esp, RECOMP_RETADDR);
        recomp_ls3df_10041DF0_orig();
        MEM32(root + FRM_CHILD) = c;
    }
}

/* 0 after a failed self-check: the translated original runs instead */
int fam_native_cull_on = 1;

/* void CTP_EnumProcFirst(I3D_frame *root)  -- LS3DF 0x10041DF0, root in ecx, plain ret */
void fam_native_ctp_enum_first(void) {
    if (!fam_native_cull_on) { recomp_ls3df_10041DF0_orig(); return; }
    enum_first(g_ecx);
    g_esp += 4;
}

/* ---------------------------------------------------- differential check */
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 33) % n; }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)rnd(1u << 20) / (float)(1u << 20); }
static void wf(uint32_t va, float f) { MEM32(va) = bits_of(f); }
/* mostly ordinary values, sometimes the edge cases the comparisons care about */
static float pick(float lo, float hi) {
    switch (rnd(40)) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: { uint32_t u = 0x7FC00000u; float f; memcpy(&f, &u, 4); return f; }
    default: return rndf(lo, hi);
    }
}

int fam_native_cull_check(void) {
    /* frames, then a sector at +NF*FS and its last visual at +0x400 (whose +0x130 link
     * must stay inside the arena, or copy A's link lands in copy B) */
    enum { NF = 40, FS = 0x200, ARENA = NF * FS + 0x800 };
    uint32_t mem = hle_va(hle_heap_alloc(2 * ARENA + 64, 1));
    uint32_t A = mem, B = mem + ARENA;
    /* the globals the pass reads and writes, saved around the test */
    uint8_t save1[0x24], save2[0x44]; uint32_t save_rn = MEM32(G_RENDERNUM);
    memcpy(save1, (void *)ADDR(G_NEAR), sizeof save1);
    memcpy(save2, (void *)ADDR(G_PMODE), sizeof save2);
    int bad = 0, n;
    for (n = 0; n < 20000 && bad < 5; n++) {
        memset((void *)ADDR(A), 0, ARENA);
        uint32_t sect = A + NF * FS, dummy = sect + 0x400;            /* sector, and its last visual */
        MEM32(sect + SCT_LASTVIS) = dummy;
        float spread = rnd(2) ? 60.0f : 6.0f;                          /* small spreads make ties likely */
        for (int i = 0; i < NF; i++) {
            uint32_t f = A + i * FS;
            static const uint32_t types[] = { 0, 1, 1, 1, 2, 6, 9, 11 };
            MEM32(f + FRM_TYPE) = types[rnd(8)];
            uint32_t fl = (rnd(10) ? FF_ON : 0) | (rnd(30) ? 0 : FF_NO_CULL) | (rnd(6) ? FF_HRBW_OK : 0)
                        | (rnd(2) ? FF_CLIP : 0) | (rnd(2) ? FF_IN_VIEW : 0) | (uint32_t)rnd(0x100) << 1;
            fl |= 0x60u;                                               /* local bound and world matrix valid */
            MEM32(f + FRM_FLAGS) = fl;
            float cx = pick(-spread, spread), cy = pick(-spread, spread), cz = pick(-spread, spread), rr = pick(0, spread / 2);
            wf(f + 0xF0, cx); wf(f + 0xF4, cy); wf(f + 0xF8, cz); wf(f + 0xFC, rr);
            for (int k = 0; k < 3; k++) {                              /* world box roughly around the sphere */
                float c = k == 0 ? cx : k == 1 ? cy : cz, e = rndf(0, spread / 2);
                wf(f + FRM_BOXMIN + 4 * k, rnd(8) ? c - e : pick(-spread, spread));
                wf(f + FRM_BOXMIN + 12 + 4 * k, rnd(8) ? c + e : pick(-spread, spread));
            }
            /* local box (+0xB0) and world matrix (+0x10) for UpdateHRBoundVolW */
            for (int k = 0; k < 3; k++) { float e = rndf(0, spread / 4); wf(f + 0xB0 + 4 * k, -e); wf(f + 0xBC + 4 * k, e); }
            for (int k = 0; k < 16; k++) wf(f + 0x10 + 4 * k, k % 5 == 0 ? 1.0f : 0.0f);
            for (int k = 0; k < 3; k++) wf(f + 0x40 + 4 * k, rndf(-spread, spread));
            if (i) {
                uint32_t p = A + rnd(i) * FS;
                MEM32(f + FRM_NEXT) = MEM32(p + FRM_CHILD); MEM32(p + FRM_CHILD) = f;
                MEM32(f + 0x120) = p;
            }
        }
        /* the planes: near/far depth plane 0 and four side planes */
        for (int k = 0; k < 5; k++) {
            wf(G_NORMALS + 12 * k, pick(-1, 1)); wf(G_NORMALS + 12 * k + 4, pick(-1, 1)); wf(G_NORMALS + 12 * k + 8, pick(-1, 1));
            wf(G_DS + 4 * k, pick(-spread, spread));
        }
        wf(G_NEAR, rnd(5) ? rndf(-spread, spread / 4) : 0.0f);
        wf(G_FAR, rndf(0, 2 * spread));
        MEM32(G_PORTAL) = rnd(10) ? 0 : 1;
        MEM32(G_RENDERNUM) = rnd(100000);
        /* B = A, pointers relocated */
        memcpy((void *)ADDR(B), (void *)ADDR(A), ARENA);
        for (uint32_t o = 0; o < ARENA; o += 4) {
            uint32_t v = MEM32(B + o);
            if (v >= A && v < A + ARENA && (o % FS == 0x120 || o % FS == FRM_CHILD || o % FS == FRM_NEXT || o >= NF * FS))
                MEM32(B + o) = v - A + B;
        }
        uint32_t root = (uint32_t)rnd(4) * FS, esp0 = g_esp;
        MEM32(G_SECTOR) = A + NF * FS;
        PUSH32(g_esp, RECOMP_RETADDR); g_ecx = A + root; recomp_ls3df_10041DF0_orig();
        int ok1 = g_esp == esp0;
        MEM32(G_SECTOR) = B + NF * FS;
        PUSH32(g_esp, RECOMP_RETADDR); g_ecx = B + root; fam_native_ctp_enum_first();
        int ok2 = g_esp == esp0;
        int ndiff = 0, diffs[3];
        for (uint32_t o = 0; o < ARENA; o += 4) {
            uint32_t a = MEM32(A + o), b = MEM32(B + o);
            if (a == b) continue;
            if (a >= A && a < A + ARENA && b == a - A + B) continue;
            if (ndiff < 3) diffs[ndiff] = (int)o;
            ndiff++;
        }
        if (ndiff || !ok1 || !ok2) {
            if (!bad) {
                fprintf(stderr, "[native] CTP_EnumProcFirst MISMATCH (case %d): %d words differ, esp %d/%d\n", n, ndiff, ok1, ok2);
                for (int i = 0; i < ndiff && i < 3; i++)
                    fprintf(stderr, "[native]   %s %d +%03X: original %08X, native %08X\n",
                            diffs[i] < NF * FS ? "frame" : "sector block", diffs[i] / FS, diffs[i] % FS,
                            MEM32(A + diffs[i]), MEM32(B + diffs[i]));
            }
            bad++;
        }
    }
    memcpy((void *)ADDR(G_NEAR), save1, sizeof save1);
    memcpy((void *)ADDR(G_PMODE), save2, sizeof save2);
    MEM32(G_RENDERNUM) = save_rn;
    fprintf(stderr, "[native] CTP_EnumProcFirst: %s against the translated original (%d random trees)%s\n",
            bad ? "DIFFERS" : "matches", n, bad ? ": native culling disabled" : "");
    hle_heap_free(hle_ptr(mem));
    if (bad) fam_native_cull_on = 0;                 /* only this replacement; the others stay on */
    return 0;
}
