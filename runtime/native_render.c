/*
 * native_render.c - native pieces of LS3D's render pass (I3D_driver::Render
 * and the per-object chain under it), from our reading of LS3DF.dll 1.2 with
 * the mafia-re decompilation (render.cpp) as the map.
 *
 * Loop splices (tools/splice_loops.py, config/splices.json, "fn":
 * fam_render_splice): the translated function runs as before up to the loop
 * head, the native code does the loop and control continues at its exit.
 *
 *   id 100: Render's per-frame pass over the driver's sounds (0x10062C14 ..
 *           0x10062C7B). Every sound in the scene is visited each frame (1500+
 *           in the city); each visit missed the cache on the sound object.
 *           The native loop is the same pass with the objects prefetched
 *           ahead; I3D_sound::m_UpdateSound stays the translated function,
 *           called for the same sounds in the same order.
 *
 * Checking (FAMILIA_NATIVE_CHECK=1): for the first frames the translated loop
 * runs instead, with m_UpdateSound's callers recorded through a hook
 * (fam_native_update_sound), and the native loop's decisions, worked out
 * beforehand without side effects, are compared with what it did.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "win32hle.h"
#include "recomp_types.h"

void recomp_ls3df_10058540_orig(void);

int fam_native_render_on = 1;
static int g_check = -1, g_check_left = 120, g_rec = 0;

#define SND_FRMFLAGS  0xAC
#define SND_LASTCOUNT 0x118
#define SND_FLAGS     0x140
#define SND_STATE     0x144
#define SND_UPDCOUNT  0x14C
#define SND_FADESECT  0x1B0
#define CAMERA_CHANGED 0x101C4CE0u     /* renderCameraChanged */

/* the m_UpdateSound calls the translated loop makes while being checked */
#define MAXREC 8192
static uint32_t g_rec_snd[MAXREC];
static int g_nrec;

/* I3D_sound::m_UpdateSound (0x10058540, thiscall, no arguments) */
void fam_native_update_sound(void) {
    if (g_rec && g_nrec < MAXREC) g_rec_snd[g_nrec++] = g_ecx;
    recomp_ls3df_10058540_orig();
}
static void call_update_sound(uint32_t snd) {
    g_ecx = snd;
    PUSH32(g_esp, RECOMP_RETADDR);
    fam_native_update_sound();
}

/* the decision for one sound: 0 skip, 1 update count changed (count copied,
 * flags | 0x83, then the call), 2 the call alone */
static int decide(uint32_t snd, int cam_changed) {
    if (!(MEM8(snd + SND_FRMFLAGS) & 1)) return 0;
    if (MEM8(snd + SND_STATE) == 1) return 0;
    if (MEM32(snd + SND_LASTCOUNT) != MEM32(snd + SND_UPDCOUNT)) return 1;
    if (cam_changed || MEM32(snd + SND_FADESECT) || MEM32(snd + SND_FLAGS)) return 2;
    return 0;
}

/* at L_10062C14: edi = the driver, edx = sounds left (> 0), [ebp-0x10] the counter's slot */
uint32_t fam_sounds_last;                                  /* growth report: sounds walked */
static void sound_pass(void) {
    uint32_t self = g_edi, slot = g_ebp - 0x10;
    fam_sounds_last = g_edx;
    uint32_t n = g_edx, last = 0;
    while (n) {
        uint32_t arr = MEM32(self + 0x38);
        if (n > 8) {                                      /* the sound 8 ahead: its three lines */
            const uint8_t *p = (const uint8_t *)ADDR(MEM32(arr + (n - 9) * 4));
            __builtin_prefetch(p + SND_FRMFLAGS); __builtin_prefetch(p + SND_LASTCOUNT); __builtin_prefetch(p + SND_FLAGS);
        }
        uint32_t snd = MEM32(arr + (n - 1) * 4);
        n--;
        MEM32(slot) = n;
        last = snd;
        int d = decide(snd, MEM8(CAMERA_CHANGED) != 0);
        if (d == 1) { MEM32(snd + SND_LASTCOUNT) = MEM32(snd + SND_UPDCOUNT); MEM32(snd + SND_FLAGS) |= 0x83; }
        if (d) { call_update_sound(snd); n = MEM32(slot); }
    }
    g_edx = 0; g_ecx = last;
}

/* the check: decisions in advance, then the translated loop with its calls recorded */
static struct { uint32_t snd; int d; } g_want[MAXREC];
static int g_nwant, g_pending = -1;
static void check_begin(void) {
    uint32_t self = g_edi, arr = MEM32(self + 0x38);
    int cam = MEM8(CAMERA_CHANGED) != 0;
    g_nwant = 0;
    for (uint32_t n = g_edx; n && g_nwant < MAXREC; n--) {
        uint32_t snd = MEM32(arr + (n - 1) * 4);
        int d = decide(snd, cam);
        if (d) { g_want[g_nwant].snd = snd; g_want[g_nwant].d = d; g_nwant++; }
    }
    g_nrec = 0; g_rec = 1;
}
static void check_end(void) {
    g_rec = 0;
    int bad = g_nrec != g_nwant;
    for (int i = 0; !bad && i < g_nrec; i++) bad = g_rec_snd[i] != g_want[i].snd;
    static int checked;
    if (bad) {
        fprintf(stderr, "[native] Render sound pass MISMATCH: %d calls translated, %d expected: native render loops off\n", g_nrec, g_nwant);
        fam_native_render_on = 0;
    } else if (++checked == 1 || g_check_left == 1)
        fprintf(stderr, "[native] Render sound pass: matches the translated loop (%d sounds updated this frame)\n", g_nrec);
}

