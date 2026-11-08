/*
 * familia_launcher - The Familia's PortMaster launcher (C + SDL2 only).
 *
 *   familia-launcher --root GAMEDIR
 *
 * First run (no data/.installed): import page. It lists what was found in
 * source/ and runs familia-import (the PortMaster 7zzs path in FAMILIA_7Z)
 * with live progress. Later runs: settings page (FPS counter, frame limit,
 * look speed, controller bindings), then Save and Run / Cancel.
 *
 * Writes conf/familia.cfg (shell, sourced by the Ports script) and
 * conf/familia.gptk (gptokeyb mapping). Runs before gptokeyb starts, so it
 * reads the controller itself.
 * Exit status: 0 start the game, 10 the user left, other values = error.
 *
 * Look: DOS text mode, 40x30 cells of 16x16 on a 640x480 canvas scaled to
 * fit any screen (fullscreen desktop; FAMILIA_WINDOWED=1: resizable window).
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
#include "font.h"

#define W 640
#define H 480
#define CELL 16
#define COLS (W / CELL)
#define ROWS (H / CELL)
enum { EXIT_RUN = 0, EXIT_USER = 10, EXIT_ERR = 1 };

/* palette: 1930s sepia on near-black */
static const SDL_Color C_BG = { 0x12, 0x0D, 0x08, 255 }, C_FRAME = { 0xC8, 0x8A, 0x32, 255 },
    C_TEXT = { 0xE8, 0xD8, 0xB0, 255 }, C_DIM = { 0x86, 0x74, 0x58, 255 }, C_HI_BG = { 0xC8, 0x8A, 0x32, 255 },
    C_HI_FG = { 0x12, 0x0D, 0x08, 255 }, C_RED = { 0xC8, 0x40, 0x30, 255 }, C_OK = { 0x9C, 0xB8, 0x6C, 255 };

static SDL_Renderer *R;
static const char *g_root;
static char g_conf[4096], g_data[4096], g_source[4096];

