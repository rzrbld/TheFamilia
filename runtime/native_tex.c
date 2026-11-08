/*
 * native_tex.c - native pixel loops spliced into the translated
 * ITexture::Create (LS3DF 0x10075F50).
 *
 * Create is 2621 instructions of C_str / vector / COM / file handling around
 * a few integer pixel loops; the loops are what costs time while textures
 * stream in. Instead of rewriting the whole function, tools/splice_loops.py
 * (config/splices.json) inserts at each loop's entry label a call to
 * fam_tex_splice(id): when it returns 1 the native loop below has done the
 * loop's work - every guest register, frame slot and output byte the x86
 * loop would leave - and the translated code jumps straight to the loop's
 * exit label. Everything outside the loops stays translated.
 *
 *   0 0x10076C8C  8-bit palette image -> ARGB (colour key per format)
 *   1 0x10076E42  16-bit X1R5G5B5 image -> ARGB
 *   2 0x10076F60  24-bit image -> ARGB
 *   3 0x10077026  32-bit image -> ARGB
 *   4 0x10077520  next mip level from the previous one (2x2, 3/8 3/8 1/8 1/8)
 *
 * Each loop is transcribed from the x86 (byte-register writes, signed
 * compares, the stale-register quirks included); values the x86 keeps in
 * frame slots are kept in host registers and written to the frame once.
 *
 * Verification (FAMILIA_NATIVE_CHECK=1): the first 300 loop runs are done
 * twice - natively into a snapshot, then by the translated loop - and the
 * registers, the frame (0x2A0 bytes) and the output rows are compared at the
 * exit label. A difference prints the first words and switches the splices
 * off for the session.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

int fam_native_tex_on = 1;

#define S32(o) MEM32(esp + (o))
#define S16(o) MEM16(esp + (o))
#define S8(o)  MEM8(esp + (o))
#define LO(r, v) ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))

typedef struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; } regs;
static inline void get_regs(regs *r) { r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx; r->esp = g_esp; r->ebp = g_ebp; r->esi = g_esi; r->edi = g_edi; }
static inline void set_regs(const regs *r) { g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx; g_esp = r->esp; g_ebp = r->ebp; g_esi = r->esi; g_edi = r->edi; }

/* ---------------------------------------------------- 8-bit palette (0x10076C8C) */
static void loop_pal8(void) {
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, esp = g_esp;
    const uint32_t pal = S32(0x30), width = S32(0x7C), fmt = S32(0x54), adder = S32(0x2C), opt_adder = S32(0x50);
    const uint32_t flags_va = ebp + 0x30, ck_va = ebp + 0x38;
    eax = S32(0x3C);
    edx++; S32(0x28) = edx;
    edx = width;
    uint32_t opt = S32(0x14);
    for (;;) {
        uint32_t j = 0;                                   /* [esp+0x1c] */
        if ((int32_t)edx > 0) {
            for (;;) {
                esi = opt;
                LO(edx, MEM8(ebx));
                if (opt) {
                    LO(ecx, MEM8(opt));
                    MEM8(edi) = (uint8_t)ecx;
                    ecx = opt + 1; opt = ecx;
                } else if (!(MEM8(flags_va) & 8)) {
                    MEM8(edi) = 0xFF;
                } else {
                    uint32_t c = (uint8_t)edx;
                    int path;                             /* 0 D12 8888, 1 CF2 565, 2 D39 4444, 3 D5C 555, 4 D82 */
                    esi = fmt;
                    if ((int32_t)esi > 0x1A) path = esi == 0x31545844u ? 3 : esi == 0x33545844u ? 2 : 4;
                    else if (esi == 0x1A) path = 2;
                    else {
                        esi -= 0x15;
                        if (esi > 4) path = 4;
                        else { static const int t[5] = { 0, 4, 1, 3, 3 }; path = t[esi]; }
                    }
                    switch (path) {
                    case 0:                                /* D12 */
                        eax = ecx + c * 4;
                        ecx = (uint32_t)MEM8(eax + 2) << 8 | MEM8(eax + 1);
                        eax = MEM8(eax);
                        ecx <<= 8;
                        eax |= ecx;
                        break;
                    case 1:                                /* CF2 */
                        eax = c;
                        esi = MEM8(ecx + eax * 4 + 1) & 0xFFFFFFFCu;
                        ecx = ecx + eax * 4;
                        eax = MEM8(ecx + 2) & 0xFFFFFFF8u;
                        eax <<= 5; eax |= esi; eax <<= 3;
                        ecx = (uint32_t)MEM8(ecx) >> 3;
                        eax |= ecx;
                        break;
                    case 2:                                /* D39 */
                        eax = c;
                        esi = MEM8(ecx + eax * 4 + 1) & 0xFFFFFFF0u;
                        ecx = ecx + eax * 4;
                        eax = MEM8(ecx + 2);
                        ecx = MEM8(ecx);
                        eax &= 0xFFFFFFF0u; eax <<= 4; eax |= esi;
                        ecx >>= 4;
                        eax |= ecx;
                        break;
                    case 3:                                /* D5C */
                        eax = c;
                        esi = MEM8(ecx + eax * 4 + 1) & 0xFFFFFFF8u;
                        ecx = ecx + eax * 4;
                        eax = MEM8(ecx + 2) & 0xFFFFFFF8u;
                        eax <<= 5; eax |= esi; eax <<= 2;
                        ecx = (uint32_t)MEM8(ecx) >> 3;
                        eax |= ecx;
                        break;
                    default: break;                       /* D82 with eax as it is */
                    }
                    ecx = eax == MEM32(ck_va) ? 0u : 0xFFu;
                    MEM8(edi) = (uint8_t)ecx;
                }
                uint32_t c4 = (uint32_t)(uint8_t)edx << 2;  /* D97 */
                edi++;
                edx = (pal & 0xFFFFFF00u) | MEM8(c4 + pal + 2); MEM8(edi) = (uint8_t)edx;
                edx = (pal & 0xFFFFFF00u) | MEM8(c4 + pal + 1); edi++; MEM8(edi) = (uint8_t)edx;
                edx = pal;
                ecx = (c4 & 0xFFFFFF00u) | MEM8(c4 + pal); edi++; MEM8(edi) = (uint8_t)ecx;
                edx = width; edi++; ebx++; j++;
                ecx = pal;
                if (!((int32_t)j < (int32_t)edx)) break;
            }
        }
        S32(0x1C) = j;
        /* DD9 */
        ebx -= adder;
        esi = opt_adder;
        opt -= esi;
        uint32_t r = S32(0x28) - 1; S32(0x28) = r;
        if (!r) break;
    }
    S32(0x14) = opt;
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi;
}

