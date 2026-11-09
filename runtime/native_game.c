/*
 * native_game.c - prefetch hooks in Game.exe's per-object scans.
 *
 * Several per-frame routines walk the mission's object list backwards
 * (an array of object pointers) and test each object's type fields
 * (+0x10 type, +0x1c state) before doing anything with it:
 *
 *   id 200, 201: C_traffic_element::Tick (0x452CD0), two scans
 *                (loop heads 0x455579, 0x4556E1; list at [0x63D2BC]),
 *   id 203:      C_ParticleEmitter::Tick (0x448960), the loop over a part's
 *                particles calling C_Particle::Tick (head 0x448ABE): the
 *                particle a few ahead, and the visual of a nearer one.
 *
 * Most objects are skipped after that test, so the scans are bound by cache
 * misses on the objects. These splices (tools/splice_loops.py) run at each
 * loop head, prefetch the objects ahead (24 iterations for the object scans,
 * whose iterations are short, FAMILIA_GAMEPF_FAR; 6 for the particles) and
 * return 0: the translated loop itself runs unchanged, so the result is exact
 * by construction. FAMILIA_GAMEPF=0 turns them off.
 */
#include <stdlib.h>
#include <stdint.h>
#include "win32hle.h"
#include "recomp_types.h"

#define AHEAD 6
static int g_on = -1;

static int g_far = 24;        /* rejected objects take a few ns each: prefetch far ahead */
static inline void pf_obj(uint32_t list, uint32_t idx) {
    uint32_t o = MEM32(list + idx * 4u);
    if (o < 0x10000u) return;
    __builtin_prefetch((const char *)ADDR(o) + 0x10);     /* +0x10 type, +0x1c state */
}

/* list = the pointer array, n = the count before this iteration's decrement:
   this iteration takes element n-1 */
static inline void ahead(uint32_t list, uint32_t n) {
    if (n > (uint32_t)g_far) pf_obj(list, n - 1 - g_far);
}

int fam_game_splice(int id) {
    if (g_on < 0) { const char *e = getenv("FAMILIA_GAMEPF"); g_on = !e || atoi(e); e = getenv("FAMILIA_GAMEPF_FAR"); if (e) g_far = atoi(e); }
    if (!g_on) return 0;
    switch (id) {
    case 200: case 201: ahead(g_eax, g_ebx); break;              /* mov edx,[eax+ebx*4-4]; dec ebx */
    case 203: {                                                  /* esi = the part, ebp = j */
        uint32_t list = MEM32(g_esi + 0x20), end = MEM32(g_esi + 0x24), j = g_ebp;
        if (!list || end <= list) break;
        uint32_t n = (end - list) >> 2;
        if (j + AHEAD < n) {
            const char *p = (const char *)ADDR(MEM32(list + (j + AHEAD) * 4u));
            __builtin_prefetch(p); __builtin_prefetch(p + 0x40);
        }
        if (j + 2 < n) {
            uint32_t v = MEM32(MEM32(list + (j + 2) * 4u));      /* its visual */
            if (v >= 0x10000u) {
                const char *q = (const char *)ADDR(v);
                __builtin_prefetch(q); __builtin_prefetch(q + 0x40); __builtin_prefetch(q + 0x80);
                __builtin_prefetch(q + 0x200);
            }
        }
        break;
    }
    }
    return 0;
}
void fam_game_splice_exit(void) {}
