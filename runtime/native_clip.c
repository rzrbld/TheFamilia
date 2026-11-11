/*
 * native_clip.c - native ClipFacesPersp (LS3DF 0x10002530): the faces of a
 * projector's receivers clipped to its perspective volume, tessellated by
 * depth, triangulated, and the new vertices lifted along the face's normal and
 * spread from the polygon's centre. Night scenes run it for every car and
 * street light that projects onto the road (about 2% of a night frame
 * translated).
 *
 * Structure from the mafia-re decompilation (render/clipper2.cpp, a map
 * only); the arithmetic from the x86 of LS3DF.dll 1.2, operation for operation:
 * x87 values are doubles, rounded to float exactly where the original stores a
 * float (struct fields, but also the spilled locals: the back-face normal's x
 * and y, the tessellation step, the centre's x sum, the lift and spread
 * factors), and kept double where the original keeps them on the x87 stack.
 * Built with -ffp-contract=off.
 *
 * fastcall: ecx = faces (3 ushorts each), edx = count; returns al = 1.
 *
 * FAMILIA_NATIVE_CHECK=1: the first calls run the translated original first,
 * keep everything it wrote (vertices, faces, polygon buffers, globals,
 * registers), put the state back, run this, and compare; a difference prints
 * where and switches this function back to the translated one.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

void recomp_ls3df_10002530_orig(void);
uint32_t hle_heap_block_end(uint32_t va);

/* globals (LS3DF data) */
#define CV_COUNT    0x100AF7C8u   /* clip_vert_count */
#define CV_END      0x100AEF44u   /* clip_vert_end */
#define POLY_NUM    0x100AF390u   /* clip_poly_num */
#define VERTS       0x100AF3A8u   /* clip_verts */
#define BUF_A       0x100AF3C8u   /* the first polygon buffer (512 ushorts) */
#define BUF_B       0x100AEF88u   /* the second */
#define FG_BLEND    0x100AEF4Cu
#define NEXT_FACE_T 0x100AF3B0u
#define NEXT_FACE   0x100AEF54u
#define TWO_SIDED   0x100AF3C0u
#define REV_FACTOR  0x100AEF7Cu
#define RATIO_NX    0x100AF3A0u
#define RATIO_NY    0x100AF39Cu
#define INV_REV     0x100AEF78u
#define TESS        0x100AF928u   /* RsNonOrthoTessellation */
#define LIFT        0x100AF394u
#define SPREAD      0x100AEF50u
#define VERTS_MAX   0x100AEF38u
#define ADDED_T     0x100AEF40u
#define ADDED       0x100AF3B8u
#define K_ZERO 0x1009B310u
#define K_ONE  0x1009B304u
#define K_MONE 0x1009B314u
#define K_EPS  0x1009B308u
#define K_TEN  0x1009B318u

int fam_native_clip_on = 1;

static inline float lf(uint32_t va) { float f; memcpy(&f, (const void *)ADDR(va), 4); return f; }
static inline void sf(uint32_t va, double d) { float f = (float)d; memcpy((void *)ADDR(va), &f, 4); }
static inline uint16_t rw(uint32_t va) { uint16_t v; memcpy(&v, (const void *)ADDR(va), 2); return v; }
static inline void ww(uint32_t va, uint16_t v) { memcpy((void *)ADDR(va), &v, 2); }

/* a clip vertex: pos x,y,z at +0, +4, +8; index +0xC; clip +0x10 (0x14 bytes) */
static uint32_t g_verts;
static inline uint32_t V(uint32_t i) { return g_verts + i * 0x14u; }

/* a new vertex at the end; returns its address */
static inline uint32_t new_vert(uint32_t *out) {
    uint32_t cnt = MEM32(CV_COUNT);
    ww(*out, (uint16_t)cnt); *out += 2;
    return MEM32(CV_END);
}
static inline void end_vert(uint32_t e) {
    MEM32(e + 0xC) = 0xFFFFFFFFu;
    MEM32(CV_END) = e + 0x14;
    MEM32(CV_COUNT) = MEM32(CV_COUNT) + 1;
}

