/*
 * native_frame.c - native replacements for LS3D scene-graph methods.
 *
 * Field offsets and semantics: our own reading of LS3DF.dll 1.2, with the
 * mafia-re decompilation (github.com/Marvisak/mafia-re) as a map of names and
 * offsets. Each function is checked against its translated original by the
 * differential test in fam_native_frame_check (FAMILIA_NATIVE_CHECK=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "win32hle.h"
#include "recomp_types.h"

/* I3D_frame */
#define FRM_TYPE   0x110
#define FRM_NAME   0x100
#define FRM_CHILD  0x124
#define FRM_NEXT   0x12C

/* the type bits a lookup's flags accept, by frame type (0 = never) */
static const uint32_t k_type_bit[32] = {
    [1] = 0x1,     /* visual */   [2] = 0x2,    /* light */   [3] = 0x4,   /* camera */
    [4] = 0x8,     /* sound */    [5] = 0x10,   /* sector */  [6] = 0x20,  /* dummy */
    [7] = 0x40,    /* target */   [8] = 0x100,  /* user */    [9] = 0x80,  /* model */
    [11] = 0x200,  /* volume */   [14] = 0x1000, /* area */   [15] = 0x2000, /* shadow */
};

static inline int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int name_matches(const char *want, const char *have, uint32_t flags) {
    int wild = (flags & 0x10000) != 0, cs = (flags & 0x20000) != 0;
    if (!wild) {
        if (cs) return strcmp(want, have) == 0;
        for (;; want++, have++) {                     /* ASCII case folding (C locale) */
            int a = up((unsigned char)*want), b = up((unsigned char)*have);
            if (a != b) return 0;
            if (!a) return 1;
        }
    }
    size_t lw = strlen(want), lh = strlen(have), n = (lw < lh ? lw : lh) + 1;
    for (; n--; want++, have++) {                     /* '*' ends the pattern, '?' is any character */
        if (*want == '*') return 1;
        if (*want == '?') continue;
        if (cs ? *want != *have : up(*want) != up(*have)) return 0;
    }
    return 1;
}

static uint32_t get_child_frame(uint32_t frm, uint32_t name, uint32_t flags) {
    uint32_t types = flags & 0xFFFF;
    int ok = 1;
    if (types != 0xFFFF) {
        uint32_t t = MEM32(frm + FRM_TYPE);
        ok = (t < 32 ? k_type_bit[t] : 0) & types ? 1 : 0;
    }
    if (ok && name) {
        uint32_t own = MEM32(frm + FRM_NAME);
        ok = own && name_matches((const char *)ADDR(name), (const char *)ADDR(own), flags);
    }
    if (ok) return frm;
    for (uint32_t c = MEM32(frm + FRM_CHILD); c; c = MEM32(c + FRM_NEXT)) {
        uint32_t r = get_child_frame(c, name, flags);
        if (r) return r;
    }
    return 0;
}

/* The same pre-order search, pipelined for an in-order core. Mission scripts
 * look names up in the whole city (tens of thousands of frames); recursing,
 * each frame costs two dependent cache misses (the frame, then its name).
 * Here a cursor walks the links ahead, prefetching both the first child and
 * the next sibling of each frame and the name of each candidate, and the name
 * comparisons run a few candidates behind it, in the same order. The result is
 * the first match in pre-order, exactly as the recursion's. */
#define GCF_LAG 6
static uint64_t g_gcf_nodes;
static uint32_t get_child_frame_fast(uint32_t frm, uint32_t name, uint32_t flags) {
    uint32_t types = flags & 0xFFFF;
    const char *want = name ? (const char *)ADDR(name) : NULL;
    uint32_t q[GCF_LAG]; unsigned qh = 0, qt = 0;     /* candidates whose names are being fetched */
    uint32_t stack[512]; int sp = 0;
    uint32_t c = frm;
    uint64_t n = 0;
    while (c) {
        n++;
        const uint8_t *p = (const uint8_t *)ADDR(c);
        uint32_t ch = *(const uint32_t *)(p + FRM_CHILD);
        uint32_t nx = c == frm ? 0 : *(const uint32_t *)(p + FRM_NEXT);
        if (ch) __builtin_prefetch((const void *)(ADDR(ch) + 0x100));
        if (nx) __builtin_prefetch((const void *)(ADDR(nx) + 0x100));
        int ok = 1;
        if (types != 0xFFFF) { uint32_t t = *(const uint32_t *)(p + FRM_TYPE); ok = (t < 32 ? k_type_bit[t] : 0) & types ? 1 : 0; }
        if (ok) {
            if (!want) { g_gcf_nodes += n; return c; }       /* no name: nothing ahead of it is pending */
            uint32_t own = *(const uint32_t *)(p + FRM_NAME);
            if (own) {
                __builtin_prefetch((const void *)ADDR(own));
                if (qt - qh == GCF_LAG) {
                    uint32_t f = q[qh++ % GCF_LAG];
                    if (name_matches(want, (const char *)ADDR(MEM32(f + FRM_NAME)), flags)) { g_gcf_nodes += n; return f; }
                }
                q[qt++ % GCF_LAG] = c;
            }
        }
        /* pre-order: first child, else next sibling, else a pending sibling */
        if (ch) {
            if (nx) { if (sp == 512) goto deep; stack[sp++] = nx; }
            c = ch;
        } else if (nx) c = nx;
        else c = sp ? stack[--sp] : 0;
    }
    while (qh != qt) {
        uint32_t f = q[qh++ % GCF_LAG];
        if (name_matches(want, (const char *)ADDR(MEM32(f + FRM_NAME)), flags)) { g_gcf_nodes += n; return f; }
    }
    g_gcf_nodes += n;
    return 0;
deep:   /* deeper than the stack: the plain recursion (same answer) */
    g_gcf_nodes += n;
    return get_child_frame(frm, name, flags);
}