/* ---------------------------------------------------- 16-bit (0x10076E42) */
static void loop_x555(void) {
    uint32_t eax = g_eax, ecx = g_ecx, edx, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, esp = g_esp;
    const uint32_t width = S32(0x7C), opt_adder = S32(0x50), flags = MEM32(ebp + 0x30), ck = MEM32(ebp + 0x38);
    edx = edi + edi;
    ecx++;
    S32(0x60) = edx;
    const uint32_t src_adder = edx;
    edx = width;
    S32(0x28) = ecx;
    uint32_t opt = S32(0x14), rows = ecx, j = 0;
    uint8_t b13 = S8(0x13);
    int any = 0;
    for (;;) {
        j = 0;
        if ((int32_t)edx > 0) {
            for (;;) {
                LO(ebx, MEM8(esi));
                edi = MEM16(esi);
                LO(ebx, (uint8_t)ebx << 3);
                b13 = (uint8_t)ebx;
                ebx = flags; any = 1;
                ecx = edi >> 7; edx = edi >> 2;
                LO(ecx, ecx & 0xF8); LO(edx, edx & 0xF8);
                if (ebx & 2) {
                    ebx = edi & 0x8000;
                    uint32_t cf = ebx != 0;
                    ebx = (uint32_t)-(int32_t)ebx;
                    LO(ebx, cf ? 0xFF : 0);
                    ebx &= 0xFF;
                    MEM8(eax) = (uint8_t)ebx;
                } else {
                    ebx = opt;
                    if (ebx) {
                        edi = ebx;
                        LO(ebx, MEM8(edi));
                        edi++;
                        MEM8(eax) = (uint8_t)ebx;
                        opt = edi;
                    } else if (!(flags & 8)) {
                        MEM8(eax) = 0xFF;
                    } else {
                        ebx = ck;
                        edi &= 0xFFFF;
                        MEM8(eax) = edi == ebx ? 0 : 0xFF;
                    }
                }
                MEM8(eax + 1) = (uint8_t)ecx;
                LO(ecx, b13);
                eax += 2; MEM8(eax) = (uint8_t)edx;
                eax++; MEM8(eax) = (uint8_t)ecx;
                j++;
                ecx = j; edx = width;
                eax++; esi += 2;
                if (!((int32_t)ecx < (int32_t)edx)) break;
            }
        }
        S32(0x1C) = j;
        /* EF8 */
        ecx = opt_adder;
        edi = opt;
        ebx = src_adder;
        edi -= ecx;
        rows--;
        ecx = rows;
        esi -= ebx;
        opt = edi;
        if (!rows) break;
    }
    S32(0x14) = opt; S32(0x28) = rows;
    if (any) { S8(0x13) = b13; S32(0x6C) = flags; }
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi;
}