static int shadow_filter(void);
static int projector_loop(void);
static int clipmesh_loop(void);
/* the shadow filter's check: native first (kept), restored, then the x86 loop; compared at the exit */
static int g_sf_left = 50, g_sf_pending;
static uint32_t g_sf_regs[6], g_sf_lo, g_sf_n;
static uint8_t *g_sf_out;
static int g_pl_left = 50, g_pl_pending;
static uint32_t g_pl_regs[6], g_pl_lo, g_pl_n, g_pl_stack[5];
static uint8_t *g_pl_out;
static int g_cm_left = 50, g_cm_pending;
static uint32_t g_cm_regs[7], g_cm_lo, g_cm_n, g_cm_tmp[3];
static uint8_t *g_cm_out;
int fam_render_splice(int id) {
    if (id == 103) {
        if (!fam_native_render_on || g_cm_pending) return 0;   /* the x86 loop re-enters its head every vertex */
        if (g_check < 0) { const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_check = e && atoi(e); }
        if (!g_check || g_cm_left <= 0) return clipmesh_loop();
        uint32_t r0[7] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp }, esp = g_esp;
        uint32_t lo = g_esi, n = g_ebp * 0x14, t0[3];
        for (int i = 0; i < 3; i++) t0[i] = MEM32(esp + 0x14 + 4 * i);
        uint8_t *save = malloc(n ? n : 1);
        g_cm_out = realloc(g_cm_out, n ? n : 1);
        if (!save || !g_cm_out || !n) { free(save); return clipmesh_loop(); }
        memcpy(save, (void *)ADDR(lo), n);
        clipmesh_loop();
        memcpy(g_cm_out, (void *)ADDR(lo), n);
        uint32_t r1[7] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp };
        memcpy(g_cm_regs, r1, sizeof r1);
        for (int i = 0; i < 3; i++) { g_cm_tmp[i] = MEM32(esp + 0x14 + 4 * i); MEM32(esp + 0x14 + 4 * i) = t0[i]; }
        memcpy((void *)ADDR(lo), save, n); free(save);
        g_eax = r0[0]; g_ebx = r0[1]; g_ecx = r0[2]; g_edx = r0[3]; g_esi = r0[4]; g_edi = r0[5]; g_ebp = r0[6];
        g_cm_lo = lo; g_cm_n = n; g_cm_pending = 1;
        return 0;                                            /* the x86 loop runs */
    }
    if (id == 102) {
        if (!fam_native_render_on || g_pl_pending) return 0;   /* the x86 loop re-enters its head every vertex */
        if (g_check < 0) { const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_check = e && atoi(e); }
        if (!g_check || g_pl_left <= 0) return projector_loop();
        /* check: the native loop first (kept), state put back, then the x86 loop; compared at the exit */
        uint32_t r0[6] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi }, esp = g_esp;
        uint32_t lo = g_ecx, n = g_esi * 0x18, st0[5];
        for (int i = 0; i < 5; i++) st0[i] = MEM32(esp + 0x10 + 4 * i);
        uint8_t *save = malloc(n ? n : 1);
        g_pl_out = realloc(g_pl_out, n ? n : 1);
        if (!save || !g_pl_out || !n) { free(save); return projector_loop(); }
        memcpy(save, (void *)ADDR(lo), n);
        projector_loop();
        memcpy(g_pl_out, (void *)ADDR(lo), n);
        uint32_t r1[6] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi };
        memcpy(g_pl_regs, r1, sizeof r1);
        for (int i = 0; i < 5; i++) { g_pl_stack[i] = MEM32(esp + 0x10 + 4 * i); MEM32(esp + 0x10 + 4 * i) = st0[i]; }
        memcpy((void *)ADDR(lo), save, n); free(save);
        g_eax = r0[0]; g_ebx = r0[1]; g_ecx = r0[2]; g_edx = r0[3]; g_esi = r0[4]; g_edi = r0[5];
        g_pl_lo = lo; g_pl_n = n; g_pl_pending = 1;
        return 0;                                            /* the x86 loop runs */
    }
    if (id == 101) {
        if (!fam_native_render_on) return 0;
        if (g_check < 0) { const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_check = e && atoi(e); }
        if (!g_check || g_sf_left <= 0) return shadow_filter();
        uint32_t r0[6] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi };
        uint32_t lo = g_edi, n = g_ecx * MEM32(g_ebp - 4);
        uint8_t *save = malloc(n);
        g_sf_out = realloc(g_sf_out, n);
        if (!save || !g_sf_out || !n) { free(save); return shadow_filter(); }
        memcpy(save, (void *)ADDR(lo), n);
        if (!shadow_filter()) { free(save); return 0; }
        memcpy(g_sf_out, (void *)ADDR(lo), n);
        uint32_t r1[6] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi };
        memcpy(g_sf_regs, r1, sizeof r1);
        memcpy((void *)ADDR(lo), save, n); free(save);
        g_eax = r0[0]; g_ebx = r0[1]; g_ecx = r0[2]; g_edx = r0[3]; g_esi = r0[4]; g_edi = r0[5];
        g_sf_lo = lo; g_sf_n = n; g_sf_pending = 1;
        return 0;                                            /* the x86 loop runs */
    }
    if (!fam_native_render_on || id != 100 || g_pending >= 0) return 0;   /* re-entered at its head every sound while checked */
    if (g_check < 0) { const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_check = e && atoi(e); }
    if (g_check && g_check_left > 0) { check_begin(); g_pending = id; return 0; }   /* the translated loop runs */
    sound_pass();
    return 1;
}
void fam_render_splice_exit(void) {
    if (g_cm_pending) {
        g_cm_pending = 0; g_cm_left--;
        uint32_t r[7] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp };
        int bad = memcmp(g_cm_out, (void *)ADDR(g_cm_lo), g_cm_n) != 0, breg = -1;
        for (int i = 0; i < 7; i++) if (r[i] != g_cm_regs[i]) { bad = 1; if (breg < 0) breg = i; }
        for (int i = 0; i < 3; i++) if (MEM32(g_esp + 0x14 + 4 * i) != g_cm_tmp[i]) bad = 1;
        static uint32_t total, largest;
        if (bad) {
            uint32_t k = 0; while (k < g_cm_n && g_cm_out[k] == ((uint8_t *)ADDR(g_cm_lo))[k]) k++;
            fprintf(stderr, "[native] ClipMesh vertex loop MISMATCH (byte %u of %u, register %d): native render loops off\n", k, g_cm_n, breg);
            fam_native_render_on = 0;
        } else {
            total += g_cm_n / 0x14; if (g_cm_n / 0x14 > largest) largest = g_cm_n / 0x14;
            if (g_cm_left == 0)
                fprintf(stderr, "[native] ClipMesh vertex loop: matches the x86 loop (50 meshes, %u vertices, largest %u)\n", total, largest);
        }
        return;
    }
    if (g_pl_pending) {
        g_pl_pending = 0; g_pl_left--;
        uint32_t r[6] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi };
        int bad = memcmp(g_pl_out, (void *)ADDR(g_pl_lo), g_pl_n) != 0;
        for (int i = 0; i < 6; i++) if (r[i] != g_pl_regs[i]) bad = 1;
        for (int i = 0; i < 5; i++) if (MEM32(g_esp + 0x10 + 4 * i) != g_pl_stack[i]) bad = 1;
        if (bad) {
            uint32_t k = 0; while (k < g_pl_n && g_pl_out[k] == ((uint8_t *)ADDR(g_pl_lo))[k]) k++;
            fprintf(stderr, "[native] projector vertex loop MISMATCH (first byte %u of %u): native render loops off\n", k, g_pl_n);
            fam_native_render_on = 0;
        } else {
            static uint32_t total, largest;
            total += g_pl_n / 0x18; if (g_pl_n / 0x18 > largest) largest = g_pl_n / 0x18;
            if (g_pl_left == 0)
                fprintf(stderr, "[native] projector vertex loop: matches the x86 loop (50 batches, %u vertices, largest %u)\n", total, largest);
        }
        return;
    }
    if (g_sf_pending) {
        g_sf_pending = 0; g_sf_left--;
        uint32_t r[6] = { g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi };
        int bad = memcmp(g_sf_out, (void *)ADDR(g_sf_lo), g_sf_n) != 0;
        for (int i = 0; i < 6; i++) if (r[i] != g_sf_regs[i]) bad = 1;
        if (bad) { fprintf(stderr, "[native] shadow texture filter MISMATCH: native render loops off\n"); fam_native_render_on = 0; }
        else if (g_sf_left == 49 || g_sf_left == 0) fprintf(stderr, "[native] shadow texture filter: matches the x86 loop (%u bytes, %d checks left)\n", g_sf_n, g_sf_left);
        return;
    }
    if (g_pending < 0) return;
    g_pending = -1;
    check_end();
    g_check_left--;
}