/* I3D_frame *I3D_frame::GetChildFrame(const char *name, uint flags)  -- LS3DF 0x1001C360, thiscall, ret 8 */
static int g_gcf_mode = -1;
void fam_native_get_child_frame(void) {
    uint32_t name = MEM32(g_esp + 4), flags = MEM32(g_esp + 8);
    if (__builtin_expect(g_gcf_mode < 0, 0)) { const char *e = getenv("FAMILIA_GCF"); g_gcf_mode = e ? atoi(e) : 1; }
    static int st = -1; if (st < 0) { const char *e = getenv("FAMILIA_GCF_STATS"); st = e && atoi(e); }
    uint64_t t0 = 0, n0 = g_gcf_nodes;
    if (st) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
    g_eax = g_gcf_mode ? get_child_frame_fast(g_ecx, name, flags) : get_child_frame(g_ecx, name, flags);
    if (st) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t dt = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec - t0;
        static uint64_t calls, ns, big;
        calls++; ns += dt;
        if (dt > 2000000) { big++; fprintf(stderr, "[gcf] %.2f ms '%s' flags %X nodes %llu -> %08X\n", dt / 1e6,
                                           name ? (const char *)ADDR(name) : "", flags, (unsigned long long)(g_gcf_nodes - n0), g_eax); }
        if (calls % 2000 == 0) fprintf(stderr, "[gcf] %llu calls %.1f ms total, %llu over 2 ms\n", (unsigned long long)calls, ns / 1e6, (unsigned long long)big);
    }
    g_esp += 4 + 8;
}

/* ------------------------------------------------- frame update (flags) */
#define FRM_FLAGS  0xAC
#define FRM_UPDCNT 0x118
#define FRM_PARENT 0x120
#define VIS_FLAGS  0x1D8
#define FRMF_CHANGED 0x40002000u            /* FRMFLAGS_UPDATE | FRMFLAGS_CHANGED */
#define VA_VISUAL_UPDATEV 0x1005A390u       /* I3D_visual::UpdateV */

/* this->UpdateV(): thiscall, no arguments. A bare `ret` (the shared empty
 * implementation) is skipped, I3D_visual's is done here, others run translated. */
/* what an UpdateV target is: a bare `ret`, I3D_visual's, or a lifted function.
 * A few dozen distinct targets exist; a tiny direct-mapped cache saves reading
 * guest code bytes and the dispatch cache for each of ~8000 frames a frame. */