/* ---------------------------------------------------- 24-bit (0x10076F60) */
static void loop_rgb24(void) {
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, esp = g_esp;
    const uint32_t width = S32(0x7C), adder = S32(0x2C), opt_adder = S32(0x50), flags_va = ebp + 0x30, ck_va = ebp + 0x38;
    edx++;
    uint32_t rows = edx;
    edx = width;
    uint8_t b13 = S8(0x13), b37 = S8(0x37);
    for (;;) {
        esi = 0;
        if ((int32_t)edx > 0) {
            for (;;) {
                LO(edx, MEM8(ecx + 2)); b13 = (uint8_t)edx;
                LO(edx, MEM8(ecx + 1)); b37 = (uint8_t)edx;
                LO(edx, MEM8(ecx));
                if (edi) {
                    LO(ebx, MEM8(edi));
                    MEM8(eax) = (uint8_t)ebx;
                    edi++;
                } else if (!(MEM8(flags_va) & 8)) {
                    MEM8(eax) = 0xFF;
                } else {
                    ebx = (uint8_t)edx;
                    MEM8(eax) = ebx == MEM32(ck_va) ? 0 : 0xFF;
                }
                LO(ebx, b13); eax++; MEM8(eax) = (uint8_t)ebx;
                LO(ebx, b37); eax++; MEM8(eax) = (uint8_t)ebx;
                eax++; MEM8(eax) = (uint8_t)edx;
                edx = width;
                eax++; ecx += 3; esi++;
                if (!((int32_t)esi < (int32_t)edx)) break;
            }
        }
        /* FC6 */
        esi = adder;
        ebx = opt_adder;
        ecx -= esi;
        rows--;
        esi = rows;
        edi -= ebx;
        if (!rows) break;
    }
    S32(0x1C) = rows; S8(0x13) = b13; S8(0x37) = b37;
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi;
}

/* ---------------------------------------------------- 32-bit (0x10077026) */
static void loop_argb32(void) {
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi, ebp = g_ebp, esp = g_esp;
    const uint32_t width = S32(0x7C), adder = S32(0x2C), opt_adder = S32(0x50), flags = MEM32(ebp + 0x30), ck_va = ebp + 0x38;
    edi = edx + 1;
    edx = width;
    uint32_t opt = S32(0x38);
    uint8_t b13 = S8(0x13), b37 = S8(0x37);
    for (;;) {
        esi = 0;
        if ((int32_t)edx > 0) {
            for (;;) {
                LO(edx, MEM8(ecx + 2));
                LO(ebx, MEM8(ecx));
                b37 = (uint8_t)edx;
                LO(edx, MEM8(ecx + 1));
                b13 = (uint8_t)edx;
                edx = flags;
                if (edx & 2) {
                    LO(edx, MEM8(ecx + 3));
                    MEM8(eax) = (uint8_t)edx;
                } else if (opt) {
                    edx = opt;
                    LO(edx, MEM8(edx));
                    MEM8(eax) = (uint8_t)edx;
                    opt++;
                } else if (!(edx & 8)) {
                    MEM8(eax) = 0xFF;
                } else {
                    edx = (uint8_t)ebx;
                    MEM8(eax) = edx == MEM32(ck_va) ? 0 : 0xFF;
                }
                LO(edx, b37); eax++; MEM8(eax) = (uint8_t)edx;
                LO(edx, b13); eax++; MEM8(eax) = (uint8_t)edx;
                eax++; MEM8(eax) = (uint8_t)ebx;
                edx = width;
                eax++; ecx += 4; esi++;
                if (!((int32_t)esi < (int32_t)edx)) break;
            }
        }
        /* 109E */
        esi = adder;
        ebx = opt;
        ecx -= esi;
        esi = opt_adder;
        ebx -= esi;
        edi--;
        opt = ebx;
        if (!edi) break;
    }
    S32(0x38) = opt; S8(0x13) = b13; S8(0x37) = b37;
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi;
}