/* ===================================================================
 * I3D_object::m_RenderFGroup (LS3DF 0x10039280, thiscall, ret 4): one face
 * group of an object, drawn. Transcribed from the original's single-pass
 * branch (HWCAPS_SINGLEPASS, which our device has: m_HardwareCaps 0x81);
 * the multi-pass branch runs the translated original. The render-state
 * helpers keep the driver's cached values (m_Curr*) exactly as the inlined
 * originals do, and every device call is the same COM call through the
 * device's vtable, so the D3D layer sees the same calls in the same order.
 * =================================================================== */
void recomp_ls3df_10039280_orig(void);
void recomp_ls3df_1001C7D0(void);   /* UpdateWMatrixProc (its probe wrapper) */

#define G_DEV          0x101C597Cu
#define G_CAPS         0x101C563Cu
#define G_MODULATE2X   0x101C5400u
#define G_CULLMODE     0x101C5944u
#define G_ALPHAOP      0x101C5974u
#define G_COLOROP      0x101C545Cu
#define G_COLOROP1     0x101C5960u
#define G_ALPHASTATE   0x101C5704u
#define G_BLENDSTATE   0x101C5474u
#define G_CULLSTATE    0x101C558Cu
#define G_VTXFORMAT    0x101C5480u
#define G_LVERTEXVS    0x101C59B8u
#define G_LVERTEX2VS   0x101C59BCu
#define G_VERTEXVS     0x101C59B4u
#define G_ALPHAREF     0x100A9948u
#define G_IDENTITY     0x1009C2F8u
/* device vtable offsets */
#define D_SETTRANSFORM 0x094
#define D_SETRS        0x0C8
#define D_SETTEXTURE   0x0F4
#define D_SETTSS       0x0FC
#define D_DIP          0x11C
#define D_SETVS        0x130
#define D_SETSTREAM    0x14C
#define D_SETINDICES   0x154

