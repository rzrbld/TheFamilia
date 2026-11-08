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
