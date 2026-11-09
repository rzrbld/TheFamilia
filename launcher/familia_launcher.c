/*
 * familia_launcher - The Familia's PortMaster launcher (C + SDL2 only).
 *
 *   familia-launcher --root GAMEDIR
 *
 * First run (no data/.installed): import page. It lists what was found in
 * source/ and runs familia-import (the PortMaster 7zzs path in FAMILIA_7Z)
 * with live progress. Later runs: settings page (FPS counter, frame limit,
 * look sensitivity, CPU, textures, the controller layout), then Save and Run / Cancel.
 *
 * Writes conf/familia.cfg (shell, sourced by the Ports script). Runs before
 * gptokeyb starts, so it reads the controller itself.
 * Exit status: 0 start the game, 10 the user left, other values = error.
 *
 * Look: the game's menus - a torn paper card with ink lettering over a dark,
 * warm scene, all drawn here (no game art). 40x30 cells of 16x16 on a
 * 640x480 canvas scaled to fit any screen (fullscreen desktop; FAMILIA_WINDOWED=1: resizable window).
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <SDL2/SDL_ttf.h>
#include "font.h"

#define W 640
#define H 480
#define CELL 16
#define COLS (W / CELL)
#define ROWS (H / CELL)
enum { EXIT_RUN = 0, EXIT_USER = 10, EXIT_ERR = 1 };

/* palette: the game's menus - ink on a torn paper card over a dark, warm scene */
static const SDL_Color C_BG = { 0x12, 0x0D, 0x08, 255 }, C_FRAME = { 0x8E, 0x1B, 0x14, 255 },
    C_TEXT = { 0x1E, 0x17, 0x12, 255 }, C_DIM = { 0x6B, 0x5D, 0x4B, 255 }, C_HI_BG = { 0x1E, 0x17, 0x12, 255 },
    C_HI_FG = { 0xEC, 0xE3, 0xCC, 255 }, C_RED = { 0x9A, 0x24, 0x18, 255 }, C_OK = { 0x3D, 0x5A, 0x22, 255 },
    C_CREAM = { 0xD8, 0xC8, 0xA4, 255 }, C_CREAM_DIM = { 0x8A, 0x7A, 0x5E, 255 }, C_LOGO = { 0xB3, 0x26, 0x1E, 255 };

static SDL_Renderer *R;
static SDL_Texture *g_bg_tex, *g_card_tex;
static const char *g_root;
static char g_conf[4096], g_data[4096], g_source[4096];

/* ------------------------------------------------------------ drawing */
static const fam_glyph *glyph(unsigned char c) {
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
    for (size_t i = 0; i < sizeof k_font / sizeof *k_font; i++) if ((unsigned char)k_font[i].c == c) return &k_font[i];
    return &k_font[0];
}
/* Text: the Special Elite typewriter face (fonts/, Apache 2.0) through
 * SDL2_ttf, each glyph rendered once in white and tinted; without the font
 * file the built-in 8x8 block font. Sizes: 0 body (one 16 px cell per
 * character), 1 small, 2 the logo. */
