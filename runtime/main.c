/*
 * main.c - The Familia entry point: platform init, guest memory, module
 * mapping (dependency order), IAT binding, DllMains, then Game.exe's CRT entry.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win32hle.h"
#include "host.h"
#include "loader.h"
#include "familia.h"

static const struct { const char *file; uint32_t base; } MODULES[] = {
    { "rw_data.dll", 0x11000000u },     /* bases must match tools/translate.py */
    { "IJoy.dll",    0x11100000u },
    { "LS3DF.dll",   0x10000000u },
    { "Game.exe",    0x00400000u },
};

void recomp_unimplemented(uint32_t va, const char *insn) {
    hle_fatal("unimplemented instruction '%s' reached at 0x%08X", insn, va);
}
/* An indirect call or tail jump to an address with no translated function:
 * a function the disassembler missed. Fatal by default, naming the address
 * (add it to config/seeds/<module>.json); FAMILIA_UNRESOLVED_CONTINUE=1 logs
 * and returns instead, for diagnosis. */
void recomp_unresolved(uint32_t va, uint32_t from, const char *kind) {
    static int cont = -1;
    if (cont < 0) cont = getenv("FAMILIA_UNRESOLVED_CONTINUE") != NULL;
    fprintf(stderr, "%s: unresolved VA 0x%08X from 0x%08X\n", kind, va, from);
    if (!cont) hle_fatal("no translated function at 0x%08X (%s from 0x%08X)", va, kind, from);
}
void recomp_lift_failed(uint32_t va) { hle_fatal("function 0x%08X failed to lift", va); }

int main(int argc, char **argv) {
    const char *game = getenv("FAMILIA_GAME_DIR");
    if (!game) game = argc > 1 ? argv[1] : "data";
    int permissive = getenv("FAMILIA_PERMISSIVE") && atoi(getenv("FAMILIA_PERMISSIVE"));
    { void fam_crash_install(void); fam_crash_install(); }
    fam_paths_init(game);
    game = ".";                      /* fam_paths_init made the game dir current */
    { void fam_readahead_start(void); fam_readahead_start(); }   /* learned archive read-ahead */
    hle_mem_init();
    recomp_host_init();
    fam_register_shims();
    { void fam_watch_start(void); fam_watch_start(); }
    { void fam_prof_start(void); fam_prof_start(); }
    for (size_t i = 0; i < sizeof MODULES / sizeof *MODULES; i++)
        if (fam_map(game, MODULES[i].file, MODULES[i].base, i + 1 == sizeof MODULES / sizeof *MODULES)) return 2;
    int bad = 0;
    for (int i = 0; i < fam_module_count(); i++) bad += fam_bind(fam_module_at(i), permissive);
    if (bad && !permissive) { fprintf(stderr, "[familia] %d unresolved imports (FAMILIA_PERMISSIVE=1 to stub)\n", bad); return 3; }
    if (fam_attach_dlls()) { fprintf(stderr, "[familia] a DLL refused to attach\n"); return 4; }
    /* native replacements: after every module is mapped, so the self-check's
     * scratch memory and guest stack cannot take an image's fixed address */
    { void fam_native_init(void); fam_native_init(); }
    { void fam_probe_init(void); fam_probe_init(); }
    fam_module *exe = fam_find_module("Game.exe");
    fprintf(stderr, "[familia] entering Game.exe at 0x%08X\n", exe->entry);
    uint32_t args[1] = {0};
    return (int)hle_call_guest(exe->entry, 0, args);
}