enum { UV_EMPTY = 1, UV_VISUAL = 2, UV_CALL = 3, UV_JOINT = 4, UV_DUMMY = 5 };
#define VA_JOINT_UPDATEV 0x1001EE30u        /* I3D_joint::UpdateV */
#define VA_DUMMY_UPDATEV 0x1001A330u        /* I3D_dummy::UpdateV */
/* I3D_frame::DiscardHRBoundVol (0x1001CC00): this one's and its parents' hierarchy bounds */
static void discard_hr_bound_vol(uint32_t frm) {
    if (MEM32(frm + FRM_TYPE) == 5) return;
    MEM32(frm + FRM_FLAGS) &= 0xFFFFFEBFu;
    for (uint32_t p = MEM32(frm + FRM_PARENT); p; p = MEM32(p + FRM_PARENT)) {
        uint32_t f = MEM32(p + FRM_FLAGS);
        if (!(f & 0x40) || MEM32(p + FRM_TYPE) == 5) break;
        MEM32(p + FRM_FLAGS) = f & 0xFFFFFEBFu;
    }
}
static struct { uint32_t fn; uint32_t kind; recomp_func_t f; } g_uv[64];
static void call_update_v(uint32_t frm) {
    uint32_t fn = MEM32(MEM32(frm) + 0x44);
    unsigned h = (fn >> 4) & 63;
    if (__builtin_expect(g_uv[h].fn != fn, 0)) {
        g_uv[h].fn = fn; g_uv[h].f = NULL;
        if (MEM8(fn) == 0xC3) g_uv[h].kind = UV_EMPTY;
        else if (fn == VA_VISUAL_UPDATEV) g_uv[h].kind = UV_VISUAL;
        else if (fn == VA_JOINT_UPDATEV) g_uv[h].kind = UV_JOINT;
        else if (fn == VA_DUMMY_UPDATEV) g_uv[h].kind = UV_DUMMY;
        else {
            recomp_func_t f = recomp_lookup_manual(fn);
            if (!f) f = recomp_lookup(fn);
            if (!f) { fprintf(stderr, "[native] UpdateV target %08X not found\n", fn); abort(); }
            g_uv[h].kind = UV_CALL; g_uv[h].f = f;
        }
    }
    switch (g_uv[h].kind) {
    case UV_EMPTY: return;
    case UV_VISUAL: {
        uint32_t vf = MEM32(frm + VIS_FLAGS);
        if (!(vf & 0x40) && (MEM32(frm + FRM_FLAGS) & FRMF_CHANGED)) MEM32(frm + VIS_FLAGS) = vf & 0xFFE3FFFFu;
        return;
    }
    case UV_DUMMY:
        if (MEM32(frm + FRM_FLAGS) & FRMF_CHANGED) MEM32(frm + 0x198) &= 0xFFFFFFEFu;
        return;
    case UV_JOINT: {
        uint32_t f = MEM32(frm + FRM_FLAGS);
        if (!(f & FRMF_CHANGED)) return;
        MEM32(frm + 0x204) &= 0xFFFFFFE3u;
        if (!(f & 0x40000000u)) return;
        uint32_t v = MEM32(frm + 0x208);                 /* the skinned visual */
        if (!v) return;
        MEM32(v + 0x224) += 1;
        MEM32(v + 0x1D8) &= 0xFFF9FFFFu;
        discard_hr_bound_vol(v);
        v = MEM32(frm + 0x208);
        MEM32(v + FRM_FLAGS) = (MEM32(v + FRM_FLAGS) & 0xFFFFFEBFu) | 0x40000000u;
        return;
    }
    default: {
        static int st = -1; static uint64_t ncall; static uint32_t fns[16]; static uint64_t cnt[16];
        if (st < 0) { const char *e = getenv("FAMILIA_UPDATE_STATS"); st = e && atoi(e); }
        if (st) { int k; for (k = 0; k < 16 && fns[k] && fns[k] != fn; k++) {}
                  if (k < 16) { fns[k] = fn; cnt[k]++; }
                  if (++ncall % 200000 == 0) { fprintf(stderr, "[updstats] UpdateV calls by target:");
                      for (k = 0; k < 16 && fns[k]; k++) fprintf(stderr, " %08X:%llu", fns[k], (unsigned long long)cnt[k]); fprintf(stderr, "\n"); } }
        g_ecx = frm;
        PUSH32(g_esp, RECOMP_RETADDR);
        g_uv[h].f();
    }
    }
}

/* the head of UpdateChildren, shared with Update (which inlines it) */
/* a frame's three hot cache lines: vtable (0x00), flags (0xAC), type/links (0x110-0x12C) */
static inline void prefetch_frame(uint32_t f) {
    if (!f) return;
    const char *p = (const char *)ADDR(f);
    __builtin_prefetch(p); __builtin_prefetch(p + 0xAC); __builtin_prefetch(p + 0x110);
    __builtin_prefetch(p + 0x1D8); __builtin_prefetch(p + 0x204);   /* visual flags; a joint's state and skinned visual */
}
/* Warm the caches for a subtree before walking it: follow only the links
 * (one cache line per frame) and prefetch every frame's other lines on the way,
 * so the exact walk below finds them loaded instead of taking 3-4 misses in a
 * row per frame. Only a prefetch schedule: the walk reads live links, so a
 * tree changed meanwhile only wastes prefetches. */
static int g_warm = -1;
static void warm_subtree(uint32_t root) {
    uint32_t stack[64]; int sp = 0, n = 0;
    uint32_t c = MEM32(root + FRM_CHILD);
    while (c && n < 160) {
        const char *p = (const char *)ADDR(c);
        __builtin_prefetch(p); __builtin_prefetch(p + 0xAC); __builtin_prefetch(p + 0x1D8); __builtin_prefetch(p + 0x204);
        n++;
        uint32_t ch = MEM32(c + FRM_CHILD), nx = MEM32(c + FRM_NEXT);
        if (ch) { if (nx && sp < 64) stack[sp++] = nx; c = ch; }
        else if (nx) c = nx;
        else c = sp ? stack[--sp] : 0;
    }
}
static uint64_t g_uc_nodes;
static void update_children(uint32_t frm) {
    g_uc_nodes++;
    prefetch_frame(MEM32(frm + FRM_CHILD));
    uint32_t changed = MEM32(frm + FRM_FLAGS) & FRMF_CHANGED;
    if (changed) {
        MEM32(frm + FRM_UPDCNT) += 1;
        if (MEM32(frm + FRM_TYPE) == 1) {
            uint32_t vf = MEM32(frm + VIS_FLAGS);
            if (!(vf & 0x40)) MEM32(frm + VIS_FLAGS) = vf & 0xFFFBFFFFu;
        }
    }
    for (uint32_t c = MEM32(frm + FRM_CHILD); c; c = MEM32(c + FRM_NEXT)) {
        prefetch_frame(MEM32(c + FRM_NEXT));
        if (changed) MEM32(c + FRM_FLAGS) = (MEM32(c + FRM_FLAGS) & 0xFFFFFEDFu) | 0x2000u;
        update_children(c);
        call_update_v(c);
        uint32_t f = MEM32(c + FRM_FLAGS);
        if (f & FRMF_CHANGED) {
            MEM32(c + FRM_FLAGS) = f & 0xBFFFDFFFu;
            MEM32(frm + FRM_FLAGS) = (MEM32(frm + FRM_FLAGS) & 0xFFFFFEBFu) | 0x2000u;
        }
    }
}