/* near/far planes: the new vertex's clip code from its x, y */
static void code_near(uint32_t e) {          /* z = 0: against 0 */
    double zero = lf(K_ZERO), x = lf(e), y;
    uint32_t c = x < zero ? 1u : x > zero ? 2u : 0u;
    MEM32(e + 0x10) = c;
    y = lf(e + 4);
    if (y < zero) MEM32(e + 0x10) = c | 4u;
    else if (y > zero) MEM32(e + 0x10) = c | 8u;
}
static void code_far(uint32_t e) {           /* z = 1: against -1 and 1 */
    double one = lf(K_ONE), mone = lf(K_MONE), x = lf(e), y;
    uint32_t c = x < mone ? 1u : x > one ? 2u : 0u;
    MEM32(e + 0x10) = c;
    y = lf(e + 4);
    if (y < mone) MEM32(e + 0x10) = c | 4u;
    else if (y > one) MEM32(e + 0x10) = c | 8u;
}
static void code_lr(uint32_t e) {            /* |y| > z: bottom or top */
    double ay = fabs((double)lf(e + 4)), z = lf(e + 8), zero = lf(K_ZERO);
    MEM32(e + 0x10) = z < ay ? (lf(e + 4) < zero ? 4u : 8u) : 0u;
}

static void v_near(uint32_t *out, uint32_t a, uint32_t b) {
    uint32_t e = new_vert(out);
    MEM32(e + 8) = 0;
    double t = -(double)lf(a + 8) / ((double)lf(b + 8) - lf(a + 8));
    sf(e, ((double)lf(b) - lf(a)) * t + lf(a));
    sf(e + 4, ((double)lf(b + 4) - lf(a + 4)) * t + lf(a + 4));
    code_near(e);
    end_vert(e);
}
static void v_far(uint32_t *out, uint32_t a, uint32_t b) {
    uint32_t e = new_vert(out);
    MEM32(e + 8) = 0x3F800000u;
    double t = ((double)lf(K_ONE) - lf(a + 8)) / ((double)lf(b + 8) - lf(a + 8));
    sf(e, ((double)lf(b) - lf(a)) * t + lf(a));
    sf(e + 4, ((double)lf(b + 4) - lf(a + 4)) * t + lf(a + 4));
    code_far(e);
    end_vert(e);
}
static void v_left(uint32_t *out, uint32_t a, uint32_t b) {
    uint32_t e = new_vert(out);
    double s = (double)lf(a + 8) + lf(a);
    double t = s / ((s - lf(b)) - lf(b + 8));
    sf(e, ((double)lf(b) - lf(a)) * t + lf(a));
    sf(e + 4, ((double)lf(b + 4) - lf(a + 4)) * t + lf(a + 4));
    sf(e + 8, -(double)lf(e));
    code_lr(e);
    end_vert(e);
}
static void v_right(uint32_t *out, uint32_t a, uint32_t b) {
    uint32_t e = new_vert(out);
    double d = (double)lf(b) - lf(a);
    double t = ((double)lf(a + 8) - lf(a)) / ((d - lf(b + 8)) + lf(a + 8));
    sf(e, d * t + lf(a));
    sf(e + 4, ((double)lf(b + 4) - lf(a + 4)) * t + lf(a + 4));
    MEM32(e + 8) = MEM32(e);
    code_lr(e);
    end_vert(e);
}
static void v_bottom(uint32_t *out, uint32_t a, uint32_t b) {     /* no clip code written */
    uint32_t e = new_vert(out);
    double s = (double)lf(a + 8) + lf(a + 4);
    double t = s / ((s - lf(b + 4)) - lf(b + 8));
    sf(e, ((double)lf(b) - lf(a)) * t + lf(a));
    sf(e + 4, ((double)lf(b + 4) - lf(a + 4)) * t + lf(a + 4));
    sf(e + 8, -(double)lf(e + 4));
    end_vert(e);
}
static void v_top(uint32_t *out, uint32_t a, uint32_t b) {
    uint32_t e = new_vert(out);
    double t = ((double)lf(a + 8) - lf(a + 4)) / ((((double)lf(b + 4) - lf(a + 4)) - lf(b + 8)) + lf(a + 8));
    sf(e, ((double)lf(b) - lf(a)) * t + lf(a));
    sf(e + 4, ((double)lf(b + 4) - lf(a + 4)) * t + lf(a + 4));
    MEM32(e + 8) = MEM32(e + 4);
    end_vert(e);
}