static TTF_Font *g_font[3];
static SDL_Texture *g_gtex[3][128];
static int g_gw[3][128], g_gh[3][128], g_gadv[3][128];
static SDL_Texture *glyph_tex(int sz, unsigned char c) {
    if (c >= 128 || !g_font[sz]) return NULL;
    if (!g_gtex[sz][c]) {
        SDL_Color white = { 255, 255, 255, 255 };
        SDL_Surface *sf = TTF_RenderGlyph_Blended(g_font[sz], c, white);
        if (!sf) return NULL;
        g_gtex[sz][c] = SDL_CreateTextureFromSurface(R, sf);
        g_gw[sz][c] = sf->w; g_gh[sz][c] = sf->h;
        int minx, maxx, miny, maxy, adv; TTF_GlyphMetrics(g_font[sz], c, &minx, &maxx, &miny, &maxy, &adv); g_gadv[sz][c] = adv;
        SDL_FreeSurface(sf);
    }
    return g_gtex[sz][c];
}
static int size_of_scale(int scale) { return scale >= 3 ? 2 : scale == 1 ? 1 : 0; }
/* one glyph at pixel x, y; block font: each dot is scale x scale pixels */
static void glyph_px(int x, int y, unsigned char c, int scale, SDL_Color fg) {
    SDL_Texture *t = glyph_tex(size_of_scale(scale), c);
    if (t) {
        int sz = size_of_scale(scale);
        SDL_SetTextureColorMod(t, fg.r, fg.g, fg.b); SDL_SetTextureAlphaMod(t, fg.a);
        SDL_Rect d = { x, y, g_gw[sz][c], g_gh[sz][c] }; SDL_RenderCopy(R, t, NULL, &d);
        return;
    }
    const fam_glyph *g = glyph(c);
    SDL_SetRenderDrawColor(R, fg.r, fg.g, fg.b, fg.a);
    for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++)
        if (g->rows[yy][xx] == '#') { SDL_Rect p = { x + xx * scale, y + yy * scale, scale, scale }; SDL_RenderFillRect(R, &p); }
}
static int adv_of(unsigned char c, int scale) {
    int sz = size_of_scale(scale);
    return glyph_tex(sz, c) ? g_gadv[sz][c] : 8 * scale;
}
static int text_w(const char *s, int scale) { int w = 0; for (; *s; s++) w += adv_of((unsigned char)*s, scale); return w; }
static void text_px(int x, int y, const char *s, int scale, SDL_Color fg) {
    for (; *s; s++) { glyph_px(x, y, (unsigned char)*s, scale, fg); x += adv_of((unsigned char)*s, scale); }
}
static void text_center(int y, const char *s, int scale, SDL_Color fg) { text_px((W - text_w(s, scale)) / 2, y, s, scale, fg); }
static void put(int col, int row, unsigned char c, SDL_Color fg, const SDL_Color *bg) {
    if (bg) { SDL_SetRenderDrawColor(R, bg->r, bg->g, bg->b, 255); SDL_Rect b = { col * CELL, row * CELL - 3, CELL, CELL + 6 }; SDL_RenderFillRect(R, &b); }
    if (glyph_tex(0, c)) {                     /* centred in its cell, typewriter spacing */
        glyph_px(col * CELL + (CELL - g_gadv[0][c]) / 2, row * CELL + (CELL - g_gh[0][c]) / 2 + 1, c, 2, fg);
        return;
    }
    glyph_px(col * CELL, row * CELL, c, 2, fg);
}
static void text(int col, int row, const char *s, SDL_Color fg, const SDL_Color *bg) {
    if (g_font[0]) {                                   /* one proportional run from the cell */
        int x = col * CELL, h = g_gh[0]['A'] ? g_gh[0]['A'] : 22;
        if (bg) { SDL_SetRenderDrawColor(R, bg->r, bg->g, bg->b, 255); SDL_Rect b = { x - 6, row * CELL - 4, text_w(s, 2) + 12, CELL + 8 }; SDL_RenderFillRect(R, &b); }
        text_px(x, row * CELL + (CELL - h) / 2 + 1, s, 2, fg);
        return;
    }
    for (; *s && col < COLS; s++, col++) put(col, row, (unsigned char)*s, fg, bg);
}
static void textc(int row, const char *s, SDL_Color fg) {
    if (g_font[0]) { int h = g_gh[0]['A'] ? g_gh[0]['A'] : 22; text_center(row * CELL + (CELL - h) / 2 + 1, s, 2, fg); return; }
    text((COLS - (int)strlen(s)) / 2, row, s, fg, NULL);
}
static __attribute__((unused)) void box(int c0, int r0, int c1, int r1, SDL_Color fg) {
    SDL_SetRenderDrawColor(R, fg.r, fg.g, fg.b, 255);
    SDL_Rect b = { c0 * CELL + 4, r0 * CELL + 4, (c1 - c0) * CELL + 8, (r1 - r0) * CELL + 8 };
    SDL_RenderDrawRect(R, &b);
}