/* ---------------------------------------------------- mip level (0x10077520) */
static inline uint32_t sar3(uint32_t v) { return (uint32_t)((int32_t)v >> 3); }
static void loop_mip(void) {
    uint32_t eax, ecx = g_ecx, edx, ebx = g_ebx, esi = g_esi, edi, ebp = g_ebp, esp = g_esp;
    /* the upper halves of the word slots the loop reads back as dwords */
    static const uint16_t wslot[13] = { 0xF0, 0xF4, 0xF8, 0x104, 0x108, 0x10C, 0x110, 0x114, 0x118, 0x11C, 0x124, 0x128, 0x12C };
    uint32_t hi[13];
    for (int k = 0; k < 13; k++) hi[k] = (uint32_t)S16(wslot[k] + 2) << 16;
#define HI(off) hi[(off) == 0xF0 ? 0 : (off) == 0xF4 ? 1 : (off) == 0xF8 ? 2 : (off) == 0x104 ? 3 : (off) == 0x108 ? 4 : \
                   (off) == 0x10C ? 5 : (off) == 0x110 ? 6 : (off) == 0x114 ? 7 : (off) == 0x118 ? 8 : (off) == 0x11C ? 9 : \
                   (off) == 0x124 ? 10 : (off) == 0x128 ? 11 : 12]
    uint16_t w[13]; int have_w = 0;
    eax = 0;
    ecx--; ecx >>= 1; ecx++;
    uint32_t s64 = eax, s74 = eax, s6c = ecx;
    const uint32_t s3c = S32(0x3C), pitch = S32(0x50), s38 = S32(0x38), src0 = S32(0x14), dst0 = S32(0x24), s274 = S32(0x274);
    uint32_t s70 = 0, sc4 = 0, sb0 = 0, s60 = 0, sfc = 0, sb8 = 0, sb4 = 0, s100 = 0, s120 = 0, sa0 = 0;
    int row_setup = 0;
    edi = g_edi;
    for (;;) {
        edx = s38 - 1;
        if ((int32_t)edx > 0) {
            ecx = s74;
            edx = ebx + 1;
            edi = esi + 1; edi += ecx;
            ecx = pitch; ecx += eax;
            ebp = ebx + ecx + 2;
            ebx -= edx; s70 = ebx;
            sc4 = src0 + 2 - edx;
            sb0 = src0 + 3 - edx;
            ebx = s64;
            eax += edx;
            s60 = ecx;
            sfc = s60 - ebx;
            edx = src0; ebx = edx; edx += 2; ebx -= edx; sb8 = ebx;
            sb4 = src0 + 3 - edx;
            edx = dst0; ebx = edx; edx++; ebx -= edx; s100 = ebx;
            s120 = dst0 - edx + 2;
            ebx = dst0 - edx;
            edx = s38; edx += 0xFFFFFFFEu; ebx += 3; edx >>= 1; edx++;
            ecx = ebp + 4;
            esi = eax + 4;
            sa0 = ebx;
            s60 = edx;
            row_setup = 1;
            /* the pixels: four source pixels to one, per channel */
            do {
                uint16_t v110 = MEM8(eax + sfc), vf0 = MEM8(ebp), v118 = MEM8(sb4 + ebp), vf8 = MEM8(s70 + esi);
                uint16_t v128 = MEM8(eax), v108 = MEM8(esi), v11c = MEM8(sc4 + esi), v104 = MEM8(eax + sc4);
                uint16_t v10c = MEM8(sb0 + esi), vf4 = MEM8(eax + sb0), v12c = MEM8(pitch + esi), v124 = MEM8(ecx);
                uint16_t v114 = MEM8(sb4 + ecx);
                uint32_t e_b8ebp = (sb8 & 0xFFFF0000u) | MEM8(sb8 + ebp);      /* edx = [esp+0xb8]; dx = [edx+ebp] */
                uint32_t bl;
                /* channel at dst-1: (3*f8 >> 3) + (3*[b8+ebp] >> 3) + ([b8+ecx] >> 3) + ([eax+70] >> 3) */
                edx = sar3(e_b8ebp * 3);
                ebx = sar3((HI(0xF8) | vf8) * 3);
                bl = (uint8_t)((uint8_t)ebx + (uint8_t)edx);
                edx = ((sb8 & 0xFFFF0000u) | MEM8(sb8 + ecx)) >> 3; bl = (uint8_t)(bl + (uint8_t)edx);
                edx = ((s70 & 0xFFFF0000u) | MEM8(eax + s70)) >> 3; bl = (uint8_t)(bl + (uint8_t)edx);
                MEM8(edi + s100) = (uint8_t)bl;
                /* channel at dst */
                ebx = sar3((HI(0x108) | v108) * 3);
                edx = sar3((HI(0x110) | v110) * 3);
                bl = (uint8_t)((uint8_t)ebx + (uint8_t)edx);
                bl = (uint8_t)(bl + (uint8_t)((HI(0x12C) | v12c) >> 3));
                bl = (uint8_t)(bl + (uint8_t)((HI(0x128) | v128) >> 3));
                MEM8(edi) = (uint8_t)bl;
                /* channel at dst+1 */
                ebx = sar3((HI(0x11C) | v11c) * 3);
                edx = sar3((HI(0xF0) | vf0) * 3);
                bl = (uint8_t)((uint8_t)ebx + (uint8_t)edx);
                bl = (uint8_t)(bl + (uint8_t)((HI(0x124) | v124) >> 3));
                bl = (uint8_t)(bl + (uint8_t)((HI(0x104) | v104) >> 3));
                MEM8(edi + s120) = (uint8_t)bl;
                /* channel at dst+2 */
                ebx = sar3((HI(0x10C) | v10c) * 3);
                edx = sar3((HI(0x118) | v118) * 3);
                bl = (uint8_t)((uint8_t)ebx + (uint8_t)edx);
                uint32_t last_edx = (HI(0xF4) | vf4) >> 3;
                bl = (uint8_t)(bl + (uint8_t)((HI(0x114) | v114) >> 3));
                bl = (uint8_t)(bl + (uint8_t)last_edx);
                MEM8(edi + sa0) = (uint8_t)bl;
                ebx = (ebx & 0xFFFFFF00u) | bl;
                w[0] = vf0; w[1] = vf4; w[2] = vf8; w[3] = v104; w[4] = v108; w[5] = v10c; w[6] = v110; w[7] = v114;
                w[8] = v118; w[9] = v11c; w[10] = v124; w[11] = v128; w[12] = v12c; have_w = 1;
                edx = s60;
                edi += 4; eax += 8; ebp += 8; esi += 8; ecx += 8;
                edx--; s60 = edx;
            } while (edx);
            esi = dst0; ecx = s6c; ebp = s274; ebx = src0; eax = s64; edi = s3c;
        }
        /* 10077812 */
        s74 += edi;
        edx = pitch; edx += edx;
        eax += edx;
        ecx--;
        s64 = eax; s6c = ecx;
        if (!ecx) break;
    }
