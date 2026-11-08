/*
 * shims_misc.c - the few Win32 calls Mafia's modules import that win32hle
 * does not answer (see analysis/imports.txt and build/hle_names.txt).
 * Each is stdcall; RET(eax, argc) pops the arguments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "win32hle.h"
#include "familia.h"

/* --- kernel32 --- */
static void k_DisableThreadLibraryCalls(void) { RET(1, 1); }

/* --- user32 --- */
static void u_SetClassLongA(void)  { RET(0, 3); }          /* (hwnd, index, value): previous value */
static void u_GetQueueStatus(void) { RET(0, 1); }          /* nothing queued: the engine then pumps anyway */
static void u_IsChild(void)        { RET(0, 2); }
static void u_VkKeyScanA(void) {                           /* (ch) -> low byte VK, high byte shift state */
    uint32_t c = A32(0) & 0xFF;
    uint32_t r;
    if (c >= 'a' && c <= 'z') r = c - 32;
    else if (c >= 'A' && c <= 'Z') r = c | 0x100;
    else if (c >= '0' && c <= '9') r = c;
    else if (c == ' ') r = 0x20;
    else r = 0xFFFF;
    RET(r, 1);
}

/* --- ole32 task allocator: guest heap --- */
static void o_CoTaskMemAlloc(void) { RETP(hle_heap_alloc(A32(0), 0), 1); }
static void o_CoTaskMemFree(void)  { if (A32(0)) hle_heap_free(APTR(0)); RET(0, 1); }
static void o_CoFreeUnusedLibraries(void) { RET(0, 0); }

/* --- binkw32 (1.9-era exports Mafia's LS3DF links) --- */
static void b_SetVolume12(void) {                         /* (bink, trackid, volume 0..32768) */
    uint32_t va = hle_resolve("_BinkSetVolume@8");
    uint32_t a[2] = { A32(0), A32(2) };
    if (va) hle_call_guest(va, 2, a);
    RET(0, 3);
}
static void b_CopyToBufferRect(void) {
    /* (bink, dest, pitch, destheight, destx, desty, srcx, srcy, srcw, srch, flags).
     * win32hle copies whole frames; the engine asks for the full frame at the
     * origin, so the rect form forwards when it covers the frame. */
    uint32_t va = hle_resolve("_BinkCopyToBuffer@28");
    if (A32(6) || A32(7))
        fprintf(stderr, "[bink] CopyToBufferRect with source offset %u,%u copies the whole frame\n", A32(6), A32(7));
    uint32_t a[7] = { A32(0), A32(1), A32(2), A32(3), A32(4), A32(5), A32(10) };
    uint32_t r = va ? hle_call_guest(va, 7, a) : 0;
    RET(r, 11);
}

static const win32hle_shim g_misc[] = {
    { "kernel32.dll!DisableThreadLibraryCalls", k_DisableThreadLibraryCalls },
    { "user32.dll!SetClassLongA",   u_SetClassLongA },
    { "user32.dll!GetQueueStatus",  u_GetQueueStatus },
    { "user32.dll!IsChild",         u_IsChild },
    { "user32.dll!VkKeyScanA",      u_VkKeyScanA },
    { "ole32.dll!CoTaskMemAlloc",   o_CoTaskMemAlloc },
    { "ole32.dll!CoTaskMemFree",    o_CoTaskMemFree },
    { "ole32.dll!CoFreeUnusedLibraries", o_CoFreeUnusedLibraries },
    { "binkw32.dll!_BinkSetVolume@12", b_SetVolume12 },
    { "binkw32.dll!_BinkCopyToBufferRect@44", b_CopyToBufferRect },
    { 0, 0 }
};

void fam_register_misc(void) { win32hle_register(g_misc); }