uint32_t hle_call_guest(uint32_t va, int nargs, const uint32_t *args);
recomp_func_t recomp_lookup_import(uint32_t va);
/* a COM call on the device: this and up to 5 arguments, stdcall */
static void dev(uint32_t off, int n, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    uint32_t dv = MEM32(G_DEV), va = MEM32(MEM32(dv) + off);
    uint32_t args[5] = { a, b, c, d, e };
    for (int k = n - 1; k >= 0; k--) PUSH32(g_esp, args[k]);
    PUSH32(g_esp, dv);
    PUSH32(g_esp, RECOMP_RETADDR);
    /* the slot's shim, resolved once (the device's vtable never changes); under the
     * check the calls go through shim_entry, which records them */
    static struct { uint32_t va; recomp_func_t f; } cache[0x200 / 4];
    recomp_func_t hle_shim_fn(uint32_t va);
    extern int hle_shimrec;
    recomp_func_t f = NULL;
    if (!hle_shimrec) {
        if (cache[off / 4].va != va) { cache[off / 4].va = va; cache[off / 4].f = hle_shim_fn(va); }
        f = cache[off / 4].f;
    }
    if (!f) { f = recomp_lookup_fast(va); if (!f) f = recomp_lookup_import(va); }
    f();
}
static void set_rs(uint32_t st, uint32_t v) { dev(D_SETRS, 2, st, v, 0, 0, 0); }
static void set_tss(uint32_t stage, uint32_t t, uint32_t v) { dev(D_SETTSS, 3, stage, t, v, 0, 0); }
/* the inlined helpers, with the driver's caches */
static void h_alphaop(uint32_t op) { if (MEM32(G_ALPHAOP) == op) return; MEM32(G_ALPHAOP) = op; set_tss(0, 4, op); }
static void h_colorop(uint32_t op) { if (MEM32(G_COLOROP) == op) return; MEM32(G_COLOROP) = op; set_tss(0, 1, op); }
static void h_colorop1(uint32_t op) { if (MEM32(G_COLOROP1) == op) return; MEM32(G_COLOROP1) = op; set_tss(1, 1, op); }
static void h_alphastate(uint32_t st) {
    if (MEM32(G_ALPHASTATE) == st) return;
    MEM32(G_ALPHASTATE) = st;
    switch (st) {
    case 0: set_rs(0x1B, 0); set_rs(0x0F, 0); break;                         /* none */
    case 1: set_rs(0x1B, 1); set_rs(0x0F, 1); set_rs(0x18, 1); break;        /* blend */
    case 3: set_rs(0x1B, 0); set_rs(0x0F, 1); set_rs(0x18, MEM32(G_ALPHAREF)); break;   /* test */
    }
}
static void h_blendstate(uint32_t st) {
    if (MEM32(G_BLENDSTATE) == st) return;
    MEM32(G_BLENDSTATE) = st;
    if (st == 0) { set_rs(0x13, 5); set_rs(0x14, 6); }                        /* alpha */
    else { set_rs(0x13, 5); set_rs(0x14, 2); }                                /* add alpha */
}
/* (uchar)(env_alpha * 255.0f) << 24, __ftol truncation */
static uint32_t factor_of(uint32_t mat) {
    float a; uint32_t w = MEM32(mat + 0x3C); memcpy(&a, &w, 4);
    double v = (double)a * 255.0;
    int64_t t = (v > -9.2e18 && v < 9.2e18) ? (int64_t)v : INT64_MIN;
    return (uint32_t)(uint8_t)t << 24;
}

static long g_rfg_native, g_rfg_fallback;

/* Prefetch along the sorted draw order (AddFGHlpStruct, 0x101C4C60: face groups at +0x1C,
 * count +0x24, sorted indices at +0x2C as {sort_value, index}): while one face group is
 * drawn, the next ones' data is fetched stage by stage, each stage reading pointers the
 * previous one brought in. Only a schedule: the draw itself reads everything as before. */
