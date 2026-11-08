/*
 * native_coll.c - native version of the collision grid sweep in Game.exe
 * (0x005C5040, a g_collision method: thiscall, one argument, ret 4).
 *
 * The query (argument) is a moving sphere: start point (+0x00), direction
 * (+0x0C), the swept box (+0x1C..+0x28), a type mask (+0x3C), the best hit so
 * far (+0x40 t, +0x48 normal, +0x54 object) and the sweep length (+0x44).
 * The function walks the grid cells the box covers, each cell's entries, and
 * tests each entry by its type (a jump table at 0x5C5558): spheres natively,
 * the others through the game's own routines (0x5C5B20, LS3DF vector/matrix
 * exports, a virtual callback for type 0x48). Measured as single-frame stalls
 * of 230-280 ms during car crashes, all in its own loop.
 *
 * Exactness model as in the other natives: x87 values are doubles, rounded to
 * float only where the original stores a dword; same operation order; the
 * comparisons decode the status word per site (unordered included). The
 * function's guest stack frame (0xEC + 4 saved registers) is laid out at the
 * same addresses, since callees read vectors from it.
 *
 * FAMILIA_NATIVE_CHECK=1 (or FAMILIA_COLL_CHECK=N): the first N real calls
 * are run twice - the translated original, then this - on the same snapshot
 * of the query and the frame, and compared; calls that would reach the
 * type-0x48 callback (side effects outside those regions) are left to the
 * original. A mismatch keeps the original's result and switches this off.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

int fam_native_coll_on = 1;
void recomp_game_005C5040_orig(void);

#define XPUSH(v) fp_push_impl(g_st, &g_fp_top, (v))

static inline double F(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return (double)f; }
static inline void SF(uint32_t va, double d) { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }
static inline uint32_t fbits_d(double d) { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); return u; }

static recomp_func_t guest_fn(uint32_t va) {
    recomp_func_t f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup(va);
    if (!f) f = recomp_lookup_import(va);
    if (!f) { fprintf(stderr, "[native] coll: guest %08X not found\n", va); abort(); }
    return f;
}

/* __ftol (0x6109D0) on v: truncating fistp qword, eax = low dword */
static uint32_t ftol_guest(double v) {
    static recomp_func_t f;
    if (!f) f = guest_fn(0x006109D0u);
    XPUSH(v);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    return g_eax;
}

static int g_dry, g_dry_hit48;          /* the check's pre-scan: stop before the type-0x48 callback */