/* the scene and the card, drawn once into textures */
static uint32_t g_rng = 0x2F6E2B1u;
static float frand(void) { g_rng = g_rng * 1664525u + 1013904223u; return (float)(g_rng >> 8) / 16777216.f; }
#define CARD_X 16
#define CARD_Y 88
#define CARD_W 608
#define CARD_H 352
static SDL_Texture *make_tex(int w, int h, const uint32_t *px) {
    SDL_Texture *t = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(t, NULL, px, w * 4);
    return t;
}
static uint32_t argb(float a, float r, float g, float b) {
    #define C8(v) (uint32_t)((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    return C8(a) << 24 | C8(r) << 16 | C8(g) << 8 | C8(b);
}
static void make_art(void) {
    uint32_t *px = malloc((size_t)W * H * 4);
    /* the scene: a warm lamp-lit dark, vignetted, with film grain and a few scratches */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float dx = (x - W * 0.5f) / (W * 0.62f), dy = (y - H * 0.42f) / (H * 0.62f), d = dx * dx + dy * dy;
        float l = 1.f - (d > 1.f ? 1.f : d); l = l * l;
        float n = (frand() - 0.5f) * 10.f;
        px[y * W + x] = argb(255, 10 + 34 * l + n, 7 + 22 * l + n * 0.8f, 5 + 12 * l + n * 0.6f);
    }
    for (int k = 0; k < 7; k++) {
        int x = (int)(frand() * W), len = 60 + (int)(frand() * 300), y0 = (int)(frand() * H);
        for (int y = y0; y < y0 + len && y < H; y++) { uint32_t *p = &px[y * W + x]; *p = argb(255, ((*p >> 16) & 255) + 14, ((*p >> 8) & 255) + 11, (*p & 255) + 8); }
    }
    g_bg_tex = make_tex(W, H, px);
    free(px);
    /* the card: off-white paper, fibres and speckles, darker toward the torn edges, a soft shadow */
    int cw = CARD_W + 16, ch = CARD_H + 16;
    px = calloc((size_t)cw * ch, 4);
    int left[CARD_H], right[CARD_H], top[CARD_W], bottom[CARD_W];
    float e = 0;
    for (int y = 0; y < CARD_H; y++) { e += (frand() - 0.5f) * 1.6f; e *= 0.85f; left[y] = 2 + (int)(e + frand() * 2.5f); }
    for (int y = 0; y < CARD_H; y++) { e += (frand() - 0.5f) * 1.6f; e *= 0.85f; right[y] = 2 + (int)(e + frand() * 2.5f); }
    for (int x = 0; x < CARD_W; x++) { e += (frand() - 0.5f) * 1.6f; e *= 0.85f; top[x] = 2 + (int)(e + frand() * 3.f); }
    for (int x = 0; x < CARD_W; x++) { e += (frand() - 0.5f) * 1.6f; e *= 0.85f; bottom[x] = 2 + (int)(e + frand() * 3.f); }
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {          /* shadow, offset 6 px */
        int sx = x - 6 - 8, sy = y - 6 - 8;
        if (sx >= 0 && sx < CARD_W && sy >= 0 && sy < CARD_H) px[y * cw + x] = argb(110, 0, 0, 0);
    }
    for (int y = 0; y < CARD_H; y++) for (int x = 0; x < CARD_W; x++) {
        if (x < left[y] || x >= CARD_W - right[y] || y < top[x] || y >= CARD_H - bottom[x]) continue;
        float ex = x < CARD_W / 2 ? x : CARD_W - 1 - x, ey = y < CARD_H / 2 ? y : CARD_H - 1 - y;
        float edge = ex < ey ? ex : ey, burn = edge < 26 ? (26 - edge) / 26.f : 0;
        float n = (frand() - 0.5f) * 9.f, fibre = (frand() < 0.004f) ? -22.f : 0.f;
        float base = 228 - burn * burn * 46 + n + fibre;
        px[(y + 8) * cw + x + 8] = argb(255, base + 8, base + 1, base - 22 - burn * 14);
    }
    for (int k = 0; k < 140; k++) {                                     /* specks and creases */
        int x = 12 + (int)(frand() * (CARD_W - 24)), y = 12 + (int)(frand() * (CARD_H - 24));
        px[(y + 8) * cw + x + 8] = argb(255, 150, 138, 112);
    }
    for (int x = 20; x < CARD_W - 20; x++) {                             /* a faint fold across the card */
        int y = CARD_H * 2 / 3 + (int)((x - CARD_W / 2) * 0.03f);
        uint32_t *p = &px[(y + 8) * cw + x + 8]; if (*p >> 24) *p = argb(255, ((*p >> 16) & 255) - 10, ((*p >> 8) & 255) - 10, (*p & 255) - 10);
    }
    g_card_tex = make_tex(cw, ch, px);
    free(px);
}
static void frame_begin(const char *title) {
    SDL_SetRenderDrawColor(R, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(R);
    if (g_bg_tex) SDL_RenderCopy(R, g_bg_tex, NULL, NULL);
    /* the logo: THE FAMILIA in the game's red, with a shadow */
    const char *logo = "THE FAMILIA";
    int lx = (W - text_w(logo, 4)) / 2;
    SDL_Color sh = { 0, 0, 0, 200 };
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    text_px(lx + 3, g_font[2] ? 6 : 15, logo, 4, sh);
    text_px(lx, g_font[2] ? 3 : 12, logo, 4, C_LOGO);
    text_center(g_font[2] ? 58 : 54, "Mafia  -  The City of Lost Heaven", 1, C_CREAM);
    if (g_card_tex) { SDL_Rect d = { CARD_X - 8, CARD_Y - 8, CARD_W + 16, CARD_H + 16 }; SDL_RenderCopy(R, g_card_tex, NULL, &d); }
    if (title) {
        textc(6, title, C_TEXT);
        int w = (int)strlen(title) * CELL; SDL_SetRenderDrawColor(R, C_FRAME.r, C_FRAME.g, C_FRAME.b, 255);
        SDL_Rect u = { (W - w) / 2, 6 * CELL + 18, w, 2 }; SDL_RenderFillRect(R, &u);
    }
}
/* development: FAMILIA_LAUNCHER_SHOT=file.bmp saves the first frame of the page shown, then exits */
static void present(void) {
    static int n; const char *shot = getenv("FAMILIA_LAUNCHER_SHOT");
    if (shot && ++n == 3) {
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
        SDL_SaveBMP(sf, shot); exit(0);
    }
    SDL_RenderPresent(R);
}
static void hint(const char *s) { text_center(H - 30, s, 1, C_CREAM_DIM); }
/* small text at a pixel position (lists, paragraphs) */
static void small(int x, int y, const char *s, SDL_Color fg) { text_px(x, y, s, 1, fg); }
/* a helper line inside the card, in the small size */
static void note(int row, const char *s, SDL_Color fg) { text_center(row * CELL + 2, s, 1, fg); }

/* ------------------------------------------------------------- input */
enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_QUIT };
static SDL_GameController *g_pad;
static int poll_key(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: return K_QUIT;
        case SDL_CONTROLLERDEVICEADDED: if (!g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which); break;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN; case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT;
            case SDLK_RETURN: case SDLK_SPACE: return K_A; case SDLK_ESCAPE: case SDLK_BACKSPACE: return K_B;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UP; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFT; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHT;
            case SDL_CONTROLLER_BUTTON_A: return K_A; case SDL_CONTROLLER_BUTTON_B: return K_B;
            }
            break;
        }
    }
    return K_NONE;
}

