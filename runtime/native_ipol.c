/*
 * native_ipol.c - native I3D_interpolator::Update (LS3DF 0x1001D320): the
 * per-frame keyframe interpolation of every animated frame (skeletons,
 * doors, vehicles).
 *
 * Transcribed from the compiled code, not the C++: each x87 value is a
 * double, rounded to float exactly where the original stores a dword
 * (compiled with -ffp-contract=off). Slerp, Normalize and S_quat::Make are
 * called through the guest ABI, so their results are the translated (or
 * their own native) ones. The rare paths - a pose blended in, and note
 * callbacks at the end - run the translated original as a whole.
 * fam_native_ipol_check (FAMILIA_NATIVE_CHECK=1) compares it with the
 * original on random interpolators; a mismatch switches it off.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

int fam_native_ipol_on = 1;
void recomp_ls3df_1001D320_orig(void);

static inline float ldf(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static inline void stf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

#define K_ZERO   0x1009B310u          /* 0.0f */
#define K_ONE    0x1009B304u          /* 1.0f */
#define K_TUNIT  0x1009BA64u          /* 0.025f: the key time unit */
#define VA_SLERP 0x1002FEE0u
#define VA_NORM  0x10030140u
#define VA_QMAKE 0x1002EEF0u

/* interpolator */
#define IP_POSE    0x04
#define IP_PBLEND  0x08
#define IP_KEYS    0x0C               /* 8 x 0x1C: pos, rot, scale, note key arrays; counts; flags */
#define IP_FRAME   0xEC
#define IP_SET     0xF0
/* ipol_set->anims[i], 0x1C each */
#define AN_ON      0x0C
#define AN_TIME    0x14
#define AN_BLEND   0x20
/* frame */
#define FR_MAT     0x50
#define FR_POS     0x80
#define FR_SCALE   0x90
#define FR_ROT     0x9C
#define FR_FLAGS   0xAC
#define FR_CB      0x108

static void guest_call(uint32_t fn) {      /* the caller has pushed the arguments */
    recomp_func_t f = recomp_lookup_manual(fn);
    if (!f) f = recomp_lookup(fn);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
}
/* S_quat S_quat::Slerp(const S_quat &q, float t, bool) - stdcall, this on the stack, ret 0x14 */
static void slerp(uint32_t self, uint32_t ret, uint32_t q, uint32_t tbits) {
    PUSH32(g_esp, 0); PUSH32(g_esp, tbits); PUSH32(g_esp, q); PUSH32(g_esp, ret); PUSH32(g_esp, self);
    g_ecx = self;
    guest_call(VA_SLERP);
}

/* the key interval for time t: returns the value pointer to use directly
 * (before the first or after the last key), or the two keys and the fraction for a lerp */
typedef struct { uint32_t v, v0, v1; int lerp; double f; } keypos;
static keypos find_key(uint32_t times, int n, uint32_t time, uint32_t stride) {
    keypos k = { 0, 0, 0, 0, 0 };
    uint16_t t = (uint16_t)((int32_t)time / 40);
    uint32_t vals = times + (uint32_t)n * 2 + ((n & 1) ? 0 : 2);
    if (MEM16(times) > t) { k.v = vals; return k; }
    n--;
    if (!(MEM16(times + (uint32_t)n * 2) > t)) { k.v = vals + (uint32_t)n * stride; return k; }
    int j = 1;
    while (j < n && !(MEM16(times + (uint32_t)j * 2) > t)) j++;
    int t0 = MEM16(times + (uint32_t)(j - 1) * 2), t1 = MEM16(times + (uint32_t)j * 2);
    k.v1 = vals + (uint32_t)j * stride; k.v0 = k.v1 - stride;
    k.f = ((double)(int32_t)time * (double)ldf(K_TUNIT) - (double)t0) / (double)(t1 - t0);
    k.lerp = 1;
    return k;
}
/* v0 + (v1 - v0) * f as compiled: x and y keep the difference unrounded, z rounds it
 * first; x and z round the product, y does not */
static void lerp3(const keypos *k, float out[3]) {
    double dx = (double)ldf(k->v1) - ldf(k->v0), dy = (double)ldf(k->v1 + 4) - ldf(k->v0 + 4);
    float dz = (float)((double)ldf(k->v1 + 8) - ldf(k->v0 + 8));
    float px = (float)(dx * k->f), pz = (float)((double)dz * k->f);
    out[0] = (float)((double)px + ldf(k->v0));
    out[1] = (float)(dy * k->f + ldf(k->v0 + 4));
    out[2] = (float)((double)pz + ldf(k->v0 + 8));
}
/* acc = v * b + acc * (1 - b) as compiled */
static void blend3(float acc[3], uint32_t v, float b) {
    double ib = (double)ldf(K_ONE) - b;
    float ax = (float)((double)acc[0] * ib), ay = (float)((double)acc[1] * ib);
    double az = (double)acc[2] * ib;
    double bx = (double)b * ldf(v), by = (double)b * ldf(v + 4);
    float bz = (float)((double)b * ldf(v + 8));
    acc[0] = (float)(bx + ax);
    acc[1] = (float)(by + ay);
    acc[2] = (float)((double)bz + az);
}

