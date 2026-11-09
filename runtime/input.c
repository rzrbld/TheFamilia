/*
 * input.c - the port's single SDL event pump and input state.
 *
 * State the guest reads through DirectInput (dinput8.c):
 *   - a 256-byte DIK key array, the OR of real keyboard keys and keys the
 *     controller mapping holds;
 *   - accumulated mouse deltas and buttons (real mouse + right stick/triggers);
 *   - a queue of key transitions for buffered reads.
 * Window messages for the guest (WM_KEYDOWN/UP, WM_CHAR, WM_CLOSE) are posted
 * to the device window, since menus and text entry read those.
 *
 * Controller (see "the game controller" below): a DirectInput joystick for
 * the game, plus mouse look, Esc and menu keys from the engine. On PortMaster
 * gptokeyb2 -x presents it as a virtual Xbox 360 pad. FAMILIA_PAD=0 turns the
 * controller off. Every key or button press is latched until the game's next
 * state read, so a tap between two polls is not lost.
 */
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "win32hle.h"
#include "input.h"
#include "d3d8_internal.h"

#define WM_CLOSE 0x0010
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102

static uint8_t g_kb_real[256], g_kb_pad[256];
static fam_mouse g_mouse;
static uint8_t g_mouse_latch;
static fam_key_event g_q[256];
static unsigned g_qh, g_qt;
static SDL_GameController *g_pad;
static int g_pad_on;
static uint32_t g_seq;
static int g_quit_posted;

