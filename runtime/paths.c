/*
 * paths.c - where the game's files live on the host.
 * C:\ is the game directory (the user's installed data), never written.
 * Writable state goes under FAMILIA_USER_DIR (default <game>/../userdata):
 * the registry file, and userdata/game, an overlay of the game directory:
 * every file the game writes there (savegame\*, its logs) lands in the
 * overlay, and reads look in the overlay first, so saves survive updates of
 * data/ and no symlinks are needed (the SD card is vfat).
 */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32hle.h"
#include "familia.h"
void fam_seed_setup_config(void);

static char g_user[PATH_MAX];

const char *fam_user_dir(void) { return g_user; }

/* LS3D_setup: the 63-byte REG_BINARY the original setup.exe writes and
 * Game.exe reads at 0x5BEB55 (missing -> it launches setup.exe and exits).
 * Seeded once with the engine's own defaults (the routine at 0x5BCE10),
 * except the display mode: native 640x480x32. Only the offsets below are
 * established; the remaining bytes keep the engine's default values. */
#define HKLM 0x80000002u
#define MAFIA_KEY "Software\\Illusion Softworks\\Mafia"
static void put32(uint8_t *b, int o, uint32_t v) { memcpy(b + o, &v, 4); }
/* The launcher's texture choice (FAMILIA_TEXTURES=32 or 16) applied to the
 * setup value at every start: texture_true_color is byte 0x1C. */
static void apply_texture_choice(void) {
    const char *e = getenv("FAMILIA_TEXTURES");
    if (!e) return;
    uint8_t b[0x3F];
    if (hle_reg_get_binary(HKLM, MAFIA_KEY, "LS3D_setup", b, sizeof b) != sizeof b) return;
    uint8_t want = atoi(e) == 16 ? 0 : 1;
    if (b[0x1C] == want) return;
    b[0x1C] = want;
    hle_reg_set_binary(HKLM, MAFIA_KEY, "LS3D_setup", b, sizeof b);
    fprintf(stderr, "[paths] textures: %s\n", want ? "32-bit" : "16-bit");
}
void fam_seed_setup_config(void) {
    if (hle_reg_has(HKLM, MAFIA_KEY, "LS3D_setup")) { apply_texture_choice(); return; }
    uint8_t b[0x3F] = {0};
    b[0x00] = 0x14;                          /* layout version the game checks */
    put32(b, 0x02, 640);                     /* width  (engine default 800) */
    put32(b, 0x06, 480);                     /* height (engine default 600) */
    put32(b, 0x0A, 32);                      /* bits per pixel */
    b[0x16] = 1; b[0x1A] = 1; b[0x1B] = 1; b[0x1E] = 1; b[0x1F] = 1; b[0x21] = 1; b[0x26] = 1;   /* engine defaults (cl = 1) */
    put32(b, 0x27, 0x3F800000u);             /* 1.0f */
    put32(b, 0x2B, 0x3F800000u);             /* 1.0f */
    b[0x1C] = 1;                             /* texture_true_color: keep 32-bit textures (the Mali has no DXT; the
                                              * 16-bit fallback dithers every texture on the CPU while streaming) */
    put32(b, 0x37, 0);                       /* shadows: Very Low (drawn by the CPU) */
    hle_reg_set_binary(HKLM, MAFIA_KEY, "LS3D_setup", b, sizeof b);
    fprintf(stderr, "[paths] seeded LS3D_setup (engine defaults, 640x480x32, 32-bit textures, shadows very low)\n");
    apply_texture_choice();
}

static char g_game[PATH_MAX];
const char *fam_game_dir(void) { return g_game; }
void fam_paths_init(const char *game_dir) {
    char real[PATH_MAX];
    if (!realpath(game_dir, real)) { fprintf(stderr, "[paths] game dir %s missing\n", game_dir); exit(2); }
    snprintf(g_game, sizeof g_game, "%s", real);
    const char *u = getenv("FAMILIA_USER_DIR");
    if (u) snprintf(g_user, sizeof g_user, "%s", u);
    else snprintf(g_user, sizeof g_user, "%s/../userdata", real);
    mkdir(g_user, 0755);
    hle_set_drive('C', real);
    static char ov[PATH_MAX];
    snprintf(ov, sizeof ov, "%s/game", g_user);
    mkdir(ov, 0755);
    hle_set_overlay(real, ov);
    static char reg[PATH_MAX];
    snprintf(reg, sizeof reg, "%s/registry.txt", g_user);
    hle_reg_load(reg);
    fam_seed_setup_config();
    if (chdir(real) != 0) perror("[paths] chdir");
    fprintf(stderr, "[paths] C:\\ = %s, user dir = %s\n", real, g_user);
}