#define AFG 0x101C4C60u
static inline int ptr_ok(uint32_t p) { return p >= 0x10000u; }
static inline void pf(uint32_t a) { __builtin_prefetch((const void *)ADDR(a)); }
static void prefetch_ahead(uint32_t h) {
    static int pos = -1, dir = 1;
    uint32_t arr = MEM32(AFG + 0x1C), n = MEM32(AFG + 0x24), sort = MEM32(AFG + 0x2C);
    if (!ptr_ok(arr) || !ptr_ok(sort) || h < arr || h >= arr + n * 0x20u) return;
    uint32_t idx = (h - arr) / 0x20u;
    if (pos >= 0 && pos + dir >= 0 && (uint32_t)(pos + dir) < n && MEM32(sort + (uint32_t)(pos + dir) * 8 + 4) == idx) pos += dir;
    else if (pos >= 0 && pos - dir >= 0 && (uint32_t)(pos - dir) < n && MEM32(sort + (uint32_t)(pos - dir) * 8 + 4) == idx) { dir = -dir; pos += dir; }
    else {
        pos = -1;
        for (uint32_t i = 0; i < n; i++) if (MEM32(sort + i * 8 + 4) == idx) { pos = (int)i; break; }
        if (pos < 0) return;
        dir = (uint32_t)pos < (uint32_t)MEM32(AFG + 0x38) ? 1 : -1;   /* the transparent ones are drawn from the last */
    }
    for (int k = 4; k >= 1; k--) {
        int q = pos + k * dir;
        if (q < 0 || (uint32_t)q >= n) continue;
        uint32_t rfg = arr + MEM32(sort + (uint32_t)q * 8 + 4) * 0x20u;
        if (k == 4) { pf(rfg); continue; }
        uint32_t vis = MEM32(rfg), fg = MEM32(rfg + 0x10);
        if (!ptr_ok(vis) || !ptr_ok(fg)) continue;
        if (k == 3) { pf(vis + 0x1D8); pf(vis + 0x200); pf(vis + 0xAC); pf(vis + 0x10); pf(fg); continue; }
        uint32_t mat = MEM32(fg + 4), cl = MEM32(vis + 0x20C), mesh = MEM32(vis + 0x1E0);
        if (cl > 16) continue;
        uint32_t l = MEM32(vis + 0x1E4 + cl * 4);
        if (k == 2) { if (ptr_ok(mat)) { pf(mat); pf(mat + 0x38); } if (ptr_ok(l)) pf(l); if (ptr_ok(mesh)) pf(mesh + 0xC + cl * 4); continue; }
        /* k == 1 */
        if (ptr_ok(mesh)) { uint32_t lev = MEM32(mesh + 0xC + cl * 4); if (ptr_ok(lev)) pf(lev); }
        if (ptr_ok(l)) { uint32_t buf = MEM32(l + 0x10); if (ptr_ok(buf)) pf(buf); }
        uint32_t tex = MEM32(rfg + 0x18); if (ptr_ok(tex)) pf(tex);
    }
}
static void render_fgroup(uint32_t obj, uint32_t h) {
    uint32_t fg = MEM32(h + 0x10), mat = MEM32(fg + 4), l, nverts, src = obj;
    /* the LOD: a proxy draws its owner's buffers with this mesh's counts */
    if (MEM32(obj + 0x1D8) & 0x10000) src = MEM32(obj + 8);
    if (mat && (MEM32(mat + 8) & 0x800)) {
        l = MEM32(src + 0x1E4);
        nverts = MEM32(MEM32(MEM32(obj + 0x1E0) + 0xC) + 8);
    } else {
        uint32_t cl = MEM32(src + 0x20C);
        l = MEM32(src + 0x1E4 + cl * 4);
        nverts = MEM32(MEM32(MEM32(obj + 0x1E0) + 0xC + cl * 4) + 8);
    }
    uint32_t vb = MEM32(l + 0x14), reset = 0;
    h_alphaop(4);
    if (!mat) {
        h_colorop(MEM32(G_MODULATE2X));
        h_colorop1(1);
        h_alphastate(0);
        dev(D_SETTEXTURE, 2, 0, 0, 0, 0, 0);
    } else {
        uint32_t flags = MEM32(mat + 8), blend = (flags >> 8) & 7;
        if (MEM32(h + 0x1C)) {                                   /* an environment map */
            if (MEM32(h + 0x18)) {                               /* with a texture: stage 1 blends it */
                switch (blend) {
                case 1: h_colorop1(0x0E); set_rs(0x3C, factor_of(mat)); break;
                case 2: h_colorop1(0x10); break;
                case 3: h_colorop1(4); break;
                case 4: h_colorop1(MEM32(G_MODULATE2X)); break;
                case 5: h_colorop1(7); break;
                default: break;                                  /* the original prints a debug message */
                }
                dev(D_SETTEXTURE, 2, 1, MEM32(MEM32(h + 0x1C) + 8), 0, 0, 0);
            } else {                                             /* alone: stage 0, second coordinates */
                switch (blend) {
                case 1: h_colorop(0x0E); set_rs(0x3C, factor_of(mat)); break;
                case 2: h_colorop(0x10); break;
                case 3: h_colorop(4); break;
                case 4: h_colorop(MEM32(G_MODULATE2X)); break;
                case 5: h_colorop(7); break;
                default: break;
                }
                dev(D_SETTEXTURE, 2, 0, MEM32(MEM32(h + 0x1C) + 8), 0, 0, 0);
                set_tss(0, 0x0B, 1);
                h_colorop1(1);
                reset = 1;
            }
        } else h_colorop1(1);
        if (flags & 0x10000) { set_tss(0, 0x0D, 3); reset |= 2; }
        if (flags & 0x20000) { set_tss(0, 0x0E, 3); reset |= 4; }
        int blendpath = 0;
        if (flags & 0x80000000u) { h_alphastate(1); h_blendstate(1); }
        else {
            if (flags & 0x40000000u) blendpath = 1;
            else {
                float fa, fb, fc; uint32_t w;
                w = MEM32(obj + 0x218); memcpy(&fa, &w, 4);
                w = MEM32(obj + 0x214); memcpy(&fb, &w, 4);
                w = MEM32(mat + 0x38); memcpy(&fc, &w, 4);
                double p = (double)fa * (double)fb * (double)fc;
                if (p < 1.0) blendpath = 1;                      /* fcomp: below, not unordered */
            }
            if (blendpath) {
                h_alphastate(1);
                if (MEM32(mat + 0x2C) || MEM32(mat + 0x30) || MEM32(mat + 0x34)) h_blendstate(1);
                else h_blendstate(0);
            } else if (flags & 0x20000000u) { h_alphastate(3); h_blendstate(0); }
            else h_alphastate(0);
        }
        uint32_t cull = (((~flags) & 0x10000000u) | 0x08000000u) >> 27;
        if (MEM32(G_CULLSTATE) != cull) {
            MEM32(G_CULLSTATE) = cull;
            set_rs(0x16, cull == 3 ? MEM32(G_CULLMODE) : cull);
        }
        if (MEM32(h + 0x18)) {
            h_colorop(MEM32(G_MODULATE2X));
            dev(D_SETTEXTURE, 2, 0, MEM32(MEM32(h + 0x18) + 8), 0, 0, 0);
        } else if (!MEM32(h + 0x1C)) {
            h_colorop(MEM32(G_MODULATE2X));
            dev(D_SETTEXTURE, 2, 0, 0, 0, 0, 0);
        }
    }
    if (MEM8(G_CAPS) & 1) {                                     /* hardware T&L: the world matrix */
        uint32_t m;
        if (MEM8(obj + 0x1D8) & 0x40) m = G_IDENTITY;
        else {
            if (!(MEM8(obj + 0xAC) & 0x20)) { g_ecx = obj; PUSH32(g_esp, RECOMP_RETADDR); recomp_ls3df_1001C7D0(); }
            m = obj + 0x10;
        }
        dev(D_SETTRANSFORM, 2, 0x100, m, 0, 0, 0);
    }
    uint32_t buffer = MEM32(l + 0x10), fmt = MEM32(buffer + 0xC);
    if (MEM32(G_VTXFORMAT) != fmt) {
        MEM32(G_VTXFORMAT) = fmt;
        switch (fmt) {
        case 0: dev(D_SETVS, 1, MEM32(G_LVERTEXVS), 0, 0, 0, 0); break;
        case 1: dev(D_SETVS, 1, MEM32(G_LVERTEX2VS), 0, 0, 0, 0); break;
        case 2: dev(D_SETVS, 1, 0x40, 0, 0, 0, 0); break;
        case 3: dev(D_SETVS, 1, 0x140, 0, 0, 0, 0); break;
        case 4: dev(D_SETVS, 1, 0x1C4, 0, 0, 0, 0); break;
        case 5: dev(D_SETVS, 1, 0x2C4, 0, 0, 0, 0); break;
        case 6: dev(D_SETVS, 1, MEM32(G_VERTEXVS), 0, 0, 0, 0); break;
        default: break;
        }
    }
    dev(D_SETSTREAM, 3, 0, vb, MEM32(buffer + 0x18), 0, 0);
    dev(D_SETINDICES, 2, MEM32(fg + 0x14), MEM32(l + 0x18), 0, 0, 0);
    dev(D_DIP, 5, 4, 0, nverts, MEM32(fg + 0x18), MEM32(fg + 8));
    if (reset & 1) set_tss(0, 0x0B, 0);
    if (reset & 2) set_tss(0, 0x0D, 1);
    if (reset & 4) set_tss(0, 0x0E, 1);
}

