/*
 * pad_controls.c - gamepad bindings in the game's own controls table.
 *
 * Mafia binds each of its 60 controls to a key, a mouse input or a joystick
 * input (G_LoadSaveClass::bindings, a profile's table; ApplyControls hands it
 * to C_controls). Its defaults are keyboard and mouse only. Before each
 * ApplyControls (Game.exe 0x607A60) this fills in the gamepad's bindings for
 * a group that has none: the on-foot controls when none of them uses the
 * joystick, the car controls while they still hold the stock joystick
 * defaults. Keyboard primaries stay; the gamepad takes the second column, and
 * the one-column controls. Anything the player rebinds in Options > Controls
 * is kept, since a group is filled only once.
 *
 * The layout follows the console one (and the community Steam Input layout
 * for the PC game): the controller is an Xbox 360 pad (dinput8.c's joystick).
 */
#include <stdio.h>
#include <stdlib.h>
#include "win32hle.h"
#include "recomp_types.h"

void recomp_game_00607A60_orig(void);

/* the controls' indices (CTRL_*, after the controls menus) */
enum {
    C_FORWARD = 0x00, C_BACKWARD = 0x02, C_LEFT = 0x04, C_RIGHT = 0x06, C_WALK = 0x08, C_ACTION = 0x0A,
    C_FIRE = 0x0C, C_CROUCH = 0x0E, C_JUMP = 0x10, C_AIM = 0x12, C_CYCLE_UP = 0x14, C_CYCLE_DOWN = 0x16,
    C_INVENTORY = 0x18, C_HIDE_WEAPON = 0x19, C_DROP_WEAPON = 0x1A, C_TOGGLE_RUN = 0x1B, C_RELOAD = 0x1C,
    C_SNIPER = 0x1D, C_OBJECTIVES = 0x1E, C_MAP = 0x1F,
    C_ACCELERATE = 0x21, C_BRAKE = 0x23, C_STEER_LEFT = 0x25, C_STEER_RIGHT = 0x27, C_HANDBRAKE = 0x29,
    C_LIMITER = 0x2B, C_GEAR_UP = 0x2D, C_GEAR_DOWN = 0x2F, C_HORN = 0x31, C_LOOK_LEFT = 0x33,
    C_LOOK_RIGHT = 0x35, C_CLUTCH = 0x37, C_GEARING = 0x39, C_CAMERA = 0x3A,
    SECOND = 1, CAR_FIRST = 0x21, NUM = 60,
};
#define DEV_NONE 0
#define DEV_JOY 3
#define HALF_FULL 0
#define HALF_POS 1
#define HALF_NEG 2
/* joystick codes: an axis is its JOY_* (X Y Z Rx Ry Rz), a button 0x100 + n, POV 0's directions 0x200 + bits */
enum { AX_LX = 0, AX_LY = 1, AX_LT = 2, AX_RX = 3, AX_RY = 4, AX_RT = 5 };
#define BTN(n) (0x100 + (n))
enum { B_A, B_B, B_X, B_Y, B_LB, B_RB, B_BACK, B_START, B_L3, B_R3, B_LT, B_RT };
#define POV_UP 0x201
#define POV_RIGHT 0x202
#define POV_DOWN 0x204
#define POV_LEFT 0x208

typedef struct { int index; int code; int half; } pad_bind;
static const pad_bind k_foot[] = {
    { C_FORWARD + SECOND, AX_LY, HALF_NEG }, { C_BACKWARD + SECOND, AX_LY, HALF_POS },
    { C_LEFT + SECOND, AX_LX, HALF_NEG },    { C_RIGHT + SECOND, AX_LX, HALF_POS },
    { C_ACTION + SECOND, BTN(B_Y), 0 },      /* use, enter and leave cars */
    { C_FIRE + SECOND, BTN(B_RT), 0 },
    { C_CROUCH + SECOND, BTN(B_RB), 0 },
    { C_JUMP + SECOND, BTN(B_X), 0 },
    { C_CYCLE_UP + SECOND, POV_RIGHT, 0 },   { C_CYCLE_DOWN + SECOND, POV_LEFT, 0 },
    { C_INVENTORY, BTN(B_LB), 0 },
    { C_HIDE_WEAPON, POV_UP, 0 },            { C_DROP_WEAPON, POV_DOWN, 0 },
    { C_TOGGLE_RUN, BTN(B_L3), 0 },
    { C_RELOAD, BTN(B_B), 0 },
    { C_SNIPER, BTN(B_LT), 0 },
    { C_OBJECTIVES, BTN(B_R3), 0 },
    { C_MAP, BTN(B_BACK), 0 },
};
static const pad_bind k_car[] = {
    { C_ACCELERATE + SECOND, AX_RT, HALF_POS }, { C_BRAKE + SECOND, AX_LT, HALF_POS },
    { C_STEER_LEFT + SECOND, AX_LX, HALF_NEG }, { C_STEER_RIGHT + SECOND, AX_LX, HALF_POS },
    { C_HANDBRAKE + SECOND, BTN(B_X), 0 },
    { C_LIMITER + SECOND, BTN(B_B), 0 },
    { C_GEAR_UP + SECOND, POV_RIGHT, 0 },       { C_GEAR_DOWN + SECOND, POV_LEFT, 0 },
    { C_HORN + SECOND, BTN(B_L3), 0 },
    { C_LOOK_LEFT + SECOND, BTN(B_LB), 0 },     { C_LOOK_RIGHT + SECOND, BTN(B_RB), 0 },
    { C_GEARING, POV_DOWN, 0 },
    { C_CAMERA, BTN(B_R3), 0 },
};

/* S_control: int half; ushort device; ushort code; int joy */
#define BIND(t, i) ((t) + (uint32_t)(i) * 12u)
static void set(uint32_t table, const pad_bind *b) {
    uint32_t e = BIND(table, b->index);
    MEM32(e) = (uint32_t)b->half;
    MEM16(e + 4) = DEV_JOY;
    MEM16(e + 6) = (uint16_t)b->code;
    MEM32(e + 8) = 0;
}

/* void G_LoadSaveClass::ApplyControls()  -- Game.exe 0x607A60, thiscall */
void fam_native_apply_controls(void) {
    uint32_t table = g_ecx + 0x1F4;                       /* bindings[NUM_CONTROLS] */
    int foot_has_pad = 0;
    for (int i = 0; i < CAR_FIRST; i++) if (MEM16(BIND(table, i) + 4) == DEV_JOY) foot_has_pad = 1;
    /* the car's stock second column binds the joystick's Y axis to the pedals */
    uint32_t acc = BIND(table, C_ACCELERATE + SECOND);
    int car_stock = MEM16(acc + 4) == DEV_JOY && MEM16(acc + 6) == AX_LY && MEM32(acc) == HALF_NEG;
    if (!foot_has_pad) for (size_t i = 0; i < sizeof k_foot / sizeof *k_foot; i++) set(table, &k_foot[i]);
    if (car_stock) for (size_t i = 0; i < sizeof k_car / sizeof *k_car; i++) set(table, &k_car[i]);
    if (!foot_has_pad || car_stock)
        fprintf(stderr, "[controls] gamepad bindings added:%s%s\n", !foot_has_pad ? " on foot" : "", car_stock ? " car" : "");
    recomp_game_00607A60_orig();
}