#undef HI
    S32(0x64) = s64; S32(0x74) = s74; S32(0x6C) = s6c;
    if (row_setup) {
        S32(0x70) = s70; S32(0xC4) = sc4; S32(0xB0) = sb0; S32(0x60) = s60; S32(0xFC) = sfc; S32(0xB8) = sb8;
        S32(0xB4) = sb4; S32(0x100) = s100; S32(0x120) = s120; S32(0xA0) = sa0;
    }
    if (have_w) for (int k = 0; k < 13; k++) S16(wslot[k]) = w[k];
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp;
}

/* ---------------------------------------------------- the splice entry/exit */
typedef void (*loop_fn)(void);
static const loop_fn g_loops[5] = { loop_pal8, loop_x555, loop_rgb24, loop_argb32, loop_mip };
static const char *const g_loop_names[5] = { "8-bit palette", "16-bit", "24-bit", "32-bit", "mip level" };

/* the output each loop writes, from the entry registers (generous) */
static void out_region(int id, uint32_t *lo, uint32_t *n) {
    uint32_t esp = g_esp;
    switch (id) {
    case 0: *lo = g_edi; *n = (g_edx + 1) * S32(0x7C) * 4 + 16; break;
    case 1: *lo = g_eax; *n = (g_ecx + 1) * S32(0x7C) * 4 + 16; break;
    case 2: *lo = g_eax; *n = (g_edx + 1) * S32(0x7C) * 4 + 16; break;
    case 3: *lo = g_eax; *n = (g_edx + 1) * S32(0x7C) * 4 + 16; break;
    default: {
        uint32_t rows = ((g_ecx - 1) >> 1) + 1;
        *lo = S32(0x24); *n = rows * (S32(0x50) >> 1) + S32(0x38) * 2 + 32;
        break;
    }
    }
    if (*n > (64u << 20)) *n = 64u << 20;
}

