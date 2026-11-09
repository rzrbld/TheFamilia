#pragma once
#include <stdint.h>
typedef struct { float acc_x, acc_y; int32_t wheel; uint8_t real_buttons, pad_buttons; } fam_mouse;
typedef struct { uint8_t dik, down; uint32_t time, seq; } fam_key_event;
void fam_input_init(void);
void fam_input_pump(void);
void fam_input_keys(uint8_t out[256]);
void fam_input_mouse(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t *buttons);
int  fam_input_next_key(fam_key_event *ev);
void fam_input_flush(void);

/* the game controller (an Xbox 360 layout), read by the DirectInput joystick */
enum { FAM_PB_A, FAM_PB_B, FAM_PB_X, FAM_PB_Y, FAM_PB_LB, FAM_PB_RB, FAM_PB_BACK, FAM_PB_START, FAM_PB_L3, FAM_PB_R3,
       FAM_PB_LT, FAM_PB_RT, FAM_PB_UP, FAM_PB_DOWN, FAM_PB_LEFT, FAM_PB_RIGHT };
typedef struct { int32_t lx, ly, rx, ry, lt, rt; uint32_t buttons; } fam_pad;
int  fam_pad_present(void);
void fam_pad_read(fam_pad *p);            /* consumes the latched presses */