/* ---- SDL scancode -> DIK (set-1 scan codes) and Win32 VK ---- */
static uint8_t dik_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t L[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                       0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
        return L[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return (uint8_t)(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F; case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B; case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37; case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A; case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49; case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C; case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_KP_1: return 0x4F; case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52; case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58; case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_KP_DIVIDE: return 0xB5; case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
    default: return 0;
    }
}
/* DIK -> Win32 virtual key, for the WM_KEY* messages a synthesized key posts */
static uint32_t vk_of_dik(uint8_t d) {
    static const char row1[] = "1234567890", rowq[] = "QWERTYUIOP", rowa[] = "ASDFGHJKL", rowz[] = "ZXCVBNM";
    if (d >= 0x02 && d <= 0x0B) return (uint32_t)row1[d - 0x02];
    if (d >= 0x10 && d <= 0x19) return (uint32_t)rowq[d - 0x10];
    if (d >= 0x1E && d <= 0x26) return (uint32_t)rowa[d - 0x1E];
    if (d >= 0x2C && d <= 0x32) return (uint32_t)rowz[d - 0x2C];
    if (d >= 0x3B && d <= 0x44) return 0x70u + (d - 0x3Bu);
    switch (d) {
    case 0x01: return 0x1B; case 0x0E: return 0x08; case 0x0F: return 0x09; case 0x1C: case 0x9C: return 0x0D;
    case 0x1D: case 0x9D: return 0x11; case 0x2A: case 0x36: return 0x10; case 0x38: case 0xB8: return 0x12;
    case 0x39: return 0x20; case 0xC8: return 0x26; case 0xD0: return 0x28; case 0xCB: return 0x25; case 0xCD: return 0x27;
    case 0xC9: return 0x21; case 0xD1: return 0x22; case 0xC7: return 0x24; case 0xCF: return 0x23;
    case 0xD2: return 0x2D; case 0xD3: return 0x2E; case 0x57: return 0x7A; case 0x58: return 0x7B;
    }
    return 0;
}

static void key_event(uint8_t dik, int down) {
    unsigned n = (g_qt - g_qh) & 255;
    if (n == 255) g_qh++;                        /* overload: drop the oldest */
    g_q[g_qt & 255] = (fam_key_event){ dik, (uint8_t)down, SDL_GetTicks(), g_seq++ };
    g_qt++;
    uint32_t vk = vk_of_dik(dik);
    if (g_dev.hwnd && vk) {
        uint32_t lp = 1u | ((uint32_t)dik << 16) | (down ? 0 : 0xC0000000u);
        hle_post_message(g_dev.hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    }
}
static uint8_t g_kb_script[256], g_kb_latch[256];
static void set_key(uint8_t *arr, uint8_t dik, int down) {
    uint8_t before = g_kb_real[dik] | g_kb_pad[dik] | g_kb_script[dik];
    arr[dik] = down ? 0x80 : 0;
    uint8_t after = g_kb_real[dik] | g_kb_pad[dik] | g_kb_script[dik];
    if (after && !before) g_kb_latch[dik] = 0x80;     /* seen by the next state read even if released before it */
    if (before != after) key_event(dik, after != 0);
}

/* ---- scripted input (development: automated benchmark runs) ----
 * FAMILIA_INPUT_SCRIPT="T:KEY[:HOLD],..."  T = seconds since start, KEY a name
 * below or a DIK code in hex (0x1C), HOLD = seconds held (default 0.15). */
typedef struct { uint32_t at_ms, up_ms; uint8_t dik, state, pad; } script_key;   /* pad: a controller button + 1 */
static uint32_t g_pad_script, g_pad_latch, g_pad_latch_engine;
static script_key g_script[256];
static int g_nscript;
static uint32_t g_script_t0;
static uint8_t dik_named(const char *n) {
    static const struct { const char *n; uint8_t d; } t[] = {
        {"esc", 0x01}, {"enter", 0x1C}, {"space", 0x39}, {"tab", 0x0F}, {"up", 0xC8}, {"down", 0xD0},
        {"left", 0xCB}, {"right", 0xCD}, {"shift", 0x2A}, {"ctrl", 0x1D}, {"alt", 0x38},
        {"q", 0x10}, {"w", 0x11}, {"e", 0x12}, {"r", 0x13}, {"a", 0x1E}, {"s", 0x1F}, {"d", 0x20},
        {"f", 0x21}, {"c", 0x2E}, {"h", 0x23}, {"y", 0x15}, {"n", 0x31}, {"1", 0x02}, {"2", 0x03},
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(t[i].n, n)) return t[i].d;
    return (uint8_t)strtoul(n, NULL, 16);
}
static void script_init(void) {
    const char *s = getenv("FAMILIA_INPUT_SCRIPT");
    if (!s) return;
    char buf[4096]; snprintf(buf, sizeof buf, "%s", s);
    for (char *save = NULL, *tok = strtok_r(buf, ", ", &save); tok && g_nscript < 256; tok = strtok_r(NULL, ", ", &save)) {
        char name[32] = ""; double t = 0, hold = 0.15;
        if (sscanf(tok, "%lf:%31[^:]:%lf", &t, name, &hold) < 2) continue;
        static const char *pads[] = { "pad_a", "pad_b", "pad_x", "pad_y", "pad_lb", "pad_rb", "pad_back", "pad_start",
                                      "pad_l3", "pad_r3", "pad_lt", "pad_rt", "pad_up", "pad_down", "pad_left", "pad_right" };
        uint8_t pad = 0;
        for (int i = 0; i < 16; i++) if (!strcmp(name, pads[i])) pad = (uint8_t)(i + 1);
        uint8_t d = pad ? 0 : dik_named(name);
        if (d || pad) g_script[g_nscript++] = (script_key){ (uint32_t)(t * 1000), (uint32_t)((t + hold) * 1000), d, 0, pad };
    }
    g_script_t0 = SDL_GetTicks();
    fprintf(stderr, "[input] script: %d key presses\n", g_nscript);
}
static void script_update(void) {
    uint32_t now = SDL_GetTicks() - g_script_t0;
    for (int i = 0; i < g_nscript; i++) {
        script_key *k = &g_script[i];
        if (k->state == 0 && now >= k->at_ms) {
            k->state = 1;
            if (k->pad) { uint32_t bit = 1u << (k->pad - 1); g_pad_script |= bit; g_pad_latch |= bit; g_pad_latch_engine |= bit; }
            else set_key(g_kb_script, k->dik, 1);
            fprintf(stderr, "[input] script %.1fs %s %02X\n", now / 1000.0, k->pad ? "pad" : "key", k->pad ? k->pad - 1 : k->dik);
        }
        if (k->state == 1 && now >= k->up_ms) { k->state = 2; if (k->pad) g_pad_script &= ~(1u << (k->pad - 1)); else set_key(g_kb_script, k->dik, 0); }
    }
}

/* ---- the game controller ----
 * The pad reaches the game as a DirectInput joystick (dinput8.c, read by
 * IJoy.dll through fam_pad_read), so the game's own controls apply: separate
 * on-foot and car bindings, analog steering and pedals, rebinding in
 * Options > Controls. On PortMaster, gptokeyb2 -x mirrors the handheld's
 * controls as a virtual Xbox 360 pad (045E:028E), which is preferred over the
 * physical device so every handheld looks the same.
 * The engine adds what the game has no binding for: right stick -> mouse
 * (looking and aiming use the mouse), Start -> Esc, and while a menu is open
 * A / B / d-pad / left stick -> Enter / Esc / arrows. */
#define GAME_MISSION 0x63788Cu        /* C_mission *act_mission */
#define GAME_MENU    0x6BD890u        /* GM_Menu::activeMenuWindow */
static int pad_score(int index) {
    if (!SDL_IsGameController(index)) return 0;
    return SDL_JoystickGetDeviceVendor(index) == 0x045E && SDL_JoystickGetDeviceProduct(index) == 0x028E ? 2 : 1;
}
static void pad_open_best(void) {
    int best = -1, score = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) { int sc = pad_score(i); if (sc >= score && sc > 0) best = i, score = sc; }
    if (best < 0) return;
    SDL_GameController *c = SDL_GameControllerOpen(best);
    if (!c) return;
    if (c == g_pad) { SDL_GameControllerClose(c); return; }   /* already ours: drop the extra reference */
    if (g_pad) SDL_GameControllerClose(g_pad);
    g_pad = c;
    fprintf(stderr, "[input] controller: %s%s\n", SDL_GameControllerName(c), score == 2 ? " (gptokeyb)" : "");
}
static uint32_t pad_bit_of(int sdl_button) {
    switch (sdl_button) {
    case SDL_CONTROLLER_BUTTON_A: return 1u << FAM_PB_A; case SDL_CONTROLLER_BUTTON_B: return 1u << FAM_PB_B;
    case SDL_CONTROLLER_BUTTON_X: return 1u << FAM_PB_X; case SDL_CONTROLLER_BUTTON_Y: return 1u << FAM_PB_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 1u << FAM_PB_LB; case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 1u << FAM_PB_RB;
    case SDL_CONTROLLER_BUTTON_BACK: return 1u << FAM_PB_BACK; case SDL_CONTROLLER_BUTTON_START: return 1u << FAM_PB_START;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 1u << FAM_PB_L3; case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 1u << FAM_PB_R3;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 1u << FAM_PB_UP; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 1u << FAM_PB_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 1u << FAM_PB_LEFT; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 1u << FAM_PB_RIGHT;
    }
    return 0;
}
#define TRIGGER_ON 12000
/* g_pad_latch: buttons pressed since the game last read them; g_pad_latch_engine: the same for
 * the engine's own keys below; g_pad_script: buttons the input script holds */
static uint32_t pad_held(void) {
    if (!g_pad) return g_pad_script;
    uint32_t b = g_pad_script;
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) if (SDL_GameControllerGetButton(g_pad, (SDL_GameControllerButton)i)) b |= pad_bit_of(i);
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > TRIGGER_ON) b |= 1u << FAM_PB_LT;
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_ON) b |= 1u << FAM_PB_RT;
    return b;
}
/* a stick with a radial dead zone, rescaled to -10000..10000 */
static void stick(SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, int32_t *ox, int32_t *oy) {
    float x = SDL_GameControllerGetAxis(g_pad, ax) / 32767.f, y = SDL_GameControllerGetAxis(g_pad, ay) / 32767.f;
    float m = sqrtf(x * x + y * y), dz = 0.2f;
    if (m <= dz) { *ox = *oy = 0; return; }
    float k = (m > 1.f ? 1.f : m) - dz; k = k / (1.f - dz) / m;
    *ox = (int32_t)(x * k * 10000.f); *oy = (int32_t)(y * k * 10000.f);
}
int fam_pad_present(void) { return g_pad != NULL; }
void fam_pad_read(fam_pad *p) {
    memset(p, 0, sizeof *p);
    if (!g_pad && !g_pad_script) return;
    stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &p->lx, &p->ly);
    stick(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &p->rx, &p->ry);
    p->lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) * 10000 / 32767;
    p->rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 10000 / 32767;
    if (g_pad_script & (1u << FAM_PB_LT)) p->lt = 10000;             /* scripted triggers are pulled fully */
    if (g_pad_script & (1u << FAM_PB_RT)) p->rt = 10000;
    p->buttons = pad_held() | g_pad_latch;           /* a tap between two polls still counts once */
    g_pad_latch = 0;
}
/* whether the game shows a menu (main menu, pause menu, loading) rather than play */
static int in_menu(void) {
    if (MEM32(GAME_MENU)) return 1;
    uint32_t mis = MEM32(GAME_MISSION), game = mis ? MEM32(mis + 0x24) : 0;
    return !game || !MEM32(game + 0xE4);               /* no player */
}
static void pad_update(void) {
    if (!g_pad) return;
    uint32_t b = pad_held() | g_pad_latch_engine;
    g_pad_latch_engine = 0;
    uint8_t want[256] = {0};
    if (b & (1u << FAM_PB_START)) want[0x01] = 0x80;                 /* Start: Esc */
    if (in_menu()) {
        g_pad_latch = 0;                               /* menu presses are not replayed into play */
        int32_t lx, ly; stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &lx, &ly);
        if (b & (1u << FAM_PB_A)) want[0x1C] = 0x80;                 /* Enter */
        if (b & (1u << FAM_PB_B)) want[0x01] = 0x80;                 /* Esc */
        if ((b & (1u << FAM_PB_UP)) || ly < -5000) want[0xC8] = 0x80;
        if ((b & (1u << FAM_PB_DOWN)) || ly > 5000) want[0xD0] = 0x80;
        if ((b & (1u << FAM_PB_LEFT)) || lx < -5000) want[0xCB] = 0x80;
        if ((b & (1u << FAM_PB_RIGHT)) || lx > 5000) want[0xCD] = 0x80;
    }
    for (int k = 0; k < 256; k++) if (want[k] != g_kb_pad[k]) set_key(g_kb_pad, (uint8_t)k, want[k] != 0);
    /* right stick -> mouse motion, by time: FAMILIA_LOOK_SENS percent of 1500 px/s at full tilt */
    static int look = -1; static uint64_t last;
    if (look < 0) look = fam_config_int("look_sens", 30);
    uint64_t now = SDL_GetTicks64();
    float dt = last ? (float)(now - last) / 1000.f : 0.f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    int32_t rx, ry; stick(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &rx, &ry);
    float fx = rx / 10000.f, fy = ry / 10000.f, m = sqrtf(fx * fx + fy * fy);
    if (m > 0.f) {
        float speed = m * m * 1500.f * (float)look / 100.f * dt / m;    /* quadratic response */
        g_mouse.acc_x += fx * speed; g_mouse.acc_y += fy * speed;
    }
}