/* ---------------------------------------------------------- settings */
static int g_fps = 1, g_limit = 0, g_sens = 30, g_cpu_perf = 1, g_tex32 = 1;

static void cfg_load(void) {
    char p[4200]; snprintf(p, sizeof p, "%s/familia.cfg", g_conf);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "FAMILIA_FPS=%d", &v) == 1) g_fps = v;
        else if (sscanf(line, "FAMILIA_LIMIT=%d", &v) == 1) g_limit = v;
        else if (sscanf(line, "FAMILIA_LOOK_SENS=%d", &v) == 1) g_sens = v;
        else if (!strncmp(line, "FAMILIA_CPU=", 12)) g_cpu_perf = strncmp(line + 12, "system", 6) != 0;
        else if (sscanf(line, "FAMILIA_TEXTURES=%d", &v) == 1) g_tex32 = v != 16;
    }
    fclose(f);
}
static int cfg_save(void) {
    char p[4200], t[4300];
    snprintf(p, sizeof p, "%s/familia.cfg", g_conf); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w"); if (!f) return -1;
    fprintf(f, "# The Familia settings (written by the launcher)\nFAMILIA_FPS=%d\nFAMILIA_LIMIT=%d\nFAMILIA_LOOK_SENS=%d\nFAMILIA_CPU=%s\nFAMILIA_TEXTURES=%d\n",
            g_fps, g_limit, g_sens, g_cpu_perf ? "perf" : "system", g_tex32 ? 32 : 16);
    if (fclose(f) || rename(t, p)) return -1;
    snprintf(p, sizeof p, "%s/familia.gptk", g_conf);   /* earlier versions' generated keyboard mapping */
    unlink(p);
    return 0;
}