/* ------------------------------------------------------------ drawing */
static const fam_glyph *glyph(unsigned char c) {
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
    for (size_t i = 0; i < sizeof k_font / sizeof *k_font; i++) if ((unsigned char)k_font[i].c == c) return &k_font[i];
    return &k_font[0];
}
static void put(int col, int row, unsigned char c, SDL_Color fg, const SDL_Color *bg) {
    if (bg) { SDL_SetRenderDrawColor(R, bg->r, bg->g, bg->b, 255); SDL_Rect b = { col * CELL, row * CELL, CELL, CELL }; SDL_RenderFillRect(R, &b); }
    const fam_glyph *g = glyph(c);
    SDL_SetRenderDrawColor(R, fg.r, fg.g, fg.b, 255);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        if (g->rows[y][x] == '#') { SDL_Rect p = { col * CELL + x * 2, row * CELL + y * 2, 2, 2 }; SDL_RenderFillRect(R, &p); }
}
static void text(int col, int row, const char *s, SDL_Color fg, const SDL_Color *bg) {
    for (; *s && col < COLS; s++, col++) put(col, row, (unsigned char)*s, fg, bg);
}
static void textc(int row, const char *s, SDL_Color fg) { text((COLS - (int)strlen(s)) / 2, row, s, fg, NULL); }
static void box(int c0, int r0, int c1, int r1, SDL_Color fg) {
    put(c0, r0, 1, fg, NULL); put(c1, r0, 2, fg, NULL); put(c0, r1, 3, fg, NULL); put(c1, r1, 4, fg, NULL);
    for (int c = c0 + 1; c < c1; c++) { put(c, r0, 5, fg, NULL); put(c, r1, 5, fg, NULL); }
    for (int r = r0 + 1; r < r1; r++) { put(c0, r, 6, fg, NULL); put(c1, r, 6, fg, NULL); }
}
static void frame_begin(const char *title) {
    SDL_SetRenderDrawColor(R, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(R);
    box(0, 0, COLS - 1, ROWS - 1, C_FRAME);
    textc(1, "THE FAMILIA", C_FRAME);
    textc(2, "MAFIA - THE CITY OF LOST HEAVEN", C_DIM);
    for (int c = 1; c < COLS - 1; c++) put(c, 3, 5, C_DIM, NULL);
    if (title) textc(5, title, C_TEXT);
}
static void hint(const char *s) { for (int c = 1; c < COLS - 1; c++) put(c, ROWS - 3, 5, C_DIM, NULL); textc(ROWS - 2, s, C_DIM); }

/* ------------------------------------------------------------- input */
enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_QUIT };
static SDL_GameController *g_pad;
static int g_capture = 0;             /* bindings page: next pad button is reported raw */
static int g_captured = -1;           /* SDL button / 100+axis for triggers */
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
        case SDL_CONTROLLERAXISMOTION:
            if (g_capture && (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) && e.caxis.value > 20000) {
                g_captured = 100 + e.caxis.axis; return K_NONE;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (g_capture) { g_captured = e.cbutton.button; return K_NONE; }
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
typedef struct { const char *label, *key; char button[16]; const char *def; } action;
static action g_act[] = {     /* key names as gptokeyb understands them */
    { "PAUSE MENU",         "esc",         "", "start" },
    { "MAP",                "tab",         "", "back" },
    { "USE / ENTER",        "enter",       "", "a" },
    { "JUMP / HANDBRAKE",   "space",       "", "b" },
    { "RELOAD",             "r",           "", "x" },
    { "CROUCH",             "c",           "", "y" },
    { "PREVIOUS WEAPON",    "q",           "", "l1" },
    { "NEXT WEAPON",        "e",           "", "r1" },
    { "RUN / WALK",         "leftshift",   "", "l3" },
    { "HORN",               "h",           "", "r3" },
    { "FIRE",               "mouse_left",  "", "r2" },
    { "AIM",                "mouse_right", "", "l2" },
};
#define NACT (int)(sizeof g_act / sizeof *g_act)
static int g_fps = 1, g_limit = 0, g_look = 100, g_cpu_perf = 1, g_tex32 = 1;

static void cfg_load(void) {
    for (int i = 0; i < NACT; i++) snprintf(g_act[i].button, sizeof g_act[i].button, "%s", g_act[i].def);
    char p[4200]; snprintf(p, sizeof p, "%s/familia.cfg", g_conf);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int v; char k[64], b[16];
        if (sscanf(line, "FAMILIA_FPS=%d", &v) == 1) g_fps = v;
        else if (sscanf(line, "FAMILIA_LIMIT=%d", &v) == 1) g_limit = v;
        else if (sscanf(line, "FAMILIA_LOOK=%d", &v) == 1) g_look = v;
        else if (!strncmp(line, "FAMILIA_CPU=", 12)) g_cpu_perf = strncmp(line + 12, "system", 6) != 0;
        else if (sscanf(line, "FAMILIA_TEXTURES=%d", &v) == 1) g_tex32 = v != 16;
        else if (sscanf(line, "# bind %63s %15s", k, b) == 2)
            for (int i = 0; i < NACT; i++) if (!strcmp(g_act[i].key, k)) snprintf(g_act[i].button, sizeof g_act[i].button, "%s", b);
    }
    fclose(f);
}
static int cfg_save(void) {
    char p[4200], t[4300];
    snprintf(p, sizeof p, "%s/familia.cfg", g_conf); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w"); if (!f) return -1;
    fprintf(f, "# The Familia settings (written by the launcher)\nFAMILIA_FPS=%d\nFAMILIA_LIMIT=%d\nFAMILIA_LOOK=%d\nFAMILIA_CPU=%s\nFAMILIA_TEXTURES=%d\n",
            g_fps, g_limit, g_look, g_cpu_perf ? "perf" : "system", g_tex32 ? 32 : 16);
    for (int i = 0; i < NACT; i++) fprintf(f, "# bind %s %s\n", g_act[i].key, g_act[i].button);
    if (fclose(f) || rename(t, p)) return -1;
    /* gptokeyb mapping */
    snprintf(p, sizeof p, "%s/familia.gptk", g_conf); snprintf(t, sizeof t, "%s.new", p);
    f = fopen(t, "w"); if (!f) return -1;
    fprintf(f, "# generated by the launcher from conf/familia.cfg\n"
               "up = up\ndown = down\nleft = left\nright = right\n"
               "left_analog_up = up\nleft_analog_down = down\nleft_analog_left = left\nleft_analog_right = right\n"
               "right_analog_up = mouse_movement_up\nright_analog_down = mouse_movement_down\n"
               "right_analog_left = mouse_movement_left\nright_analog_right = mouse_movement_right\n");
    /* gptokeyb's mouse_scale: smaller moves faster; 512 is the speed tuned as 100 % */
    fprintf(f, "mouse_scale = %d\n", 512 * 100 / (g_look > 0 ? g_look : 100));
    for (int i = 0; i < NACT; i++) if (g_act[i].button[0]) fprintf(f, "%s = %s\n", g_act[i].button, g_act[i].key);
    if (fclose(f) || rename(t, p)) return -1;
    return 0;
}
static const char *button_name(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "a"; case SDL_CONTROLLER_BUTTON_B: return "b"; case SDL_CONTROLLER_BUTTON_X: return "x";
    case SDL_CONTROLLER_BUTTON_Y: return "y"; case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "l1"; case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "r1";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "l3"; case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "r3";
    case SDL_CONTROLLER_BUTTON_START: return "start"; case SDL_CONTROLLER_BUTTON_BACK: return "back";
    case 100 + SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "l2"; case 100 + SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "r2";
    }
    return NULL;   /* d-pad and guide stay fixed */
}
static void upper(char *o, const char *s, size_t n) { size_t i = 0; for (; s[i] && i + 1 < n; i++) o[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]); o[i] = 0; }