/* void I3D_frame::UpdateChildren()  -- LS3DF 0x1001C9A0, thiscall */
void fam_native_update_children(void) {
    if (g_warm < 0) { const char *e = getenv("FAMILIA_WARM"); g_warm = !e || atoi(e); }
    if (g_warm) warm_subtree(g_ecx);
    update_children(g_ecx);
    g_esp += 4;
}

/* void __stdcall I3D_frame::Update()  -- LS3DF 0x1001CA60, this on the stack, ret 4 */
void fam_native_frame_update(void) {
    uint32_t frm = MEM32(g_esp + 4);
    static int stats = -1; static uint32_t seen[4096], nseen, last_rn; static uint64_t calls, dups, frames, nodes0;
    if (stats < 0) { const char *e = getenv("FAMILIA_UPDATE_STATS"); stats = e && atoi(e); }
    if (stats) {
        uint32_t rn = MEM32(0x100AFC24u);
        if (rn != last_rn) { last_rn = rn; nseen = 0; frames++;
            if (frames % 100 == 0) fprintf(stderr, "[updstats] per frame: %.0f Update calls, %.0f on a root already updated this frame, %.0f nodes walked\n",
                                           (double)calls / 100, (double)dups / 100, (double)g_uc_nodes / 100), calls = dups = 0, g_uc_nodes = 0; }
        calls++;
        int d = 0; for (uint32_t i = 0; i < nseen; i++) if (seen[i] == frm) { d = 1; break; }
        if (d) dups++; else if (nseen < 4096) seen[nseen++] = frm;
        (void)nodes0;
    }
    if (g_warm < 0) { const char *e = getenv("FAMILIA_WARM"); g_warm = !e || atoi(e); }
    if (g_warm) warm_subtree(frm);
    update_children(frm);
    call_update_v(frm);
    discard_hr_bound_vol(frm);                             /* inlined in the original */
    uint32_t f = MEM32(frm + FRM_FLAGS);
    if ((f & FRMF_CHANGED) && MEM32(frm + FRM_TYPE) == 1) {
        uint32_t vf = MEM32(frm + VIS_FLAGS);
        if (!(vf & 0x40)) MEM32(frm + VIS_FLAGS) = vf & 0xFFFBFFFFu;
    }
    MEM32(frm + FRM_FLAGS) = f & 0xBFFFDFFFu;
    /* the targets waiting for an update (UpdateTargetList), unless already updating */
    uint32_t n = MEM32(0x100C003Cu);
    if (n && !MEM8(0x100C0048u)) {
        uint32_t list = MEM32(0x100C0034u);
        MEM8(0x100C0048u) = 1;
        recomp_func_t upd = recomp_lookup(0x100599B0u);   /* I3D_target::UpdateFrames, thiscall */
        for (uint32_t i = 0; i < n; i++) {
            g_ecx = MEM32(list + 4 * i);
            PUSH32(g_esp, RECOMP_RETADDR);
            upd();
        }
        MEM32(0x100C003Cu) = 0;
        MEM8(0x100C0048u) = 0;
    }
    g_esp += 4 + 4;
}


/* ------------------------------------------------- hierarchy bound volumes */
/* I3D_frame: m_LocalMatrix 0x50, m_Matrix 0x10, m_HRBoundVol 0xB0 (bbox min,
 * max, then sphere pos 0xC8, radius 0xD4), m_HRBoundVolW 0xD8 (bbox, sphere
 * pos 0xF0, radius 0xFC). Arithmetic as the translated x87 code does it:
 * doubles on the FPU stack, a rounding to float at each dword store. */
#define BV_LOCAL 0xB0
#define BV_WORLD 0xD8
void fam_vmm(const float *V, const float *M, float *R);