/* The controller layout. The game reads the pad itself (on foot and in the
 * car separately); rebinding happens in the game's Options > Controls. */
static int page_controls(void) {
    static const char *foot[][2] = {
        { "LEFT STICK", "Move" }, { "RIGHT STICK", "Look, aim" }, { "A", "Confirm in menus" }, { "B", "Reload" },
        { "X", "Jump" }, { "Y", "Use, get in a car" }, { "LB", "Inventory" }, { "RB", "Crouch" },
        { "LT", "Sniper mode" }, { "RT", "Fire" }, { "L3", "Run / walk" }, { "R3", "Objectives" },
        { "D-PAD", "Weapons: next / previous, holster / drop" }, { "SELECT", "City map" }, { "START", "Pause menu" } };
    static const char *car[][2] = {
        { "LEFT STICK", "Steer" }, { "RIGHT STICK", "Look around" }, { "RT", "Accelerate" }, { "LT", "Brake, reverse" },
        { "X", "Handbrake" }, { "Y", "Get out" }, { "B", "Speed limiter" }, { "LB / RB", "Look left / right" },
        { "L3", "Horn" }, { "R3", "Camera" }, { "D-PAD L / R", "Gear down / up" }, { "D-PAD DOWN", "Manual / automatic gears" },
        { "SELECT", "City map" }, { "START", "Pause menu" } };
    int page = 0;
    for (;;) {
        frame_begin(page ? "CONTROLS: IN THE CAR" : "CONTROLS: ON FOOT");
        int n = page ? (int)(sizeof car / sizeof *car) : (int)(sizeof foot / sizeof *foot);
        for (int i = 0; i < n; i++) {
            const char *b = page ? car[i][0] : foot[i][0], *a = page ? car[i][1] : foot[i][1];
            small(76, 128 + i * 18, b, C_FRAME); small(220, 128 + i * 18, a, C_TEXT);
        }
        note(26, "Rebind in the game: Options > Controls (player and car pages)", C_DIM);
        hint(page ? "Left/Right: on foot    B back" : "Left/Right: in the car    B back");
        present();
        int k = poll_key();
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_LEFT || k == K_RIGHT) page = !page;
        if (k == K_B || k == K_A) return -1;
        SDL_Delay(16);
    }
}