#define FRAME 0x2A0
static int g_verify = -1, g_pending = -1, g_verified;
static regs g_want;
static uint8_t g_frame_want[FRAME], *g_out_want;
static uint32_t g_out_lo, g_out_n;

int fam_tex_splice(int id) {
    if (!fam_native_tex_on || id < 0 || id > 4) return 0;
    if (g_verify < 0) { const char *e = getenv("FAMILIA_NATIVE_CHECK"); g_verify = e && atoi(e); }
    if (g_pending >= 0) return 0;                       /* a verification run of the translated loop is under way */
    if (!g_verify) { g_loops[id](); return 1; }
    /* verify: native into the real state, keep its result, restore, let the x86 loop run, compare at the exit */
    regs in; get_regs(&in);
    uint32_t lo, n; out_region(id, &lo, &n);
    uint8_t *frame0 = malloc(FRAME), *out0 = malloc(n);
    g_out_want = realloc(g_out_want, n);
    if (!frame0 || !out0 || !g_out_want) { free(frame0); free(out0); g_loops[id](); return 1; }
    memcpy(frame0, (void *)ADDR(in.esp), FRAME); memcpy(out0, (void *)ADDR(lo), n);
    g_loops[id]();
    get_regs(&g_want);
    memcpy(g_frame_want, (void *)ADDR(in.esp), FRAME); memcpy(g_out_want, (void *)ADDR(lo), n);
    memcpy((void *)ADDR(in.esp), frame0, FRAME); memcpy((void *)ADDR(lo), out0, n);
    set_regs(&in);
    free(frame0); free(out0);
    g_out_lo = lo; g_out_n = n; g_pending = id;
    return 0;
}

void fam_tex_splice_exit(void) {
    if (g_pending < 0) return;
    int id = g_pending; g_pending = -1;
    regs got; get_regs(&got);
    int bad = 0;
    static const char *const rn[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    const uint32_t *a = &g_want.eax, *b = &got.eax;
    for (int i = 0; i < 8; i++)
        if (a[i] != b[i]) { if (bad < 3) fprintf(stderr, "[native]   %s: %08X native / %08X translated\n", rn[i], a[i], b[i]); bad++; }
    const uint8_t *fr = (const uint8_t *)ADDR(got.esp);
    for (uint32_t o = 0; o + 4 <= FRAME; o += 4)
        if (memcmp(g_frame_want + o, fr + o, 4)) {
            uint32_t x, y; memcpy(&x, g_frame_want + o, 4); memcpy(&y, fr + o, 4);
            if (bad < 3) fprintf(stderr, "[native]   [esp+%03X]: %08X native / %08X translated\n", o, x, y);
            bad++;
        }
    const uint8_t *out = (const uint8_t *)ADDR(g_out_lo);
    for (uint32_t o = 0; o < g_out_n; o++)
        if (g_out_want[o] != out[o]) {
            if (bad < 3) fprintf(stderr, "[native]   output +%u: %02X native / %02X translated\n", o, g_out_want[o], out[o]);
            bad++;
        }
    if (bad) {
        fprintf(stderr, "[native] ITexture::Create %s loop MISMATCH (%d differences): texture pixel loops disabled\n", g_loop_names[id], bad);
        fam_native_tex_on = 0;
        return;
    }
    if (++g_verified == 300) {
        fprintf(stderr, "[native] ITexture::Create pixel loops: 300 runs match the translated loops (verification off)\n");
        g_verify = 0;
    }
}