static inline float ldf(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static inline void stf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

static void guest_thiscall0(uint32_t fn, uint32_t self) {
    recomp_func_t f = recomp_lookup_manual(fn);
    if (!f) f = recomp_lookup(fn);
    g_ecx = self;
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
}
static void guest_stdcall1(uint32_t fn, uint32_t arg) {
    recomp_func_t f = recomp_lookup_manual(fn);
    if (!f) f = recomp_lookup(fn);
    PUSH32(g_esp, arg);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
}

/* BBoxVertices (LS3DF 0x1006BC00): the box's 8 corners into its static
 * buffer, in its order; the buffer's guard registers a destructor on first use */
static uint32_t bbox_vertices(uint32_t box) {
    if (!(MEM8(0x101C5290u) & 1)) {                   /* first use: let the original register it */
        g_ecx = box; PUSH32(g_esp, RECOMP_RETADDR); recomp_lookup(0x1006BC00u)();
        return 0x101C5230u;
    }
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

/* min/max absorb of one point, the original's compare forms: min takes p
 * when cur > p, max when cur < p (strict; NaN never taken) */
static inline void absorb(uint32_t box, const float p[3]) {
    for (int k = 0; k < 3; k++) {
        float mn = ldf(box + 4 * k), mx = ldf(box + 12 + 4 * k);
        if ((double)mn > (double)p[k]) stf(box + 4 * k, p[k]);
        if ((double)mx < (double)p[k]) stf(box + 12 + 4 * k, p[k]);
    }
}

/* the sphere around a box, as both versions compute it */
static void sphere_of_box(uint32_t box, uint32_t pos, uint32_t radius) {
    double mnx = ldf(box), mny = ldf(box + 4), mnz = ldf(box + 8);
    double dx = (double)ldf(box + 12) - mnx, dy = (double)ldf(box + 16) - mny;
    float dz = (float)((double)ldf(box + 20) - mnz);
    float hx = (float)(dx * 0.5);
    double hy = dy * 0.5, hz = (double)dz * 0.5;
    stf(pos, (float)((double)hx + mnx));
    stf(pos + 4, (float)(hy + mny));
    stf(pos + 8, (float)(hz + mnz));
    double sq = (hz * hz + hy * hy) + (double)hx * (double)hx;
    stf(radius, (float)sqrt(sq));
}

static void update_hr_bound_vol(uint32_t frm) {
    uint32_t type = MEM32(frm + FRM_TYPE);
    if (type == 5) { MEM32(frm + FRM_FLAGS) |= 0x40; return; }
    uint32_t box = frm + BV_LOCAL;
    if (type == 1) {
        if (!(MEM32(frm + VIS_FLAGS) & 0x20000)) guest_stdcall1(MEM32(MEM32(frm) + 0x64), frm);   /* UpdateBoundProc */
        memcpy((void *)ADDR(box), (const void *)ADDR(frm + 0x1AC), 24);
    } else {
        for (int k = 0; k < 3; k++) { MEM32(box + 4 * k) = 0x5A0E1BCAu; MEM32(box + 12 + 4 * k) = 0xDA0E1BCAu; }
    }
    for (uint32_t c = MEM32(frm + FRM_CHILD); c; c = MEM32(c + FRM_NEXT)) {
        if (MEM32(c + FRM_TYPE) == 0xB) continue;                    /* volumes */
        uint32_t f = MEM32(c + FRM_FLAGS);
        if (!(f & 1)) continue;                                      /* off */
        if (!(f & 0x40)) update_hr_bound_vol(c);
        if (!((double)ldf(c + BV_LOCAL) <= (double)ldf(c + BV_LOCAL + 12))) continue;   /* invalid box */
        uint32_t v = bbox_vertices(c + BV_LOCAL);
        if ((MEM32(c + FRM_FLAGS) & 0x14) != 0x14) guest_thiscall0(0x1001A6E0u, c);    /* local matrix */
        const float *m = (const float *)ADDR(c + 0x50);
        for (int i = 0; i < 8; i++) {
            float p[3];
            fam_vmm((const float *)ADDR(v + 12 * i), m, p);
            absorb(box, p);
        }
    }
    if ((double)ldf(box) <= (double)ldf(box + 12)) sphere_of_box(box, frm + 0xC8, frm + 0xD4);
    else MEM32(frm + 0xD4) = 0;
    MEM32(frm + FRM_FLAGS) |= 0x40;
}

/* void __stdcall I3D_frame::UpdateHRBoundVol()  -- LS3DF 0x1001CC50, ret 4 */
void fam_native_update_hr_bound_vol(void) {
    update_hr_bound_vol(MEM32(g_esp + 4));
    g_esp += 8;
}

/* void __stdcall I3D_frame::UpdateHRBoundVolW()  -- LS3DF 0x1001CF90, ret 4 */
void fam_native_update_hr_bound_vol_w(void) {
    uint32_t frm = MEM32(g_esp + 4);
    if (!(MEM32(frm + FRM_FLAGS) & 0x40)) update_hr_bound_vol(frm);
    uint32_t box = frm + BV_WORLD;
    for (int k = 0; k < 3; k++) { MEM32(box + 4 * k) = 0x5A0E1BCAu; MEM32(box + 12 + 4 * k) = 0xDA0E1BCAu; }
    if ((double)ldf(frm + BV_LOCAL) <= (double)ldf(frm + BV_LOCAL + 12)) {
        uint32_t v = bbox_vertices(frm + BV_LOCAL);
        for (int i = 0; i < 8; i++) {
            if (!(MEM32(frm + FRM_FLAGS) & 0x20)) guest_thiscall0(0x1001C7D0u, frm);  /* world matrix */
            float p[3];
            fam_vmm((const float *)ADDR(v + 12 * i), (const float *)ADDR(frm + 0x10), p);
            if (i == 0) { memcpy((void *)ADDR(box + 12), p, 12); memcpy((void *)ADDR(box), p, 12); }
            else absorb(box, p);
        }
        sphere_of_box(box, frm + 0xF0, frm + 0xFC);
    } else {
        MEM32(frm + 0xF0) = 0; MEM32(frm + 0xF4) = 0; MEM32(frm + 0xF8) = 0; MEM32(frm + 0xFC) = 0;
    }
    MEM32(frm + FRM_FLAGS) |= 0x100;
    g_esp += 8;
}

/* ---------------------------------------------------- differential check */
void recomp_ls3df_1001C360_orig(void);
void recomp_ls3df_1001C9A0_orig(void);
void recomp_ls3df_1001CA60_orig(void);

static uint64_t g_rng = 0xD1B54A32D192ED03ull;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 33) % n; }