/* the body; returns 0 if a dry pre-scan stopped at a type-0x48 callback */
static int sweep(void) {
    const uint32_t E = g_esp, self = g_ecx;
    const uint32_t S = E - 0xFC;                         /* esp after sub esp,0xEC and 4 pushes */
    const uint32_t q = MEM32(E + 4);
    MEM32(S + 0xC) = g_ebx; MEM32(S + 8) = g_ebp; MEM32(S + 4) = g_esi; MEM32(S) = g_edi;
    g_esp = S;
    MEM32(S + 0x1C) = self;
    MEM8(S + 0x13) = 0;
    uint32_t grid = MEM32(self + 0x28C);
    /* the cell range */
    double inv = 1.0 / F(grid + 8);
    uint32_t x0 = ftol_guest(((F(q + 0x1C) - F(grid)) * inv) - 1.0);
    MEM32(S + 0x24) = x0;
    uint32_t x1 = ftol_guest(((F(q + 0x24) - F(grid)) * inv) + 2.0);
    inv = 1.0 / F(grid + 0xC);
    MEM32(S + 0x18) = x1;
    uint32_t z0 = ftol_guest(((F(q + 0x20) - F(grid + 4)) * inv) - 1.0);
    uint32_t z1 = ftol_guest(((F(q + 0x28) - F(grid + 4)) * inv) + 2.0);
    uint32_t ecx = z1, edx;
    MEM32(S + 0x20) = z1;
    if ((int32_t)MEM32(S + 0x24) < 0) MEM32(S + 0x24) = 0;
    if ((int32_t)z0 < 0) z0 = 0;
    if (MEM32(S + 0x18) > MEM32(grid + 0x10)) MEM32(S + 0x18) = MEM32(grid + 0x10);
    if (ecx > MEM32(grid + 0x14)) MEM32(S + 0x20) = MEM32(grid + 0x14);
    edx = z0;
    MEM32(S + 0x2C) = edx;
    if ((int32_t)z0 < (int32_t)MEM32(S + 0x20)) {
        for (;;) {                                       /* z rows */
            int32_t xe = (int32_t)MEM32(S + 0x18);
            ecx = MEM32(S + 0x24);
            MEM32(S + 0x28) = ecx;
            if ((int32_t)ecx < xe) {
                for (;;) {                               /* x cells */
                    uint32_t g = MEM32(self + 0x28C);
                    uint32_t cell = MEM32(MEM32(g + 0x18) + 4 * (MEM32(g + 0x10) * edx + ecx));
                    MEM32(S + 0x48) = cell;
                    if (cell && MEM32(cell + 0xC) < MEM32(cell + 0x10)) {
                        for (uint32_t it = MEM32(cell + 0xC); it < MEM32(MEM32(S + 0x48) + 0x10); it += 4) {
                            uint32_t e = MEM32(it);
                            if (MEM32(q + 0x3C) & MEM32(e + 8)) continue;
                            {   /* the exclusion list */
                                uint32_t n = MEM32(self + 0x2BC), lst = MEM32(self + 0x2B8), skip = 0;
                                for (; n; n--) if (MEM32(lst + 4 * (n - 1)) == e) { skip = 1; break; }
                                if (skip) continue;
                            }
                            /* the entry's distance to the motion line, or -1e16 for no motion */
                            double q0c = F(q + 0xC), q10 = F(q + 0x10), q14 = F(q + 0x14);
                            double lsq = (q0c * q0c + q10 * q10) + q14 * q14;
                            SF(S + 0x14, lsq);
                            double d;
                            if (lsq < F(0x62323Cu)) d = F(0x623778u);
                            else {
                                uint32_t c = e + 0x10;
                                double vz = (float)(F(c + 8) - F(q + 8)), vy = (float)(F(c + 4) - F(q + 4)), vx = (float)(F(c) - F(q));
                                SF(S + 0x4C, vx); SF(S + 0x50, vy); SF(S + 0x54, vz);      /* S_vector(x,y,z) at S+0x4C */
                                double dot = (F(S + 0x4C) * q0c + F(S + 0x54) * q14) + F(S + 0x50) * q10;
                                double t = dot / F(S + 0x14);
                                double pz = (float)(t * q14), py = (float)(t * q10), px = (float)(t * q0c);
                                SF(S + 0x70, px); SF(S + 0x74, py); SF(S + 0x78, pz);      /* S_vector at S+0x70 */
                                SF(S + 0xF0, F(S + 0x70) + F(q));                           /* operator+ -> S+0xF0 */
                                SF(S + 0xF4, F(S + 0x74) + F(q + 4));
                                SF(S + 0xF8, F(S + 0x78) + F(q + 8));
                                SF(S + 0xD8, F(S + 0xF0) - F(c));                           /* operator- -> S+0xD8 */
                                SF(S + 0xDC, F(S + 0xF4) - F(c + 4));
                                SF(S + 0xE0, F(S + 0xF8) - F(c + 8));
                                double mx = F(S + 0xD8), my = F(S + 0xDC), mz = F(S + 0xE0);  /* Magnitude */
                                double s = (mx * mx + my * my) + mz * mz;
                                uint32_t sb = fbits_d(s);
                                if ((int32_t)sb < 0x322BCC77) d = F(0x623230u);
                                else {
                                    double sf = (float)s;
                                    double eps; memcpy(&eps, (const void *)ADDR(0x6232E8u), 8);
                                    if (fabs(sf - F(0x623238u)) < eps) d = F(0x623238u);
                                    else d = sqrt(sf);
                                }
                            }
                            /* within the entry's radius, and its box against the swept box */
                            double r = F(e + 0x1C);
                            if (d > r) continue;
                            if (r + F(e + 0x10) < F(q + 0x1C)) continue;
                            if (F(e + 0x10) - r > F(q + 0x24)) continue;
                            if (r + F(e + 0x18) < F(q + 0x20)) continue;
                            if (F(e + 0x18) - r > F(q + 0x28)) continue;
                            uint32_t type = (MEM32(e) & 0xFF) - 0x40;
                            if (type > 8) continue;
                            switch (type) {
                            case 8: {                    /* a callback object decides */
                                uint32_t cb = MEM32(self + 0x2C4);
                                if (!cb) break;
                                if (g_dry) { g_dry_hit48 = 1; g_esp = E; return 0; }
                                PUSH32(g_esp, e);
                                PUSH32(g_esp, RECOMP_RETADDR);
                                g_ecx = cb;
                                guest_fn(MEM32(MEM32(cb) + 0x90))();
                                break;
                            }
                            case 2: case 4: {            /* a sphere: the first time the moving sphere touches it */
                                double a = F(e + 0x10) - F(q), b = F(e + 0x14) - F(q + 4), c = F(e + 0x18) - F(q + 8);
                                double dotv = (c * q14 + b * q10) + a * q0c;
                                SF(S + 0x14, dotv);
                                double df = F(S + 0x14), rr = F(e + 0x1C), q44 = F(q + 0x44);
                                double L2 = (c * c + b * b) + a * a;
                                double disc = df * df - (L2 - rr * rr) * q44;
                                if (disc < 0.0) break;
                                double t = (df - sqrt(disc)) / q44;
                                if (t < 0.0) break;
                                if (!(t < F(q + 0x40)) && !isnan(t) && !isnan(F(q + 0x40))) break;
                                double tq0c = t * q0c, tq10 = t * q10, tq14 = t * q14;
                                MEM8(S + 0x13) = 1;
                                SF(S + 0x60, tq14);
                                SF(S + 0x64, tq0c + F(q));
                                double py = tq10 + F(q + 4);
                                double pz = F(S + 0x60) + F(q + 8);
                                SF(S + 0x30, F(e + 0x10) - F(S + 0x64));
                                SF(S + 0x34, F(e + 0x14) - py);
                                SF(S + 0x38, F(e + 0x18) - pz);
                                MEM32(q + 0x48) = MEM32(S + 0x30); MEM32(q + 0x4C) = MEM32(S + 0x34);
                                SF(q + 0x40, t);
                                MEM32(q + 0x50) = MEM32(S + 0x38);
                                MEM32(q + 0x54) = e;
                                break;
                            }
                            case 1: {                    /* a face (0x5C5B20 tests and fills the query) */
                                PUSH32(g_esp, q); PUSH32(g_esp, e + 0x30); PUSH32(g_esp, e + 0x24);
                                PUSH32(g_esp, RECOMP_RETADDR);
                                g_ecx = self;
                                guest_fn(0x005C5B20u)();
                                if (g_eax & 0xFF) { MEM8(S + 0x13) = 1; MEM32(q + 0x54) = MEM32(it); }
                                break;
                            }
                            case 0: {                    /* a face in the entry's own space: query moved there */
                                memcpy((void *)ADDR(S + 0x7C), (const void *)ADDR(q), 0x17 * 4);
                                PUSH32(g_esp, MEM32(e + 0x40)); PUSH32(g_esp, S + 0x7C); PUSH32(g_esp, RECOMP_RETADDR);
                                guest_fn(0x0060FC54u)();                         /* S_vector::operator*=(S_matrix) */
                                PUSH32(g_esp, MEM32(MEM32(it) + 0x40)); PUSH32(g_esp, S + 0xA8); PUSH32(g_esp, RECOMP_RETADDR);
                                guest_fn(0x0060FC54u)();
                                SF(S + 0x3C, F(S + 0xA8) - F(S + 0x7C));
                                SF(S + 0x40, F(S + 0xAC) - F(S + 0x80));
                                MEM32(S + 0x88) = MEM32(S + 0x3C);
                                SF(S + 0x44, F(S + 0xB0) - F(S + 0x84));
                                MEM32(S + 0x8C) = MEM32(S + 0x40);
                                MEM32(S + 0x90) = MEM32(S + 0x44);
                                uint32_t ent = MEM32(it);
                                PUSH32(g_esp, S + 0x7C); PUSH32(g_esp, ent + 0x30); PUSH32(g_esp, ent + 0x24);
                                PUSH32(g_esp, RECOMP_RETADDR);
                                g_ecx = MEM32(S + 0x1C);
                                guest_fn(0x005C5B20u)();
                                if (g_eax & 0xFF) {
                                    MEM32(q + 0x40) = MEM32(S + 0xBC);
                                    PUSH32(g_esp, MEM32(MEM32(it) + 0x3C)); PUSH32(g_esp, S + 0xE4); PUSH32(g_esp, S + 0xC4);
                                    PUSH32(g_esp, RECOMP_RETADDR);
                                    guest_fn(0x0060FC66u)();                     /* S_vector::RotateByMatrix -> eax */
                                    uint32_t rv = g_eax;
                                    MEM8(S + 0x13) = 1;
                                    MEM32(q + 0x48) = MEM32(rv); MEM32(q + 0x4C) = MEM32(rv + 4); MEM32(q + 0x50) = MEM32(rv + 8);
                                    MEM32(q + 0x54) = MEM32(it);
                                }
                                break;
                            }
                            default: break;              /* 0x43, 0x45-0x47: nothing */
                            }
                            g_esp = S;
                        }
                    }
                    ecx = MEM32(S + 0x28) + 1;
                    MEM32(S + 0x28) = ecx;
                    if ((int32_t)ecx >= (int32_t)MEM32(S + 0x18)) break;
                }
            }
            edx = MEM32(S + 0x2C) + 1;
            MEM32(S + 0x2C) = edx;
            if ((int32_t)edx >= (int32_t)MEM32(S + 0x20)) break;
        }
    }
    g_eax = (MEM32(S + 0x20) & 0xFFFFFF00u) | MEM8(S + 0x13);
    g_ecx = ecx; g_edx = edx;
    g_esp = E + 8;                                       /* ret 4 */
    return 1;
}