static int page_bindings(void) {
    int sel = 0;
    for (;;) {
        frame_begin("CONTROLLER BINDINGS");
        text(3, 7, "D-PAD / LEFT STICK  MOVE, STEER", C_DIM, NULL);
        text(3, 8, "RIGHT STICK         LOOK, AIM", C_DIM, NULL);
        for (int i = 0; i < NACT; i++) {
            char b[16]; upper(b, g_act[i].button, sizeof b);
            const SDL_Color *bg = i == sel ? &C_HI_BG : NULL; SDL_Color fg = i == sel ? C_HI_FG : C_TEXT;
            char line[40]; snprintf(line, sizeof line, " %-20s %6s ", g_act[i].label, b);
            text(5, 10 + i, line, fg, bg);
        }
        if (g_capture) textc(24, "PRESS A BUTTON FOR THIS ACTION", C_FRAME);
        hint(g_capture ? "WAITING FOR A BUTTON..." : "A REBIND   B BACK   L/R RESET");
        SDL_RenderPresent(R);
        int k = poll_key();
        if (g_capture && g_captured >= 0) {
            const char *nm = button_name(g_captured);
            g_capture = 0; g_captured = -1;
            if (nm) {
                for (int i = 0; i < NACT; i++) if (i != sel && !strcmp(g_act[i].button, nm))   /* swap a clash */
                    snprintf(g_act[i].button, sizeof g_act[i].button, "%s", g_act[sel].button);
                snprintf(g_act[sel].button, sizeof g_act[sel].button, "%s", nm);
            }
            SDL_Delay(200);
            continue;
        }
        if (g_capture) { SDL_Delay(16); continue; }
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_UP) sel = (sel + NACT - 1) % NACT;
        if (k == K_DOWN) sel = (sel + 1) % NACT;
        if (k == K_A) { g_capture = 1; g_captured = -1; SDL_Delay(150); }
        if (k == K_LEFT || k == K_RIGHT) snprintf(g_act[sel].button, sizeof g_act[sel].button, "%s", g_act[sel].def);
        if (k == K_B) return -1;
        SDL_Delay(16);
    }
}