/* one clipping stage over the polygon in `in`, into `out`; lo/hi: the two
   planes' clip bits, mk_lo/mk_hi their vertex makers */
typedef void (*mk_fn)(uint32_t *, uint32_t, uint32_t);
static void stage(uint32_t *in, uint32_t *out, uint32_t lo_bit, uint32_t hi_bit, mk_fn mk_lo, mk_fn mk_hi) {
    uint32_t num = MEM32(POLY_NUM);
    while (num) {
        num--; MEM32(POLY_NUM) = num;
        uint32_t a = V(rw(*in)), b = V(rw(*in + 2));
        uint32_t ca = MEM32(a + 0x10), cb = MEM32(b + 0x10);
        if (ca & lo_bit) {
            if (!(cb & lo_bit)) {
                if (cb & hi_bit) { mk_lo(out, a, b); mk_hi(out, a, b); }
                else mk_lo(out, a, b);
            }
        } else if (ca & hi_bit) {
            if (cb & lo_bit) { mk_hi(out, a, b); mk_lo(out, a, b); }
            else if (!(cb & hi_bit)) mk_hi(out, a, b);
        } else {
            ww(*out, rw(*in)); *out += 2;
            if (cb & lo_bit) mk_lo(out, a, b);
            else if (cb & hi_bit) mk_hi(out, a, b);
        }
        *in += 2;
    }
}
/* close the polygon in out, make it the input; recompute the clip bits */
static uint32_t close_poly(uint32_t *in, uint32_t *out, int *swap, int with_clip) {
    uint32_t base;
    if (*swap) { base = BUF_A; *swap = 0; }
    else { base = BUF_B; *swap = 1; }
    ww(*out, rw(base));
    uint32_t num = (uint32_t)((int32_t)(*out - base) >> 1);
    MEM32(POLY_NUM) = num;
    *in = base; *out = base == BUF_A ? BUF_B : BUF_A;
    if (!with_clip) return 0;
    if ((int32_t)num <= 0) return 0;
    uint32_t c = rw(V(rw(base)) + 0x10);
    for (uint32_t i = 1; i < num; i++) c |= rw(V(rw(base + 2 * i)) + 0x10);
    return c;
}

static int32_t ftol32(double v) {          /* _ftol: truncation, the 64-bit result's low dword */
    double t = trunc(v);
    if (!(t >= -9223372036854775808.0 && t < 9223372036854775808.0)) return 0;
    return (int32_t)(int64_t)t;
}