static int check_update(void) {
    enum { NF = 48, FS = 0x240 };
    uint32_t mem = hle_va(hle_heap_alloc(2 * NF * FS + 128, 1));
    uint32_t A = mem, B = mem + NF * FS, vt = mem + 2 * NF * FS;   /* two copies of one tree, a vtable pair */
    int bad = 0;
    for (int n = 0; n < 20000 && bad < 5; n++) {
        memset((void *)ADDR(A), 0, NF * FS);
        for (int i = 0; i < NF; i++) {
            uint32_t f = A + i * FS;
            MEM32(f) = vt + 8 * rnd(4);                          /* vtable: empty, visual, joint or dummy UpdateV */
            MEM32(f + FRM_TYPE) = rnd(4) ? rnd(7) : 5;
            MEM32(f + FRM_FLAGS) = (uint32_t)rnd(0x10000) | (rnd(2) << 30) | (rnd(2) << 13) | (rnd(2) << 6);
            MEM32(f + VIS_FLAGS) = (uint32_t)rnd(0x80000) | (rnd(2) << 18);
            MEM32(f + FRM_UPDCNT) = rnd(1000);
            if (i) {
                uint32_t p = A + rnd(i) * FS;
                MEM32(f + FRM_NEXT) = MEM32(p + FRM_CHILD); MEM32(p + FRM_CHILD) = f;
                MEM32(f + FRM_PARENT) = p;
            }
        }
        MEM32(vt + 0x44) = 0x10088540u;                          /* vtable 0: the bare ret */
        MEM32(vt + 8 + 0x44) = VA_VISUAL_UPDATEV;                /* vtable 1 (overlapping, +8) */
        MEM32(vt + 16 + 0x44) = VA_JOINT_UPDATEV;
        MEM32(vt + 24 + 0x44) = VA_DUMMY_UPDATEV;
        for (int i = 0; i < NF; i++) {                           /* joints: a skinned visual elsewhere in the tree, or none */
            uint32_t f = A + i * FS;
            MEM32(f + 0x208) = rnd(4) ? A + rnd(NF) * FS : 0;
            MEM32(f + 0x204) = rnd(0x100); MEM32(f + 0x198) = rnd(0x100); MEM32(f + 0x224) = rnd(100);
        }
        /* B = A relocated */
        memcpy((void *)ADDR(B), (void *)ADDR(A), NF * FS);
        for (int i = 0; i < NF; i++) {
            uint32_t f = B + i * FS;
            for (int o = FRM_PARENT; o <= FRM_NEXT; o += 4) if (MEM32(f + o)) MEM32(f + o) += B - A;
            if (MEM32(f + 0x208)) MEM32(f + 0x208) += B - A;
        }
        int which = (int)rnd(2), root = (int)rnd(NF);
        uint32_t esp0 = g_esp; int ok1, ok2;
        if (which == 0) {      /* UpdateChildren, thiscall */
            PUSH32(g_esp, RECOMP_RETADDR); g_ecx = A + root * FS; recomp_ls3df_1001C9A0_orig(); ok1 = g_esp == esp0;
            PUSH32(g_esp, RECOMP_RETADDR); g_ecx = B + root * FS; fam_native_update_children(); ok2 = g_esp == esp0;
        } else {               /* Update, stdcall */
            PUSH32(g_esp, A + root * FS); PUSH32(g_esp, RECOMP_RETADDR); recomp_ls3df_1001CA60_orig(); ok1 = g_esp == esp0;
            PUSH32(g_esp, B + root * FS); PUSH32(g_esp, RECOMP_RETADDR); fam_native_frame_update(); ok2 = g_esp == esp0;
        }
        int diff = -1;
        for (int i = 0; i < NF * FS && diff < 0; i += 4) {
            uint32_t a = MEM32(A + i), b = MEM32(B + i);
            int o = i % FS;
            if ((o == FRM_PARENT || o == FRM_CHILD || o == FRM_NEXT || o == 0x208) && a && b) { if (b - a != B - A) diff = i; }
            else if (a != b) diff = i;
        }
        if (diff >= 0 || !ok1 || !ok2) {
            if (!bad) fprintf(stderr, "[native] %s MISMATCH (case %d): frame %d +%03X: %08X/%08X esp %d/%d\n",
                              which ? "Update" : "UpdateChildren", n, diff / FS, diff % FS,
                              diff >= 0 ? MEM32(A + diff) : 0, diff >= 0 ? MEM32(B + diff) : 0, ok1, ok2);
            bad++;
        }
    }
    fprintf(stderr, "[native] I3D_frame::Update/UpdateChildren: %s against the translated originals (20000 random trees)\n",
            bad ? "DIFFER" : "match");
    hle_heap_free(hle_ptr(mem));
    return bad;
}