static int page_settings(void) {
    enum { S_FPS, S_LIMIT, S_LOOK, S_CPU, S_TEX, S_BIND, S_RUN, S_CANCEL, S_N };
    int sel = S_RUN;
    for (;;) {
        frame_begin("SETTINGS");
        char v[S_N][32];
        snprintf(v[S_FPS], 32, "%s", g_fps ? "On" : "Off");
        snprintf(v[S_LIMIT], 32, "%s", g_limit == 30 ? "30 FPS" : g_limit == 60 ? "60 FPS" : "Off");
        snprintf(v[S_LOOK], 32, "%d %%", g_sens);
        snprintf(v[S_CPU], 32, "%s", g_cpu_perf ? "1.5 GHz" : "System");
        snprintf(v[S_TEX], 32, "%s", g_tex32 ? "32-bit" : "16-bit");
        v[S_BIND][0] = v[S_RUN][0] = v[S_CANCEL][0] = 0;
        static const char *lab[S_N] = { "FPS counter", "Frame limit", "Look sensitivity", "CPU", "Textures", "Controls...", "Save and run", "Cancel" };
        for (int i = 0; i < S_N; i++) {
            int row = 8 + i * 2 + (i >= S_RUN ? 1 : 0);
            SDL_Color fg = i == sel ? C_HI_FG : C_TEXT;
            if (i == sel) { SDL_SetRenderDrawColor(R, C_HI_BG.r, C_HI_BG.g, C_HI_BG.b, 255); SDL_Rect b = { 60, row * CELL - 5, W - 120, CELL + 10 }; SDL_RenderFillRect(R, &b); }
            int h = g_gh[0]['A'] ? g_gh[0]['A'] : 16, y = row * CELL + (CELL - h) / 2 + 1;
            text_px(84, y, lab[i], 2, fg);
            if (v[i][0]) {
                SDL_Color ar = i == sel ? C_HI_FG : C_FRAME;
                text_px(370, y, "<", 2, ar); text_px(540, y, ">", 2, ar);
                text_px(455 - text_w(v[i], 2) / 2, y, v[i], 2, fg);
            }
        }
        note(26, sel == S_TEX ? (g_tex32 ? "32-bit textures: smoother streaming, about +150 MB of RAM" : "16-bit textures: less RAM, more hitches while streaming")
                 : sel == S_LOOK ? "Right stick speed (default 30 %). Fine-tune X and Y in the game: Options > Controls"
                 : sel == S_CPU ? "1.5 GHz: performance governor, restored on exit"
                 : sel == S_BIND ? "The controller layout; rebind in the game's Options > Controls"
                 : sel == S_LIMIT ? "Frame limit: off lets the game run as fast as it can"
                                  : "Rendering 640x480, scaled to the screen", C_DIM);
        hint("Up/Down select    Left/Right change    A confirm    B cancel");
        present();
        int k = poll_key();
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_UP) sel = (sel + S_N - 1) % S_N;
        if (k == K_DOWN) sel = (sel + 1) % S_N;
        if (k == K_LEFT || k == K_RIGHT) {
            int d = k == K_RIGHT ? 1 : -1;
            if (sel == S_FPS) g_fps = !g_fps;
            if (sel == S_LIMIT) { static const int L[3] = { 0, 30, 60 }; int i = g_limit == 30 ? 1 : g_limit == 60 ? 2 : 0; g_limit = L[(i + 3 + d) % 3]; }
            if (sel == S_LOOK) { g_sens += d * 5; if (g_sens < 5) g_sens = 5; if (g_sens > 200) g_sens = 200; }
            if (sel == S_CPU) g_cpu_perf = !g_cpu_perf;
            if (sel == S_TEX) g_tex32 = !g_tex32;
        }
        if (k == K_A) {
            if (sel == S_BIND) { int r = page_controls(); if (r >= 0) return r; }
            else if (sel == S_RUN) return cfg_save() == 0 ? EXIT_RUN : EXIT_ERR;
            else if (sel == S_CANCEL) return EXIT_USER;
        }
        if (k == K_B) return EXIT_USER;
        SDL_Delay(16);
    }
}