static uint64_t g_ipol_native, g_ipol_fallback;

/* void I3D_interpolator::Update()  -- LS3DF 0x1001D320, thiscall, ret */
void fam_native_ipol_update(void) {
    uint32_t self = g_ecx, frame = MEM32(self + IP_FRAME);
    if (!fam_native_ipol_on || !frame) { recomp_ls3df_1001D320_orig(); return; }
    uint32_t pose = MEM32(self + IP_POSE);
    if ((pose && (double)ldf(self + IP_PBLEND) > 0.0) ||
        ((MEM32(frame + FR_FLAGS) & 0x1200) && MEM32(frame + FR_CB))) {
        g_ipol_fallback++;
        recomp_ls3df_1001D320_orig();
        return;
    }
    g_ipol_native++;
    uint32_t sp0 = g_esp;
    g_esp -= 0x100;                                  /* a frame of our own for the callees and buffers */
    uint32_t buf_ret = g_esp + 0x40, buf_q = g_esp + 0x60, buf_rot = g_esp + 0x80;
    uint32_t set = MEM32(self + IP_SET);
    float pos[3] = { 0, 0, 0 }, scale[3] = { 0, 0, 0 };
    int num_pos = 0, num_rot = 0, num_scale = 0;

    for (int i = 0; i < 8; i++) {                    /* positions */
        uint32_t key = self + IP_KEYS + (uint32_t)i * 0x1C, an = set + (uint32_t)i * 0x1C;
        if (!(MEM8(key + 0x18) & 2)) continue;
        if ((double)ldf(an + AN_BLEND) <= 0.0) continue;
        if (!MEM8(an + AN_ON)) continue;
        keypos k = find_key(MEM32(key), MEM16(key + 0x10), MEM32(an + AN_TIME), 12);
        float v[3];
        if (k.lerp) lerp3(&k, v); else { v[0] = ldf(k.v); v[1] = ldf(k.v + 4); v[2] = ldf(k.v + 8); }
        float b = ldf(an + AN_BLEND);
        if (num_pos && !((double)b >= 1.0)) {
            stf(buf_q, v[0]); stf(buf_q + 4, v[1]); stf(buf_q + 8, v[2]);
            blend3(pos, buf_q, b);
        } else memcpy(pos, v, 12);
        num_pos++;
    }

    for (int i = 0; i < 8; i++) {                    /* rotations */
        uint32_t key = self + IP_KEYS + (uint32_t)i * 0x1C, an = set + (uint32_t)i * 0x1C;
        if (!MEM8(an + AN_ON)) continue;
        if (!(MEM8(key + 0x18) & 4)) continue;
        if ((double)ldf(an + AN_BLEND) <= 0.0) continue;
        keypos k = find_key(MEM32(key + 4), MEM16(key + 0x12), MEM32(an + AN_TIME), 16);
        uint32_t p = k.v;
        if (k.lerp) {
            float f = (float)k.f;
            uint32_t fbits; memcpy(&fbits, &f, 4);
            slerp(k.v0, buf_ret, k.v1, fbits);
            for (int w = 0; w < 4; w++) MEM32(buf_q + 4 * w) = MEM32(g_eax + 4 * w);
            p = buf_q;
        }
        if (num_rot && !((double)ldf(an + AN_BLEND) >= 1.0)) {
            slerp(buf_rot, buf_ret, p, MEM32(an + AN_BLEND));
            for (int w = 0; w < 4; w++) MEM32(buf_rot + 4 * w) = MEM32(g_eax + 4 * w);
            num_rot++;
        } else {
            for (int w = 0; w < 4; w++) MEM32(buf_rot + 4 * w) = MEM32(p + 4 * w);
            num_rot = 1;
        }
    }

    for (int i = 0; i < 8; i++) {                    /* scales */
        uint32_t key = self + IP_KEYS + (uint32_t)i * 0x1C, an = set + (uint32_t)i * 0x1C;
        if (!(MEM8(key + 0x18) & 8)) continue;
        if ((double)ldf(an + AN_BLEND) <= 0.0) continue;
        if (!MEM8(an + AN_ON)) continue;
        keypos k = find_key(MEM32(key + 8), MEM16(key + 0x14), MEM32(an + AN_TIME), 12);
        float v[3];
        if (k.lerp) lerp3(&k, v); else { v[0] = ldf(k.v); v[1] = ldf(k.v + 4); v[2] = ldf(k.v + 8); }
        float b = ldf(an + AN_BLEND);
        if (num_scale && !((double)b >= 1.0)) {
            stf(buf_q, v[0]); stf(buf_q + 4, v[1]); stf(buf_q + 8, v[2]);
            blend3(scale, buf_q, b);
        } else memcpy(scale, v, 12);
        num_scale++;
    }

    /* the frame: SetPos, the rotation, SetScale (inlined in the original) */
    if (num_pos && !(MEM8(frame + FR_FLAGS) & 0x80)) {
        stf(frame + FR_POS, pos[0]); stf(frame + FR_POS + 4, pos[1]); stf(frame + FR_POS + 8, pos[2]);
        MEM32(frame + FR_FLAGS) = (MEM32(frame + FR_FLAGS) & 0xFFFFFEDFu) | 0x40000000u;
    }
    if (num_rot) {
        if (num_rot > 1) { PUSH32(g_esp, buf_rot); guest_call(VA_NORM); }
        for (int w = 0; w < 4; w++) MEM32(frame + FR_ROT + 4 * w) = MEM32(buf_rot + 4 * w);
        PUSH32(g_esp, frame + FR_ROT); guest_call(VA_NORM);
        MEM32(frame + FR_FLAGS) = (MEM32(frame + FR_FLAGS) & 0xFFFFFECBu) | 0x40000008u;
    }
    if (num_scale) {
        stf(frame + FR_SCALE, scale[0]); stf(frame + FR_SCALE + 4, scale[1]); stf(frame + FR_SCALE + 8, scale[2]);
        if (!(MEM8(frame + FR_FLAGS) & 8)) {
            PUSH32(g_esp, frame + FR_MAT); PUSH32(g_esp, frame + FR_ROT);
            g_ecx = frame + FR_ROT;
            guest_call(VA_QMAKE);
        }
        MEM32(frame + FR_FLAGS) = (MEM32(frame + FR_FLAGS) & 0xFFFFFECBu) | 0x40000008u;
    }
    g_esp = sp0 + 4;                                 /* ret */
}