static int page_settings(void) {
    enum { S_FPS, S_LIMIT, S_LOOK, S_CPU, S_TEX, S_BIND, S_RUN, S_CANCEL, S_N };
    int sel = S_RUN;
    for (;;) {
        frame_begin("SETTINGS");
        char v[S_N][32];
        snprintf(v[S_FPS], 32, "%s", g_fps ? "ON" : "OFF");
        snprintf(v[S_LIMIT], 32, "%s", g_limit == 30 ? "30 FPS" : g_limit == 60 ? "60 FPS" : "OFF");
        snprintf(v[S_LOOK], 32, "%d %%", g_look);
        snprintf(v[S_CPU], 32, "%s", g_cpu_perf ? "1.5 GHZ" : "SYSTEM");
        snprintf(v[S_TEX], 32, "%s", g_tex32 ? "32-BIT" : "16-BIT");
        v[S_BIND][0] = v[S_RUN][0] = v[S_CANCEL][0] = 0;
        static const char *lab[S_N] = { "FPS COUNTER", "FRAME LIMIT", "LOOK SPEED", "CPU", "TEXTURES", "CONTROLLER BINDINGS...", "SAVE AND RUN", "CANCEL" };
        for (int i = 0; i < S_N; i++) {
            int row = 9 + i * 2 + (i >= S_RUN ? 1 : 0);
            const SDL_Color *bg = i == sel ? &C_HI_BG : NULL; SDL_Color fg = i == sel ? C_HI_FG : C_TEXT;
            char line[40];
            if (v[i][0]) snprintf(line, sizeof line, " %-16s < %-8s > ", lab[i], v[i]);
            else snprintf(line, sizeof line, " %-30s ", lab[i]);
            text(4, row, line, fg, bg);
        }
        textc(26, sel == S_TEX ? (g_tex32 ? "32-BIT: SMOOTHER, ABOUT +150 MB RAM" : "16-BIT: LESS RAM, MORE HITCHES")
                                : "RENDERING 640X480, SCALED TO THE SCREEN", C_DIM);
        hint("UP/DOWN SELECT  LEFT/RIGHT CHANGE  A OK  B CANCEL");
        SDL_RenderPresent(R);
        int k = poll_key();
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_UP) sel = (sel + S_N - 1) % S_N;
        if (k == K_DOWN) sel = (sel + 1) % S_N;
        if (k == K_LEFT || k == K_RIGHT) {
            int d = k == K_RIGHT ? 1 : -1;
            if (sel == S_FPS) g_fps = !g_fps;
            if (sel == S_LIMIT) { static const int L[3] = { 0, 30, 60 }; int i = g_limit == 30 ? 1 : g_limit == 60 ? 2 : 0; g_limit = L[(i + 3 + d) % 3]; }
            if (sel == S_LOOK) { g_look += d * 10; if (g_look < 30) g_look = 30; if (g_look > 300) g_look = 300; }
            if (sel == S_CPU) g_cpu_perf = !g_cpu_perf;
            if (sel == S_TEX) g_tex32 = !g_tex32;
        }
        if (k == K_A) {
            if (sel == S_BIND) { int r = page_bindings(); if (r >= 0) return r; }
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
        char el[40]; snprintf(el, sizeof el, "ELAPSED %02u:%02u", s / 60, s % 60);
        textc(7, el, C_FRAME);
        put(COLS / 2, 9, (unsigned char)"-/|*"[(SDL_GetTicks() / 200) % 4], C_FRAME, NULL);
        for (int i = 0; i < LOGN; i++) text(2, 12 + i, g_log[i], i == LOGN - 1 ? C_TEXT : C_DIM, NULL);
        hint("PLEASE KEEP THE HANDHELD POWERED");
        SDL_RenderPresent(R);
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
        text(2, 7, "PUT YOUR OWN FILES IN PORTS/THE_FAMILIA/", C_DIM, NULL);
        text(2, 8, "SOURCE/ (SEE README):", C_DIM, NULL);
        char l[40];
        snprintf(l, sizeof l, "%s DISC IMAGES (3 CDS)  %d FOUND", f.discs >= 3 ? "[X]" : "[ ]", f.discs);
        text(3, 10, l, f.discs >= 3 ? C_OK : C_RED, NULL);
        text(3, 11, f.patch ? "[X] 1.2 PATCH" : "[ ] 1.2 PATCH (MAFIA_PATCH_1.2_ENG.EXE)", f.patch ? C_OK : C_RED, NULL);
        text(3, 12, f.exe ? "[X] UNPROTECTED 1.2 GAME.EXE" : "[ ] UNPROTECTED 1.2 GAME.EXE", f.exe ? C_OK : C_RED, NULL);
        int ready = f.discs >= 1 && f.patch && f.exe;
        text(2, 15, "THE IMPORT TAKES SEVERAL MINUTES AND", C_DIM, NULL);
        text(2, 16, "NEEDS ABOUT 2 GB OF FREE SPACE.", C_DIM, NULL);
        text(4, 19, ready ? " A  IMPORT GAME FILES " : " A  RESCAN SOURCE ", C_HI_FG, &C_HI_BG);
        text(4, 21, " B  EXIT ", C_TEXT, NULL);
        hint("A CONTINUE   B EXIT");
        SDL_RenderPresent(R);
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
                for (int i = 0; i < LOGN; i++) text(2, 8 + i, g_log[i], i == LOGN - 1 ? C_RED : C_DIM, NULL);
                text(2, 19, "FULL LOG: PORTS/THE_FAMILIA/LOG.TXT", C_DIM, NULL);
                text(4, 22, " A  TRY AGAIN ", C_HI_FG, &C_HI_BG); text(4, 24, " B  EXIT ", C_TEXT, NULL);
                hint("A RETRY   B EXIT");
                SDL_RenderPresent(R);
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
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++) if (SDL_IsGameController(i)) g_pad = SDL_GameControllerOpen(i);
    cfg_load();
    int rc = -1;
    char marker[4200]; snprintf(marker, sizeof marker, "%s/.installed", g_data);
    if (access(marker, F_OK) != 0) rc = page_import();
    else if (getenv("FAMILIA_AUTOSTART")) rc = cfg_save() == 0 ? EXIT_RUN : EXIT_ERR;   /* automated tests */
    if (rc < 0) rc = page_settings();
    SDL_DestroyRenderer(R); SDL_DestroyWindow(win); SDL_Quit();
    return rc;
}