static uint32_t call_gcf(void (*fn)(void), uint32_t self, uint32_t name, uint32_t flags, int *esp_ok) {
    uint32_t esp0 = g_esp;
    PUSH32(g_esp, flags); PUSH32(g_esp, name); PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    fn();
    *esp_ok = g_esp == esp0;
    return g_eax;
}


static float rndf(void) {
    int e = (int)rnd(24) - 8;
    float f = (float)((double)(int32_t)(rnd(0x7FFFFFFF) - 0x3FFFFFFF) / 1073741824.0);
    return ldexpf(f, e);
}
void recomp_ls3df_1001CC50_orig(void);
void recomp_ls3df_1001CF90_orig(void);

static int check_bounds(void) {
    enum { NF = 32, FS = 0x200 };
    uint32_t mem = hle_va(hle_heap_alloc(2 * NF * FS + 128, 1));
    uint32_t A = mem, B = mem + NF * FS, vt = mem + 2 * NF * FS;
    int bad = 0, ncase = 0;
    for (int n = 0; n < 20000 && bad < 5; n++) {
        memset((void *)ADDR(A), 0, NF * FS);
        for (int i = 0; i < NF; i++) {
            uint32_t f = A + i * FS;
            MEM32(f) = vt;
            static const uint32_t types[] = { 0, 1, 6, 5, 11, 9 };
            MEM32(f + FRM_TYPE) = rnd(5) ? types[rnd(3) ? 2 : rnd(6)] : 1;
            MEM32(f + FRM_FLAGS) = (rnd(8) ? 1u : 0u) | (rnd(3) ? 0x40u : 0) | (rnd(4) ? 0x14u : rnd(32) & 0x1Cu)
                                 | (rnd(4) ? 0x20u : 0) | (rnd(2) ? 0x100u : 0);
            MEM32(f + VIS_FLAGS) = 0x20000u;                       /* bound volume valid: no virtual call */
            for (int k = 0; k < 16; k++) { stf(f + 0x10 + 4 * k, rndf()); stf(f + 0x50 + 4 * k, rndf()); }
            for (int k = 0; k < 3; k++) {                          /* scale, rotation */
                stf(f + 0x90 + 4 * k, rndf()); }
            for (int k = 0; k < 4; k++) stf(f + 0x9C + 4 * k, rndf());
            for (int b = 0; b < 2; b++) {                          /* local box of this frame (+0xB0) and visual box (+0x1AC) */
                uint32_t box = f + (b ? 0x1AC : 0xB0);
                for (int k = 0; k < 3; k++) {
                    float a = rndf(), c = rndf();
                    if (rnd(10)) { if (a > c) { float t = a; a = c; c = t; } }
                    stf(box + 4 * k, a); stf(box + 12 + 4 * k, c);
                }
            }
            if (i) {
                uint32_t p = A + rnd(i) * FS;
                MEM32(f + FRM_NEXT) = MEM32(p + FRM_CHILD); MEM32(p + FRM_CHILD) = f;
                MEM32(f + FRM_PARENT) = p;
            }
        }
        memcpy((void *)ADDR(B), (void *)ADDR(A), NF * FS);
        for (int i = 0; i < NF; i++) {
            uint32_t f = B + i * FS;
            for (int o = FRM_PARENT; o <= FRM_NEXT; o += 4) if (MEM32(f + o)) MEM32(f + o) += B - A;
        }
        int which = (int)rnd(2), root = (int)rnd(4);
        uint32_t esp0 = g_esp; int ok1, ok2;
        void (*orig)(void) = which ? recomp_ls3df_1001CF90_orig : recomp_ls3df_1001CC50_orig;
        void (*nat)(void) = which ? fam_native_update_hr_bound_vol_w : fam_native_update_hr_bound_vol;
        uint8_t buf1[96], buf2[96];
        PUSH32(g_esp, A + root * FS); PUSH32(g_esp, RECOMP_RETADDR); orig(); ok1 = g_esp == esp0;
        memcpy(buf1, (void *)ADDR(0x101C5230u), 96);
        PUSH32(g_esp, B + root * FS); PUSH32(g_esp, RECOMP_RETADDR); nat(); ok2 = g_esp == esp0;
        memcpy(buf2, (void *)ADDR(0x101C5230u), 96);
        ncase++;
        int diff = -1;
        for (int i = 0; i < NF * FS && diff < 0; i += 4) {
            uint32_t a = MEM32(A + i), b = MEM32(B + i);
            int o = i % FS;
            if ((o == FRM_PARENT || o == FRM_CHILD || o == FRM_NEXT) && a && b) { if (b - a != B - A) diff = i; }
            else if (a != b) diff = i;
        }
        if (diff >= 0 || !ok1 || !ok2 || memcmp(buf1, buf2, 96)) {
            if (!bad) fprintf(stderr, "[native] %s MISMATCH (case %d): frame %d +%03X: %08X/%08X esp %d/%d\n",
                              which ? "UpdateHRBoundVolW" : "UpdateHRBoundVol", n, diff / FS, diff % FS,
                              diff >= 0 ? MEM32(A + diff) : 0, diff >= 0 ? MEM32(B + diff) : 0, ok1, ok2);
            bad++;
        }
    }
    fprintf(stderr, "[native] I3D_frame::UpdateHRBoundVol/W: %s against the translated originals (%d random trees)\n",
            bad ? "DIFFER" : "match", ncase);
    hle_heap_free(hle_ptr(mem));
    return bad;
}