/* ---------------------------------------------------- differential check */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 33) % n; }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)rnd(1000001) / 1000000.0f; }

int fam_native_ipol_check(void) {
    enum { SZ = 0x2000, CASES = 20000 };
    uint32_t mem = hle_va(hle_heap_alloc(SZ, 1));
    static uint8_t snap[SZ], r1[SZ];
    uint32_t SELF = mem, SET = mem + 0x100, FRAME = mem + 0x200, KEYS = mem + 0x400;   /* keys: 8 anims x 3 tracks x 0x100 */
    int bad = 0, nat = 0;
    for (int n = 0; n < CASES && bad < 5; n++) {
        memset((void *)ADDR(mem), 0, SZ);
        for (int i = 0; i < 8; i++) {
            uint32_t key = SELF + IP_KEYS + (uint32_t)i * 0x1C, an = SET + (uint32_t)i * 0x1C;
            uint8_t fl = (uint8_t)(rnd(3) ? (rnd(2) * 2 | rnd(2) * 4 | rnd(2) * 8) : 0);
            if (rnd(4) == 0) fl |= 0x30 & (rnd(2) ? 0x10 : 0);           /* note flags: unused without a callback */
            MEM8(key + 0x18) = fl;
            MEM8(an + AN_ON) = (uint8_t)(rnd(5) != 0);
            static const float bl[] = { 0.0f, -0.5f, 1.0f, 1.5f, 0.25f, 0.5f };
            float b = rnd(3) ? rndf(0, 1) : bl[rnd(6)];
            if (rnd(60) == 0) { uint32_t q = 0x7FC00000u; memcpy(&b, &q, 4); }     /* NaN */
            stf(an + AN_BLEND, b);
            int last_t = 0;
            for (int tr = 0; tr < 3; tr++) {
                uint32_t times = KEYS + ((uint32_t)i * 3 + (uint32_t)tr) * 0x100;
                int cnt = 1 + (int)rnd(6), stride = tr == 1 ? 16 : 12;
                MEM32(key + 4 * (uint32_t)tr) = times;
                MEM16(key + 0x10 + 2 * (uint32_t)tr) = (uint16_t)cnt;
                int t = (int)rnd(40);
                for (int j = 0; j < cnt; j++) { MEM16(times + 2 * (uint32_t)j) = (uint16_t)t; t += rnd(25) == 0 ? 0 : 1 + (int)rnd(30); }
                if (t > last_t) last_t = t;
                uint32_t vals = times + (uint32_t)cnt * 2 + ((cnt & 1) ? 0 : 2);
                for (int j = 0; j < cnt; j++) {
                    if (tr == 1) {
                        double q[4], l = 0; for (int w = 0; w < 4; w++) { q[w] = rndf(-1, 1); l += q[w] * q[w]; }
                        for (int w = 0; w < 4; w++) stf(vals + (uint32_t)j * 16 + 4 * (uint32_t)w, (float)(rnd(5) && l > 0 ? q[w] / sqrt(l) : q[w]));
                    } else for (int w = 0; w < 3; w++) stf(vals + (uint32_t)j * (uint32_t)stride + 4 * (uint32_t)w, rndf(-50, 50));
                }
            }
            int32_t time = (int32_t)rnd((uint32_t)(last_t + 10) * 40);
            if (rnd(30) == 0) time = -(int32_t)rnd(200);
            MEM32(an + AN_TIME) = (uint32_t)time;
        }
        /* the frame: pos, scale, rotation, a matrix for S_quat::Make, flags */
        for (uint32_t o = 0x50; o < 0xAC; o += 4) stf(FRAME + o, rndf(-2, 2));
        MEM32(FRAME + FR_FLAGS) = (rnd(2) ? 0x80u : 0) | (rnd(2) ? 8u : 0) | (rnd(2) ? 0x120u : 0) | (rnd(4) == 0 ? 0x1200u : 0) | rnd(2) * 0x40000000u;
        MEM32(FRAME + FR_CB) = 0;                         /* note callbacks would take the fallback */
        MEM32(SELF + IP_FRAME) = FRAME;
        MEM32(SELF + IP_SET) = SET;
        MEM32(SELF + IP_POSE) = rnd(4) == 0 ? FRAME : 0;  /* with pose_blend <= 0: ignored */
        stf(SELF + IP_PBLEND, rnd(2) ? 0.0f : -1.0f);
        memcpy(snap, (void *)ADDR(mem), SZ);
        uint32_t dsp[2]; int dst[2];
        for (int pass = 0; pass < 2; pass++) {
            memcpy((void *)ADDR(mem), snap, SZ);
            uint32_t esp0 = g_esp; int sb0 = g_st_base;
            g_ecx = SELF;
            PUSH32(g_esp, RECOMP_RETADDR);
            uint64_t before = g_ipol_native;
            (pass ? fam_native_ipol_update : recomp_ls3df_1001D320_orig)();
            if (pass && g_ipol_native != before) nat++;
            dsp[pass] = g_esp - esp0; dst[pass] = g_st_base - sb0;
            g_esp = esp0; g_st_base = sb0;
            if (!pass) memcpy(r1, (void *)ADDR(mem), SZ);
        }
        int nd = 0;
        for (uint32_t o = 0; o < SZ; o += 4) if (memcmp(r1 + o, (void *)ADDR(mem + o), 4)) nd++;
        if (nd || dsp[0] != dsp[1] || dst[0] != dst[1]) {
            if (!bad) {
                fprintf(stderr, "[native] I3D_interpolator::Update MISMATCH (case %d): %d words, esp %d/%d x87 %d/%d\n",
                        n, nd, (int)dsp[0], (int)dsp[1], dst[0], dst[1]);
                int shown = 0;
                for (uint32_t o = 0; o < SZ && shown < 4; o += 4) {
                    uint32_t x, y; memcpy(&x, r1 + o, 4); y = MEM32(mem + o);
                    if (x != y) { fprintf(stderr, "[native]   +%03X: %08X / %08X (input %08X)\n", o, x, y, *(uint32_t *)(snap + o)); shown++; }
                }
            }
            bad++;
        }
    }
    if (bad) fam_native_ipol_on = 0;
    fprintf(stderr, "[native] I3D_interpolator::Update: %s against the translated original (%d random interpolators, %d on the native path)\n",
            bad ? "DIFFERS - disabled" : "matches", CASES, nat);
    hle_heap_free(hle_ptr(mem));
    return 0;                                        /* a mismatch turns off only this function */
}

/* how often the native path runs (FAMILIA_NATIVE_CHECK reports it once) */
void fam_native_ipol_stats(void) {
    fprintf(stderr, "[native] interpolator: %llu native, %llu fallback\n",
            (unsigned long long)g_ipol_native, (unsigned long long)g_ipol_fallback);
}