void fam_native_clip_faces_persp(void) {
    uint32_t faces = g_ecx, count = g_edx;
    const uint32_t E = g_esp;                   /* return address at [E] */
    const uint32_t min_i_slot = E - 0x58;       /* the original's local min_i ([esp+0xb8]) */
    g_verts = MEM32(VERTS);
    const uint32_t first_vert = MEM32(CV_COUNT);
    const int blend = MEM32(FG_BLEND) != 0;
    uint32_t fo = blend ? MEM32(NEXT_FACE_T) : MEM32(NEXT_FACE);
    const double zero = lf(K_ZERO), one = lf(K_ONE), eps = lf(K_EPS);
    while (count) {
        count--;
        uint16_t i0 = rw(faces), i1 = rw(faces + 2), i2 = rw(faces + 4);
        uint32_t v0 = V(i0), v1 = V(i1), v2 = V(i2);
        if (MEM32(v0 + 0x10) & MEM32(v1 + 0x10) & MEM32(v2 + 0x10)) goto next_face;
        if (!MEM32(TWO_SIDED)) {
            /* back faces: the normal (v2 - v1) x (v0 - v1), as the original rounds it */
            double ex = (double)lf(v2) - lf(v1), ey = (double)lf(v2 + 4) - lf(v1 + 4), ez = (double)lf(v2 + 8) - lf(v1 + 8);
            float fx = (float)((double)lf(v0) - lf(v1)), fy = (float)((double)lf(v0 + 4) - lf(v1 + 4));
            double fz = (double)lf(v0 + 8) - lf(v1 + 8);
            float nx = (float)(fz * ey - (double)fy * ez);
            float ny = (float)((double)fx * ez - ex * fz);
            double nzd = ex * fy - (double)fx * ey;
            float nz = (float)nzd;
            if (nzd > zero) {
                if ((int32_t)MEM32(REV_FACTOR) < 0x322BCC77) goto next_face;
                double a = (double)lf(RATIO_NX) * nx, b = (double)lf(RATIO_NY) * ny;
                double c = (double)lf(INV_REV) * nz * nz;
                double sum = (b * b + a * a) + (double)nz * nz;
                if (sum < c) goto next_face;
            }
        }
        {
            uint32_t clip = (uint32_t)(rw(v0 + 0x10) | rw(v1 + 0x10) | rw(v2 + 0x10));
            ww(BUF_A, i0); ww(BUF_A + 6, i0); ww(BUF_A + 2, i1); ww(BUF_A + 4, i2);
            uint32_t in = BUF_A, out = BUF_B;
            int swap = 0;
            MEM32(POLY_NUM) = 3;
            if (clip & 0x30) {
                stage(&in, &out, 0x10, 0x20, v_near, v_far);
                clip = close_poly(&in, &out, &swap, 1);
            }
            if (clip & 0x03) {
                if (MEM32(POLY_NUM)) stage(&in, &out, 0x01, 0x02, v_left, v_right);
                clip = close_poly(&in, &out, &swap, 1);
            }
            if (clip & 0x0C) {
                if (MEM32(POLY_NUM)) stage(&in, &out, 0x04, 0x08, v_bottom, v_top);
                close_poly(&in, &out, &swap, 0);
            }
            uint32_t pn = MEM32(POLY_NUM);
            if ((int32_t)pn > 2) {
                /* depth tessellation: edges spanning more than TESS split; the nearest vertex found */
                double min_z = lf(K_TEN), T = lf(TESS);
                uint32_t num = 0;
                while (pn) {
                    pn--; MEM32(POLY_NUM) = pn;
                    uint16_t ia = rw(in);
                    uint32_t a = V(ia);
                    if (min_z > (double)lf(a + 8)) { min_z = lf(a + 8); MEM32(min_i_slot) = num; }
                    ww(out, ia); out += 2;
                    num++;
                    uint32_t b = V(rw(in + 2));
                    double dz = fabs((double)lf(a + 8) - lf(b + 8));
                    if (dz > T) {
                        int32_t k = ftol32(dz / T);
                        num += (uint32_t)k;
                        double dx = (double)lf(b) - lf(a), dy = (double)lf(b + 4) - lf(a + 4);
                        float dzf = (float)((double)lf(b + 8) - lf(a + 8));
                        float inv = (float)(1.0 * (double)lf(K_ONE) / dz);
                        float sx1 = (float)(dx * inv);
                        float stx = (float)((double)sx1 * T);
                        float sty = (float)(dy * inv * T);
                        float stz = (float)((double)dzf * inv * T);
                        float px, py, pz;
                        if ((double)lf(a + 8) < (double)lf(b + 8)) { px = lf(a); py = lf(a + 4); pz = lf(a + 8); }
                        else {
                            double kd = (double)k;
                            double sx = (double)stx * kd;
                            float sy = (float)((double)sty * kd), sz = (float)((double)stz * kd);
                            px = (float)((double)lf(b) - sx);
                            py = (float)((double)lf(b + 4) - sy);
                            pz = (float)((double)lf(b + 8) - sz);
                        }
                        for (int32_t j = k; j; j--) {
                            px = (float)((double)stx + px); py = (float)((double)sty + py); pz = (float)((double)stz + pz);
                            uint32_t e = new_vert(&out);
                            memcpy((void *)ADDR(e), &px, 4); memcpy((void *)ADDR(e + 4), &py, 4); memcpy((void *)ADDR(e + 8), &pz, 4);
                            end_vert(e);
                        }
                    }
                    in += 2;
                }
                MEM32(POLY_NUM) = num;
                if (swap) { in = BUF_A; out = BUF_B; } else { in = BUF_B; out = BUF_A; }
                /* triangulated from the nearest vertex */
                if (num == 3) {
                    ww(fo, rw(in)); ww(fo + 2, rw(in + 2)); ww(fo + 4, rw(in + 4)); fo += 6;
                } else {
                    int32_t min_i = (int32_t)MEM32(min_i_slot), first = min_i + 1;
                    if (first < (int32_t)num) {
                        memmove((void *)ADDR(out), (const void *)ADDR(in + 2 * (uint32_t)first), 2u * (num - (uint32_t)first));
                        if (min_i) memmove((void *)ADDR(out + 2 * (num - (uint32_t)first)), (const void *)ADDR(in), 2u * (uint32_t)min_i);
                    } else
                        memmove((void *)ADDR(out), (const void *)ADDR(in), 2u * (uint32_t)min_i);
                    int32_t hi = (int32_t)num - 2, lo = 0, i = 1, j = (int32_t)num - 3;
                    ww(fo, rw(out + 2 * (uint32_t)hi)); ww(fo + 2, rw(in + 2 * (uint32_t)min_i)); ww(fo + 4, rw(out)); fo += 6;
                    while (j > i) {
                        if ((double)lf(V(rw(out + 2 * (uint32_t)j)) + 8) < (double)lf(V(rw(out + 2 * (uint32_t)i)) + 8)) {
                            ww(fo, rw(out + 2 * (uint32_t)hi)); ww(fo + 2, rw(out + 2 * (uint32_t)lo)); ww(fo + 4, rw(out + 2 * (uint32_t)j)); fo += 6;
                            hi = j; j--;
                        } else {
                            ww(fo, rw(out + 2 * (uint32_t)hi)); ww(fo + 2, rw(out + 2 * (uint32_t)lo)); ww(fo + 4, rw(out + 2 * (uint32_t)i)); fo += 6;
                            lo = i; i++;
                        }
                    }
                    ww(fo, rw(out + 2 * (uint32_t)hi)); ww(fo + 2, rw(out + 2 * (uint32_t)lo)); ww(fo + 4, rw(out + 2 * (uint32_t)j)); fo += 6;
                }
                /* the lift along the face's normal (v2 - v1) x (v0 - v1) */
                float ex = (float)((double)lf(v2) - lf(v1)), ey = (float)((double)lf(v2 + 4) - lf(v1 + 4));
                double ez = (double)lf(v2 + 8) - lf(v1 + 8);
                float fx = (float)((double)lf(v0) - lf(v1)), fy = (float)((double)lf(v0 + 4) - lf(v1 + 4));
                double fz = (double)lf(v0 + 8) - lf(v1 + 8);
                float nx = (float)((double)ey * fz - (double)fy * ez);
                double nyd = ez * fx - (double)ex * fz;
                float nz = (float)((double)ex * fy - (double)fx * ey);
                double sq = ((double)nz * nz + nyd * nyd) + (double)nx * nx;
                double L = lf(LIFT);
                float ox, oy, oz;
                if (sq < eps) {
                    if (nx != 0.0f) {                          /* NaN counts as not 0, and then as positive */
                        double s = nx < zero ? lf(K_MONE) : one;
                        ox = (float)(s * L); oy = (float)(nyd * L); oz = (float)((double)nz * L);
                    } else if (nz != 0.0f) {
                        double s = nz < zero ? lf(K_MONE) : one;
                        ox = (float)((double)nx * L); oy = (float)(nyd * L); oz = (float)(s * L);
                    } else {
                        double s = nyd < zero ? lf(K_MONE) : one;
                        ox = (float)((double)nx * L); oy = (float)(s * L); oz = (float)((double)nz * L);
                    }
                } else {
                    double f = L / sqrt(sq);
                    float ff = (float)f;
                    ox = (float)(f * nx); oy = (float)((double)ff * nyd); oz = (float)((double)ff * nz);
                }
                /* the polygon's centre */
                double cxs = lf(V(rw(in))), cys = lf(V(rw(in)) + 4), czs = lf(V(rw(in)) + 8);
                for (uint32_t k = 1; k < num; k++) {
                    uint32_t w = V(rw(in + 2 * k));
                    cxs += lf(w); cys += lf(w + 4); czs += lf(w + 8);
                }
                double invn = (double)lf(K_ONE) / (double)(int32_t)num;
                float cx = (float)((double)(float)cxs * invn), cy = (float)(cys * invn), cz = (float)(czs * invn);
                double S = lf(SPREAD);
                for (uint32_t k = 0; k < num; k++) {
                    uint32_t vi = rw(in + 2 * k);
                    if ((int32_t)vi < (int32_t)first_vert) continue;
                    uint32_t w = V(vi);
                    sf(w, (double)ox + lf(w)); sf(w + 4, (double)oy + lf(w + 4)); sf(w + 8, (double)oz + lf(w + 8));
                    if (S > zero) {
                        float dx = (float)((double)lf(w) - cx);
                        double dyd = (double)lf(w + 4) - cy;
                        float dz = (float)((double)lf(w + 8) - cz);
                        double q = ((double)dz * dz + dyd * dyd) + (double)dx * dx;
                        float X; double Y, Z;
                        if (q < eps) {
                            if (dx != 0.0f) { double s = dx < zero ? lf(K_MONE) : one; X = (float)(s * S); Y = dyd * S; Z = (double)dz * S; }
                            else if (dz != 0.0f) { X = (float)((double)dx * S); Y = dyd * S; Z = (double)dz * S; }   /* the original sets n.z here */
                            else { double s = dyd < zero ? lf(K_MONE) : one; X = (float)((double)dx * S); Y = s * S; Z = (double)dz * S; }
                        } else {
                            double g = S / sqrt(q);
                            float gf = (float)g;
                            X = (float)(g * dx); Y = dyd * gf; Z = (double)gf * dz;
                        }
                        sf(w, (double)X + lf(w)); sf(w + 4, Y + lf(w + 4)); sf(w + 8, Z + lf(w + 8));
                    }
                }
            }
            if ((int32_t)MEM32(CV_COUNT) >= (int32_t)MEM32(VERTS_MAX)) break;
        }
next_face:
        faces += 6;
    }
    /* the faces added */
    uint32_t start = blend ? MEM32(NEXT_FACE_T) : MEM32(NEXT_FACE);
    int32_t bytes = (int32_t)(fo - start);
    int32_t hi32 = (int32_t)(((int64_t)bytes * 0x2AAAAAABll) >> 32);
    uint32_t sgn = (uint32_t)hi32 >> 31;
    int32_t nf = hi32 + (int32_t)sgn;
    uint32_t sum;
    if (blend) { sum = MEM32(ADDED_T) + (uint32_t)nf; MEM32(NEXT_FACE_T) = fo; MEM32(ADDED_T) = sum; g_ecx = sgn; }
    else { sum = MEM32(ADDED) + (uint32_t)nf; MEM32(NEXT_FACE) = fo; MEM32(ADDED) = sum; g_ecx = (uint32_t)bytes; }
    g_eax = (sum & 0xFFFFFF00u) | 1u;
    g_edx = (uint32_t)nf;
    g_esp += 4;
}