/* the check: translated then native from the same cache state, device calls recorded */
#define HLE_SHIMREC_MAX 4096
extern int hle_shimrec, hle_shimrec_n;
extern uint32_t hle_shimrec_buf[HLE_SHIMREC_MAX][8];
static const uint32_t k_caches[] = { G_ALPHAOP, G_COLOROP, G_COLOROP1, G_ALPHASTATE, G_BLENDSTATE, G_CULLSTATE, G_VTXFORMAT };
#define NCACHE (int)(sizeof k_caches / sizeof *k_caches)
static int g_rfg_check_left = 400, g_rfg_on = 1;
/* the stack words of a recorded device call: this and its arguments */
static int dev_words(uint32_t shim) {
    const char *hle_shim_name_at(int);
    const char *n = hle_shim_name_at((int)shim);
    if (!n) return 1;
    static const struct { const char *m; int w; } t[] = {
        { "::SetRenderState", 3 }, { "::SetTextureStageState", 4 }, { "::SetTexture", 3 }, { "::SetTransform", 3 },
        { "::SetVertexShader", 2 }, { "::SetStreamSource", 4 }, { "::SetIndices", 3 }, { "::DrawIndexedPrimitive", 6 } };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) { const char *p = strstr(n, t[i].m); if (p && !p[strlen(t[i].m)]) return t[i].w; }
    return 1;
}
static uint32_t g_reca[256][8], g_recb[256][8];

void fam_native_render_fgroup(void) {
    static int env = -1; if (env < 0) { const char *e = getenv("FAMILIA_RFG"); env = e ? atoi(e) : 1; if (!env) g_rfg_on = 0; }
    if (!g_rfg_on || !(MEM8(G_CAPS) & 0x80)) { g_rfg_fallback++; recomp_ls3df_10039280_orig(); return; }
    uint32_t obj = g_ecx, h = MEM32(g_esp + 4);
    if (g_check < 0) { const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_check = e && atoi(e); }
    if (g_check && g_rfg_check_left > 0) {
        g_rfg_check_left--;
        uint32_t esp0 = g_esp, before[NCACHE], after_t[NCACHE];
        for (int i = 0; i < NCACHE; i++) before[i] = MEM32(k_caches[i]);
        hle_shimrec_n = 0; hle_shimrec = 1;
        recomp_ls3df_10039280_orig();
        hle_shimrec = 0;
        int na = hle_shimrec_n < 256 ? hle_shimrec_n : 256; memcpy(g_reca, hle_shimrec_buf, sizeof(uint32_t) * 8 * (size_t)na);
        for (int i = 0; i < NCACHE; i++) { after_t[i] = MEM32(k_caches[i]); MEM32(k_caches[i]) = before[i]; }
        uint32_t esp_t = g_esp;
        g_esp = esp0; g_ecx = obj;
        hle_shimrec_n = 0; hle_shimrec = 1;
        render_fgroup(obj, h);
        hle_shimrec = 0;
        g_esp += 4 + 4;
        int nb = hle_shimrec_n < 256 ? hle_shimrec_n : 256; memcpy(g_recb, hle_shimrec_buf, sizeof(uint32_t) * 8 * (size_t)nb);
        int bad = na != nb || g_esp != esp_t;
        for (int i = 0; !bad && i < na; i++) {
            if (g_reca[i][0] != g_recb[i][0]) { bad = 1; break; }
            int words = dev_words(g_reca[i][0]);               /* this and the method's own arguments */
            for (int k = 1; k <= words; k++) if (g_reca[i][k] != g_recb[i][k]) { bad = 1; break; }
        }
        for (int i = 0; !bad && i < NCACHE; i++) bad = after_t[i] != MEM32(k_caches[i]);
        static int reported;
        if (bad) {
            fprintf(stderr, "[native] I3D_object::m_RenderFGroup MISMATCH: %d device calls translated, %d native (esp %08X/%08X)\n", na, nb, esp_t, g_esp);
            int hle_shim_count(void); const char *hle_shim_name_at(int);
            for (int i = 0; i < (na > nb ? na : nb) && i < 12; i++)
                fprintf(stderr, "[native]   %-44s %08X %08X %08X | %-44s %08X %08X %08X\n",
                        i < na ? hle_shim_name_at((int)g_reca[i][0]) : "-", i < na ? g_reca[i][2] : 0, i < na ? g_reca[i][3] : 0, i < na ? g_reca[i][4] : 0,
                        i < nb ? hle_shim_name_at((int)g_recb[i][0]) : "-", i < nb ? g_recb[i][2] : 0, i < nb ? g_recb[i][3] : 0, i < nb ? g_recb[i][4] : 0);
            g_rfg_on = 0;
        } else if (!reported++ || g_rfg_check_left == 0)
            fprintf(stderr, "[native] I3D_object::m_RenderFGroup: matches the translated original (%d device calls in this one; %d checks left)\n", na, g_rfg_check_left);
        return;
    }
    g_rfg_native++;
    prefetch_ahead(h);
    render_fgroup(obj, h);
    g_esp += 4 + 4;                                              /* ret 4 */
}