/* development: the game state the controller layer reads (FAMILIA_PADDBG=1) */
static void pad_debug(void) {
    static uint32_t last; uint32_t now = SDL_GetTicks();
    if (now - last < 250) return;
    last = now;
    uint32_t mis = MEM32(0x63788Cu), game = mis ? MEM32(mis + 0x24) : 0, pl = game ? MEM32(game + 0xE4) : 0;
    uint32_t car = pl ? MEM32(pl + 0x98) : 0, menu = MEM32(0x6BD890u);
    uint32_t ctl = MEM32(0x647C30u + 0xB4), val = MEM32(0x647C30u + 0xC4), prs = MEM32(0x647C30u + 0xD4);
    fprintf(stderr, "[paddbg] mission %08X game %08X player %08X car %08X menu %08X | joys %08X..%08X", mis, game, pl, car, menu,
            MEM32(0x647C30u + 8), MEM32(0x647C30u + 12));
    if (ctl && val) {
        for (int i = 0x0A; i <= 0x0B; i++) {
            float v, pv; uint32_t a = MEM32(val + i * 4), b = prs ? MEM32(prs + i * 4) : 0; memcpy(&v, &a, 4); memcpy(&pv, &b, 4);
            fprintf(stderr, " | ctl%02X half %u dev %u code %03X value %.2f pressed %.2f", i, MEM32(ctl + i * 12), MEM16(ctl + i * 12 + 4), MEM16(ctl + i * 12 + 6), v, pv);
        }
    }
    if (game) { float pp[3]; memcpy(pp, hle_ptr(game + 0x2A10), 12); fprintf(stderr, " | pos %.1f %.1f %.1f", pp[0], pp[1], pp[2]); }
    fprintf(stderr, "\n");
}