/* ---- the hook: native, or (FAMILIA_NATIVE_CHECK=1, first calls) both, compared ---- */
static int g_check = -1, g_left = 30, g_left0;
static uint64_t g_total_faces, g_total_verts;
typedef struct { uint32_t va, n; uint8_t *pre, *orig; } region;
static void reg_take(region *r, uint32_t va, uint32_t end) {
    r->va = va; r->n = end > va ? end - va : 0;
    r->pre = malloc(r->n ? r->n : 1); r->orig = malloc(r->n ? r->n : 1);
    if (r->n) memcpy(r->pre, (const void *)ADDR(va), r->n);
}
static void reg_keep(region *r) { if (r->n) memcpy(r->orig, (const void *)ADDR(r->va), r->n); }
static void reg_restore(region *r) { if (r->n) memcpy((void *)ADDR(r->va), r->pre, r->n); }
static long reg_diff(region *r) {
    for (uint32_t i = 0; i < r->n; i++) if (((const uint8_t *)ADDR(r->va))[i] != r->orig[i]) return (long)i;
    return -1;
}
static void reg_free(region *r) { free(r->pre); free(r->orig); }

void fam_native_clip_faces_persp_hook(void) {
    if (!fam_native_clip_on) { recomp_ls3df_10002530_orig(); return; }
    if (g_check < 0) {
        const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_check = e && atoi(e);
        e = getenv("FAMILIA_CLIP_CHECK"); if (e && atoi(e) > 0) { g_check = 1; g_left = atoi(e); }   /* this many calls */
        g_left0 = g_left;
    }
    if (!g_check || g_left <= 0) { fam_native_clip_faces_persp(); return; }
    static const uint32_t globals[] = { CV_COUNT, CV_END, POLY_NUM, NEXT_FACE, NEXT_FACE_T, ADDED, ADDED_T };
    enum { NG = sizeof globals / sizeof *globals };
    uint32_t g0[NG], g1[NG];
    for (int i = 0; i < NG; i++) g0[i] = MEM32(globals[i]);
    uint32_t verts = MEM32(VERTS), first = MEM32(CV_COUNT), E = g_esp;
    uint32_t fo = MEM32(FG_BLEND) ? MEM32(NEXT_FACE_T) : MEM32(NEXT_FACE);
    uint32_t vend = hle_heap_block_end(verts), fend = hle_heap_block_end(fo);
    if (!vend || !fend) { fam_native_clip_faces_persp(); return; }    /* not our heap's large blocks: no check */
    region rv, rf, ra, rb, rs;
    reg_take(&rv, verts + first * 0x14u, vend);
    reg_take(&rf, fo, fend);
    reg_take(&ra, BUF_A, BUF_A + 1024);
    reg_take(&rb, BUF_B, BUF_B + 1024);
    reg_take(&rs, E - 0x110, E);                            /* the original's frame (min_i is in it) */
    uint32_t r0[4] = { g_eax, g_ecx, g_edx, g_esp };
    uint32_t nfaces = g_edx;
    /* the original */
    recomp_ls3df_10002530_orig();
    uint32_t r1[4] = { g_eax, g_ecx, g_edx, g_esp };
    for (int i = 0; i < NG; i++) g1[i] = MEM32(globals[i]);
    uint32_t min_orig = MEM32(E - 0x58);
    reg_keep(&rv); reg_keep(&rf); reg_keep(&ra); reg_keep(&rb);
    uint32_t added_v = MEM32(CV_COUNT) - first;
    /* back, then the native */
    reg_restore(&rv); reg_restore(&rf); reg_restore(&ra); reg_restore(&rb); reg_restore(&rs);
    for (int i = 0; i < NG; i++) MEM32(globals[i]) = g0[i];
    g_eax = r0[0]; g_ecx = r0[1]; g_edx = r0[2]; g_esp = r0[3];
    fam_native_clip_faces_persp();
    uint32_t r2[4] = { g_eax, g_ecx, g_edx, g_esp };
    char what[160] = "";
    long d;
    for (int i = 0; i < NG && !what[0]; i++) if (MEM32(globals[i]) != g1[i]) snprintf(what, sizeof what, "global %08X: %08X, native %08X", globals[i], g1[i], MEM32(globals[i]));
    for (int i = 0; i < 4 && !what[0]; i++) if (r1[i] != r2[i]) snprintf(what, sizeof what, "register %d: %08X, native %08X", i, r1[i], r2[i]);
    if (!what[0] && (d = reg_diff(&rv)) >= 0) snprintf(what, sizeof what, "clip vertex %u +%u", first + (uint32_t)(d / 0x14), (uint32_t)(d % 0x14));
    if (!what[0] && (d = reg_diff(&rf)) >= 0) snprintf(what, sizeof what, "face %ld", d / 6);
    if (!what[0] && (d = reg_diff(&ra)) >= 0) snprintf(what, sizeof what, "polygon buffer A [%ld]", d / 2);
    if (!what[0] && (d = reg_diff(&rb)) >= 0) snprintf(what, sizeof what, "polygon buffer B [%ld]", d / 2);
    if (!what[0] && MEM32(E - 0x58) != min_orig) snprintf(what, sizeof what, "min_i %u, native %u", min_orig, MEM32(E - 0x58));
    reg_free(&rv); reg_free(&rf); reg_free(&ra); reg_free(&rb); reg_free(&rs);
    if (what[0]) {
        fprintf(stderr, "[native] ClipFacesPersp MISMATCH (%u faces in): %s: the translated one from now on\n", nfaces, what);
        fam_native_clip_on = 0;
        return;
    }
    g_total_faces += nfaces; g_total_verts += added_v;
    if (--g_left == 0)
        fprintf(stderr, "[native] ClipFacesPersp: matches the translated original (%d calls, %llu faces, %llu new vertices)\n",
                g_left0, (unsigned long long)g_total_faces, (unsigned long long)g_total_verts);
}