/* ---------------------------------------------------------- live check */
static int g_check_left = -1;
#define FRAME_LO 0x110u                                  /* below the entry esp: frame + callee slots */
typedef struct { uint32_t eax, esp, ebx, esi, edi, ebp; int stb; uint8_t mq[0x60], mf[FRAME_LO + 8]; } coll_state;

static void capture(coll_state *s, uint32_t q, uint32_t E) {
    s->eax = g_eax; s->esp = g_esp; s->ebx = g_ebx; s->esi = g_esi; s->edi = g_edi; s->ebp = g_ebp; s->stb = g_st_base;
    memcpy(s->mq, (const void *)ADDR(q), sizeof s->mq);
    memcpy(s->mf, (const void *)ADDR(E - FRAME_LO), sizeof s->mf);
}
static void put_back(const coll_state *s, uint32_t q, uint32_t E) {
    g_eax = s->eax; g_esp = s->esp; g_ebx = s->ebx; g_esi = s->esi; g_edi = s->edi; g_ebp = s->ebp; g_st_base = s->stb;
    memcpy((void *)ADDR(q), s->mq, sizeof s->mq);
    memcpy((void *)ADDR(E - FRAME_LO), s->mf, sizeof s->mf);
}

static void checked_call(void) {
    uint32_t E = g_esp, q = MEM32(E + 4), self = g_ecx;
    uint32_t in_ecx = g_ecx, in_edx = g_edx;
    double st_save[8]; memcpy(st_save, g_st, sizeof st_save);
    coll_state in, orig, nat;
    capture(&in, q, E);
    /* pre-scan: would the native path reach the callback with side effects? */
    g_dry = 1; g_dry_hit48 = 0;
    sweep();
    g_dry = 0;
    put_back(&in, q, E); g_ecx = in_ecx; g_edx = in_edx; memcpy(g_st, st_save, sizeof st_save);
    if (g_dry_hit48) { recomp_game_005C5040_orig(); return; }
    recomp_game_005C5040_orig();
    capture(&orig, q, E);
    put_back(&in, q, E); g_ecx = in_ecx; g_edx = in_edx; memcpy(g_st, st_save, sizeof st_save);
    sweep();
    capture(&nat, q, E);
    /* the frame below the function's own (callees' frames) is not compared */
    uint32_t own = FRAME_LO - 0xFC;
    int bad = orig.eax != nat.eax || orig.esp != nat.esp || orig.ebx != nat.ebx || orig.esi != nat.esi ||
              orig.edi != nat.edi || orig.ebp != nat.ebp || orig.stb != nat.stb ||
              memcmp(orig.mq, nat.mq, sizeof orig.mq) || memcmp(orig.mf + own, nat.mf + own, sizeof orig.mf - own);
    if (bad) {
        fprintf(stderr, "[native] collision sweep 0x5C5040 MISMATCH (this %08X query %08X): eax %08X/%08X esp %08X/%08X st %d/%d\n",
                self, q, orig.eax, nat.eax, orig.esp, nat.esp, orig.stb, nat.stb);
        int shown = 0;
        for (uint32_t o = 0; o < sizeof orig.mq && shown < 3; o += 4)
            if (memcmp(orig.mq + o, nat.mq + o, 4)) { uint32_t a, b; memcpy(&a, orig.mq + o, 4); memcpy(&b, nat.mq + o, 4);
                fprintf(stderr, "    query+%02X: %08X / %08X\n", o, a, b); shown++; }
        for (uint32_t o = own; o + 4 <= sizeof orig.mf && shown < 3; o += 4)
            if (memcmp(orig.mf + o, nat.mf + o, 4)) { uint32_t a, b; memcpy(&a, orig.mf + o, 4); memcpy(&b, nat.mf + o, 4);
                fprintf(stderr, "    frame esp+%03X: %08X / %08X\n", o - own, a, b); shown++; }
        put_back(&orig, q, E);                           /* keep the original's result */
        fam_native_coll_on = 0;
        g_check_left = 0;
        fprintf(stderr, "[native] collision sweep: native version disabled\n");
        return;
    }
    if (--g_check_left == 0) fprintf(stderr, "[native] collision sweep 0x5C5040: live check passed\n");
}

/* bool g_collision::<sweep>(query *)  -- Game.exe 0x005C5040, thiscall, ret 4 */
void fam_native_coll_sweep(void) {
    if (!fam_native_coll_on) { recomp_game_005C5040_orig(); return; }
    if (g_check_left > 0) { checked_call(); return; }
    sweep();
}

int fam_native_coll_check(void) {
    const char *e = getenv("FAMILIA_COLL_CHECK");
    int n = e ? atoi(e) : 300;
    if (n > 0) {
        g_check_left = n;
        fprintf(stderr, "[native] collision sweep 0x5C5040: checking the first %d calls live against the translated original\n", n);
    }
    return 0;
}