void fam_input_pump(void) {
    static int dbg = -1; if (dbg < 0) dbg = fam_config_int("paddbg", 0);
    if (dbg) pad_debug();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) fam_update_output_rect();
            break;
        case SDL_QUIT:
            if (!g_quit_posted && g_dev.hwnd) { hle_post_message(g_dev.hwnd, WM_CLOSE, 0, 0); g_quit_posted = 1; }
            break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            if (e.key.repeat) break;
            uint8_t d = dik_of(e.key.keysym.scancode);
            if (d) set_key(g_kb_real, d, e.type == SDL_KEYDOWN);
            break;
        }
        case SDL_TEXTINPUT:
            if (g_dev.hwnd) for (const char *c = e.text.text; *c; c++) if ((unsigned char)*c < 0x80) hle_post_message(g_dev.hwnd, WM_CHAR, (uint8_t)*c, 1);
            break;
        case SDL_MOUSEMOTION: g_mouse.acc_x += (float)e.motion.xrel; g_mouse.acc_y += (float)e.motion.yrel; break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            if (b >= 0) { if (e.type == SDL_MOUSEBUTTONDOWN) g_mouse.real_buttons |= (uint8_t)(1 << b), g_mouse_latch |= (uint8_t)(1 << b); else g_mouse.real_buttons &= (uint8_t)~(1 << b); }
            break;
        }
        case SDL_MOUSEWHEEL: g_mouse.wheel += e.wheel.y * 120; break;
        case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad_on) {
                if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                    SDL_GameControllerClose(g_pad); g_pad = NULL;
                    memset(g_kb_pad, 0, sizeof g_kb_pad);
                }
                pad_open_best();                       /* gptokeyb's virtual pad can appear after the game starts */
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (g_pad && e.cbutton.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) { uint32_t bit = pad_bit_of(e.cbutton.button); g_pad_latch |= bit; g_pad_latch_engine |= bit; }
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (g_pad && e.caxis.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) && e.caxis.value > TRIGGER_ON) {
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) g_pad_latch |= 1u << FAM_PB_LT, g_pad_latch_engine |= 1u << FAM_PB_LT;
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) g_pad_latch |= 1u << FAM_PB_RT, g_pad_latch_engine |= 1u << FAM_PB_RT;
            }
            break;
        }
    }
    pad_update();
    if (g_nscript) script_update();
}