/* ------------------------------------------------------------ import */
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }
typedef struct { int discs, patch, exe; } found;
static found scan_source(void) {
    found f = {0};
    DIR *d = opendir(g_source); if (!d) return f;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (ends_with(e->d_name, ".bin") || ends_with(e->d_name, ".iso") || ends_with(e->d_name, ".img")) f.discs++;
        else if (ends_with(e->d_name, ".exe") && strcasestr(e->d_name, "patch")) f.patch = 1;
        else if (ends_with(e->d_name, ".exe") || ends_with(e->d_name, ".rar") || ends_with(e->d_name, ".zip") || ends_with(e->d_name, ".7z")) f.exe = 1;
    }
    closedir(d);
    return f;
}
#define LOGN 9
static char g_log[LOGN][64];
static void log_push(const char *s) {
    memmove(g_log[0], g_log[1], sizeof g_log[0] * (LOGN - 1));
    snprintf(g_log[LOGN - 1], sizeof g_log[0], "%s", strncmp(s, "[import] ", 9) ? s : s + 9);
}
static int run_import(void) {
    char tool[4200], seven[4200];
    snprintf(tool, sizeof tool, "%s/familia-import.aarch64", g_root);
    const char *z = getenv("FAMILIA_7Z");
    snprintf(seven, sizeof seven, "%s", z ? z : "7zzs.aarch64");
    int pfd[2]; if (pipe(pfd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1); dup2(pfd[1], 2); close(pfd[0]);
        execl(tool, tool, "--source", g_source, "--dest", g_data, "--7z", seven, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    memset(g_log, 0, sizeof g_log);
    Uint32 t0 = SDL_GetTicks();
    char buf[512]; size_t bl = 0; int status = -1;
    for (;;) {
        ssize_t r = read(pfd[0], buf + bl, sizeof buf - 1 - bl);
        if (r > 0) {
            bl += (size_t)r; buf[bl] = 0;
            char *nl;
            while ((nl = strchr(buf, '\n'))) { *nl = 0; printf("%s\n", buf); log_push(buf); memmove(buf, nl + 1, strlen(nl + 1) + 1); bl = strlen(buf); }
            if (bl == sizeof buf - 1) { log_push(buf); bl = 0; }
        }
        int st; pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) { status = WIFEXITED(st) ? WEXITSTATUS(st) : -1; if (r <= 0) break; }
        Uint32 s = (SDL_GetTicks() - t0) / 1000;
        frame_begin("IMPORTING YOUR GAME FILES");
        char el[40]; snprintf(el, sizeof el, "Elapsed %02u:%02u", s / 60, s % 60);
        textc(8, el, C_FRAME);
        put(COLS / 2, 10, (unsigned char)"-/|*"[(SDL_GetTicks() / 200) % 4], C_FRAME, NULL);
        for (int i = 0; i < LOGN; i++) small(52, 196 + i * 19, g_log[i], i == LOGN - 1 ? C_TEXT : C_DIM);
        hint("Please keep the handheld powered");
        present();
        SDL_Event e; while (SDL_PollEvent(&e)) {}
        if (r <= 0) SDL_Delay(50);
    }
    close(pfd[0]);
    return status;
}
static int page_import(void) {
    for (;;) {
        found f = scan_source();
        frame_begin("FIRST START: IMPORT YOUR GAME");
        small(52, 128, "Put your own game files in ports/the_familia/source/ (see README):", C_TEXT);
        char l[40];
        snprintf(l, sizeof l, "%s Disc images (3 CDs): %d found", f.discs >= 3 ? "[x]" : "[ ]", f.discs);
        small(72, 162, l, f.discs >= 3 ? C_OK : C_RED);
        small(72, 182, f.patch ? "[x] 1.2 patch" : "[ ] 1.2 patch (mafia_patch_1.2_eng.exe)", f.patch ? C_OK : C_RED);
        small(72, 202, f.exe ? "[x] Unprotected 1.2 Game.exe (loose or in .zip / .rar / .7z)" : "[ ] Unprotected 1.2 Game.exe (loose or in .zip / .rar / .7z)", f.exe ? C_OK : C_RED);
        int ready = f.discs >= 1 && f.patch && f.exe;
        small(52, 240, "The import takes several minutes and needs about 2 GB of free space.", C_DIM);
        text(4, 19, ready ? "A   Import game files" : "A   Look again", C_HI_FG, &C_HI_BG);
        text(4, 21, "B   Exit", C_TEXT, NULL);
        hint("A continue    B exit");
        present();
        int k = poll_key();
        if (k == K_QUIT || k == K_B) return EXIT_USER;
        if (k == K_A && ready) {
            int rc = run_import();
            if (rc == 0) {
                char m[4200]; snprintf(m, sizeof m, "%s/.installed", g_data);
                FILE *fm = fopen(m, "w"); if (fm) fclose(fm);
                return -1;   /* continue to settings */
            }
            for (;;) {      /* error page */
                frame_begin("IMPORT FAILED");
                for (int i = 0; i < LOGN; i++) small(52, 132 + i * 19, g_log[i], i == LOGN - 1 ? C_RED : C_DIM);
                small(52, 312, "Full log: ports/the_familia/log.txt", C_DIM);
                text(4, 22, "A   Try again", C_HI_FG, &C_HI_BG); text(4, 24, "B   Exit", C_TEXT, NULL);
                hint("A retry    B exit");
                present();
                int k2 = poll_key();
                if (k2 == K_A) break;
                if (k2 == K_B || k2 == K_QUIT) return EXIT_USER;
                SDL_Delay(16);
            }
        }
        SDL_Delay(16);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--root")) g_root = argv[i + 1];
    if (!g_root) { fprintf(stderr, "usage: familia-launcher --root GAMEDIR\n"); return EXIT_ERR; }
    snprintf(g_conf, sizeof g_conf, "%s/conf", g_root);
    snprintf(g_data, sizeof g_data, "%s/data", g_root);
    snprintf(g_source, sizeof g_source, "%s/source", g_root);
    mkdir(g_conf, 0755); mkdir(g_source, 0755);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return EXIT_ERR; }
    uint32_t fl = SDL_WINDOW_RESIZABLE | (getenv("FAMILIA_WINDOWED") ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Window *win = SDL_CreateWindow("The Familia", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, fl);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return EXIT_ERR; }
    R = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!R) R = SDL_CreateRenderer(win, -1, 0);
    if (!R) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return EXIT_ERR; }
    SDL_RenderSetLogicalSize(R, W, H);     /* 4:3, scaled to any screen, letterboxed */
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    if (TTF_Init() == 0) {
        char fp[4200]; snprintf(fp, sizeof fp, "%s/fonts/SpecialElite-Regular.ttf", g_root);
        static const int pt[3] = { 19, 14, 46 };
        for (int i = 0; i < 3; i++) g_font[i] = TTF_OpenFont(fp, pt[i]);
        if (!g_font[0]) fprintf(stderr, "[launcher] %s: %s (block font)\n", fp, TTF_GetError());
        else glyph_tex(0, 'A');
    }
    make_art();
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++) if (SDL_IsGameController(i)) g_pad = SDL_GameControllerOpen(i);
    cfg_load();
    int rc = -1;
    char marker[4200]; snprintf(marker, sizeof marker, "%s/.installed", g_data);
    const char *pg = getenv("FAMILIA_LAUNCHER_PAGE");   /* development: open a page directly */
    if (pg && !strcmp(pg, "controls")) rc = page_controls();
    else if (pg && !strcmp(pg, "import")) rc = page_import();
    else if (access(marker, F_OK) != 0) rc = page_import();
    else if (getenv("FAMILIA_AUTOSTART")) rc = cfg_save() == 0 ? EXIT_RUN : EXIT_ERR;   /* automated tests */
    if (rc < 0) rc = page_settings();
    SDL_DestroyRenderer(R); SDL_DestroyWindow(win); SDL_Quit();
    return rc;
}