int fam_native_frame_check(void) {
    enum { NF = 64, NAMEMAX = 12 };
    static const char alpha[] = "aAbB?*._1";
    uint32_t mem = hle_va(hle_heap_alloc(NF * 0x140 + NF * NAMEMAX * 2 + 64, 1));
    uint32_t frames = mem, names = mem + NF * 0x140, pat = names + NF * NAMEMAX;
    int bad = 0;
    for (int n = 0; n < 20000 && bad < 5; n++) {
        memset((void *)ADDR(frames), 0, NF * 0x140);
        for (int i = 0; i < NF; i++) {              /* a random tree: each frame hangs under an earlier one */
            uint32_t f = frames + i * 0x140;
            MEM32(f + FRM_TYPE) = rnd(18);
            char *s = (char *)ADDR(names + i * NAMEMAX);
            int len = (int)rnd(NAMEMAX - 1);
            for (int k = 0; k < len; k++) s[k] = "aAbB._1"[rnd(7)];
            s[len] = 0;
            MEM32(f + FRM_NAME) = rnd(8) ? names + i * NAMEMAX : 0;
            if (i) {
                uint32_t p = frames + rnd(i) * 0x140;
                MEM32(f + FRM_NEXT) = MEM32(p + FRM_CHILD); MEM32(p + FRM_CHILD) = f;
            }
        }
        char *q = (char *)ADDR(pat);                 /* a pattern: often a real name, case and wildcards mixed in */
        if (rnd(2)) { strcpy(q, (const char *)ADDR(names + rnd(NF) * NAMEMAX));
                      for (char *c = q; *c; c++) if (!rnd(4)) *c = alpha[rnd(sizeof alpha - 1)]; }
        else { int len = (int)rnd(NAMEMAX - 1); for (int k = 0; k < len; k++) q[k] = alpha[rnd(sizeof alpha - 1)]; q[len] = 0; }
        uint32_t flags = (rnd(3) ? 0xFFFF : rnd(0x4000)) | (rnd(2) << 16) | (rnd(2) << 17);
        uint32_t name = rnd(10) ? pat : 0, root = frames + rnd(4) * 0x140;
        int ok1, ok2;
        uint32_t r1 = call_gcf(recomp_ls3df_1001C360_orig, root, name, flags, &ok1);
        uint32_t r2 = call_gcf(fam_native_get_child_frame, root, name, flags, &ok2);
        if (r1 != r2 || !ok1 || !ok2) {
            if (!bad) fprintf(stderr, "[native] GetChildFrame MISMATCH (case %d): '%s' flags %X -> %08X/%08X esp %d/%d\n",
                              n, name ? q : "(null)", flags, r1, r2, ok1, ok2);
            bad++;
        }
    }
    fprintf(stderr, "[native] I3D_frame::GetChildFrame: %s against the translated original (20000 random trees)\n",
            bad ? "DIFFERS" : "matches");
    hle_heap_free(hle_ptr(mem));
    return bad + check_update() + check_bounds();
}