/* ===================================================================
 * id 101: I3D_driver::RenderShadowTexture's 2x2 box filter
 * (0x100164C0 .. 0x10016507): an 8-bit shadow buffer (256-byte rows)
 * halved into a 16-bit texture, each texel ((a+b+c+d) >> 6) << 12, the
 * sum in 9..10 bits as the original's add/adc pairs keep it. Exact integer
 * work: NEON adds 16 source bytes of each of the two rows pairwise, 8 texels
 * at a time. Registers are left as the x86 loop leaves them.
 * =================================================================== */
#include <arm_neon.h>
/* ===================================================================
 * id 102: I3D_projector::ProcessRecList's output loop for a perspective
 * projector (0x100475B0 .. 0x10047706, exit to 0x10047CBD): for each clipped
 * vertex, the position copied, the projection's texture coordinates (near the
 * projector, z under the engine's epsilon, a side's 0 or 1, both stored in u
 * as the original does), and the colour faded by depth, packed as the
 * original packs it. Night scenes project car and street lights onto every
 * receiver near them: this loop was about 1.5% of a frame there. The x87 is
 * followed operation for operation (double, float stores, fistp through the
 * translation's own rounding).
 *   regs at the head: ecx = output vertex (0x18 bytes), edx = clip result
 *   (12 bytes), esi = count, ebp = the projector.
 * =================================================================== */
static inline double fl(uint32_t va) { float f; memcpy(&f, (const void *)ADDR(va), 4); return f; }
static inline void sf(uint32_t va, double d) { float f = (float)d; memcpy((void *)ADDR(va), &f, 4); }
static int projector_loop(void) {
    uint32_t ecx = g_ecx, edx = g_edx, esi = g_esi, ebp = g_ebp, esp = g_esp;
    if (!esi) return 0;
    const double zero = fl(0x1009B310u), one = fl(0x1009B304u), half = fl(0x1009B5B0u), k255 = fl(0x1009B5B4u);
    const double fade = fl(ebp + 0x1E8), cr = fl(ebp + 0x1EC), cg = fl(ebp + 0x1F0), cb = fl(ebp + 0x1F4);
    uint32_t eax = 0, ebx = 0, edi = 0;
    do {
        MEM32(ecx) = MEM32(edx); ebx = MEM32(edx + 4); MEM32(ecx + 4) = ebx; MEM32(ecx + 8) = MEM32(edx + 8);
        uint32_t zb = MEM32(edx + 8);
        MEM32(esp + 0x20) = zb;
        double x = fl(edx), y = fl(edx + 4), z = fl(edx + 8);
        if ((int32_t)zb < 0x322BCC77) {
            sf(ecx + 0x10, (x > zero) ? one : zero);          /* !(x <= 0) */
            sf(ecx + 0x10, (y > zero) ? zero : one);          /* the original stores this one into u as well */
        } else {
            double t = half / z;
            sf(ecx + 0x10, t * x + half);
            sf(ecx + 0x14, half - t * y);
        }
        double f = (one - z) * fade;
        sf(esp + 0x10, f);                                   /* fst: the double stays on the stack */
        if (f > one) MEM32(esp + 0x10) = 0x3F800000u;        /* the double above 1 (NaN is not): 1 */
        else if (fl(esp + 0x10) < zero) MEM32(esp + 0x10) = 0;   /* the stored float below 0: 0 */
        double ff = fl(esp + 0x10);
        sf(esp + 0x1C, ff * cr * k255); MEM32(esp + 0x1C) = (uint32_t)(int32_t)fp_round_cw(fl(esp + 0x1C), g_fpu_cw);
        sf(esp + 0x18, ff * cg * k255); MEM32(esp + 0x18) = (uint32_t)(int32_t)fp_round_cw(fl(esp + 0x18), g_fpu_cw);
        sf(esp + 0x14, ff * cb * k255); MEM32(esp + 0x14) = (uint32_t)(int32_t)fp_round_cw(fl(esp + 0x14), g_fpu_cw);
        eax = MEM32(esp + 0x1C); edi = MEM32(esp + 0x18);
        eax = ((eax | 0xFFFFFF00u) << 8 | edi) << 8;
        edi = MEM32(esp + 0x14);
        eax |= edi;
        MEM32(ecx + 0xC) = eax;
        ecx += 0x18; edx += 0xC;
    } while (--esi);
    g_eax = eax; g_ebx = ebx; g_ecx = ecx; g_edx = edx; g_esi = 0; g_edi = edi;
    return 1;
}