void fam_input_init(void) {
    static int done;
    if (done) return;
    done = 1;
    SDL_InitSubSystem(SDL_INIT_EVENTS);
    script_init();
    g_pad_on = fam_config_int("pad", 1);              /* FAMILIA_PAD=0: no controller */
    if (!g_pad_on) return;
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    const char *db = getenv("SDL_GAMECONTROLLERCONFIG_FILE");
    if (db) SDL_GameControllerAddMappingsFromFile(db);
    pad_open_best();
}

void fam_input_keys(uint8_t out[256]) {
    for (int i = 0; i < 256; i++) out[i] = g_kb_real[i] | g_kb_pad[i] | g_kb_script[i] | g_kb_latch[i];
    memset(g_kb_latch, 0, sizeof g_kb_latch);
}
void fam_input_mouse(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t *buttons) {
    int32_t x = (int32_t)g_mouse.acc_x, y = (int32_t)g_mouse.acc_y;
    g_mouse.acc_x -= (float)x; g_mouse.acc_y -= (float)y;     /* keep the fraction */
    *dx = x; *dy = y; *dz = g_mouse.wheel; g_mouse.wheel = 0;
    *buttons = g_mouse.real_buttons | g_mouse.pad_buttons | g_mouse_latch;
    g_mouse_latch = 0;
}
int fam_input_next_key(fam_key_event *ev) {
    if (g_qh == g_qt) return 0;
    *ev = g_q[g_qh++ & 255];
    return 1;
}
void fam_input_flush(void) { g_qh = g_qt; }