/* ===================================================================
 * id 103: ClipMesh's perspective vertex loop (0x10003C90 .. 0x10003D95,
 * exit to 0x10003D99): each vertex of a projector's receiver into the
 * projection's space (S_vector * S_matrix, bit-exact fam_vmm, called here
 * directly instead of through the translated call) and its clip code. eax is
 * kept as the original leaves it, the x87 status word included (the
 * translation's fnstsw encoding: 0x0100 less, 0x4000 equal, 0 greater,
 * 0x4500 unordered).
 *   regs at the head: esi = clip vertex (0x14), edi = mesh vertex (0x20),
 *   ebp = count, ebx = 0x12; the matrix at [esp+0x20], the call's result
 *   buffer at [esp+0x14].
 * =================================================================== */
void fam_vmm(const float *V, const float *M, float *R);
static inline uint32_t sw_of(double a, double b) { return (a != a || b != b) ? 0x4500u : a < b ? 0x0100u : a == b ? 0x4000u : 0u; }
#define SW(e, a, b) ((e) = ((e) & 0xFFFF0000u) | sw_of((a), (b)))
static int clipmesh_loop(void) {
    uint32_t esi = g_esi, edi = g_edi, ebp = g_ebp, esp = g_esp, eax = g_eax, ecx = g_ecx, edx = g_edx;
    if (!ebp) return 0;
    const uint32_t res = esp + 0x14, mm = esp + 0x20;
    const double zero = fl(0x1009B310u), one = fl(0x1009B304u);
    do {
        float R[3];
        fam_vmm((const float *)ADDR(edi), (const float *)ADDR(mm), R);
        memcpy((void *)ADDR(res), R, 12);
        uint32_t b[3]; memcpy(b, R, 12);
        MEM32(esi) = b[0]; MEM32(esi + 4) = b[1]; MEM32(esi + 8) = b[2];
        ecx = esi; edx = b[1]; eax = b[2];
        double x = R[0], y = R[1], z = R[2], ax = fabs(x), ay = fabs(y);
        uint32_t c;
        SW(eax, z, zero);
        if (z < zero) {                                      /* behind the projector */
            double nz = -z;
            c = 0x10;
            SW(eax, nz, ax);
            if (nz < ax) { SW(eax, x, zero); c = x < zero ? 0x11 : 0x12; }
            MEM32(esi + 0x10) = c;
            SW(eax, nz, ay);
            if (nz < ay) { SW(eax, y, zero); eax = c | (y < zero ? 4u : 8u); MEM32(esi + 0x10) = eax; }
        } else {
            SW(eax, z, one);
            c = z > one ? 0x20 : 0;
            MEM32(esi + 0x10) = c;
            SW(eax, z, ax);
            if (z < ax) { SW(eax, x, zero); c |= x < zero ? 1u : 2u; eax = c; MEM32(esi + 0x10) = c; }
            SW(eax, z, ay);
            if (z < ay) { SW(eax, y, zero); c |= y < zero ? 4u : 8u; eax = c; MEM32(esi + 0x10) = c; }
        }
        MEM32(esi + 0xC) = 0xFFFFFFFFu;
        esi += 0x14; edi += 0x20;
    } while (--ebp);
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_esi = esi; g_edi = edi; g_ebp = 0;
    g_ebx = MEM32(esp + 0x10);                               /* 0x10003D95: mov ebx, [esp + 0x10] */
    return 1;
}

static int shadow_filter(void) {
    uint32_t ebp = g_ebp, rows = g_ecx, cols = MEM32(ebp + 0xC);
    if (!rows || !cols) return 0;                           /* the x86 loop would count down from 0 */
    uint32_t dst = g_edi, src = g_esi, dstride = MEM32(ebp - 4), sskip = MEM32(ebp + 0x1C);
    uint32_t last = 0, edx = dst;
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *s0 = (const uint8_t *)ADDR(src), *s1 = s0 + 0x100;
        uint16_t *d = (uint16_t *)ADDR(dst);
        uint32_t c = 0;
        for (; c + 8 <= cols; c += 8) {
            uint16x8_t a = vpaddlq_u8(vld1q_u8(s0 + 2 * c)), b = vpaddlq_u8(vld1q_u8(s1 + 2 * c));
            uint16x8_t v = vshlq_n_u16(vshrq_n_u16(vaddq_u16(a, b), 6), 12);
            vst1q_u16(d + c, v);
        }
        for (; c < cols; c++) {
            uint32_t sum = (uint32_t)s0[2 * c] + s0[2 * c + 1] + s1[2 * c] + s1[2 * c + 1];
            d[c] = (uint16_t)((sum >> 6) << 12);
        }
        {   /* the last texel of the row, as eax holds it */
            uint32_t k = cols - 1, sum = (uint32_t)s0[2 * k] + s0[2 * k + 1] + s1[2 * k] + s1[2 * k + 1];
            last = (sum >> 6) << 12;
        }
        edx = dst + 2 * cols;
        src += 2 * cols + sskip;
        dst += dstride;
    }
    g_eax = last; g_ebx = 0; g_ecx = 0; g_edx = edx; g_esi = src; g_edi = dst;
    return 1;
}
