/*
 * dinput8.c - DirectInput 8 (keyboard, mouse) for LS3DF.dll, and
 * DirectInput 7 for IJoy.dll with one joystick: the controller from input.c
 * (an Xbox 360 layout), so the game's own joystick controls work.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "com.h"
#include "dinput_slots.h"
#include "input.h"
#include "d3d8_internal.h"

FAM_DEFINE_IFACE(DI8, IDirectInput8A, IDIRECTINPUT8A)
FAM_DEFINE_IFACE(DID8, IDirectInputDevice8A, IDIRECTINPUTDEVICE8A)
FAM_DEFINE_IFACE(DI7, IDirectInput7A, IDIRECTINPUT7A)
FAM_DEFINE_IFACE(DIJ, IDirectInputDevice8A, IDIRECTINPUTDEVICE8A)   /* the joystick: DX7 shares the slots */

#define DI_OK 0u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_DEVICENOTREG 0x80040154u
#define DI_BUFFEROVERFLOW 1u

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_JOY = 3 };
typedef struct { int type; int acquired; uint32_t buffer_size; uint32_t obj; } didev;

/* {6F1D2B61-D5A0-11CF-BFC7-444553540000} keyboard, ...60... mouse */
static int guid_is(const uint8_t *g, uint32_t first) {
    static const uint8_t tail[12] = { 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    uint32_t d1; memcpy(&d1, g, 4);
    return d1 == first && !memcmp(g + 4, tail, 12);
}

static void di_QueryInterface(void) { if (A32(2)) MEM32(A32(2)) = A32(0); fam_com_addref(A32(0)); RET(DI_OK, 3); }
static void di_AddRef(void) { RET(fam_com_addref(A32(0)), 1); }
static void di_Release(void) { RET(fam_com_release(A32(0)), 1); }

static void di_CreateDevice(void) {                  /* (this, REFGUID, device**, outer) */
    const uint8_t *g = APTR(1);
    int type = guid_is(g, 0x6F1D2B61u) ? DEV_KEYBOARD : guid_is(g, 0x6F1D2B60u) ? DEV_MOUSE : 0;
    if (!type) { fprintf(stderr, "[dinput] CreateDevice: unknown device GUID\n"); RET(DIERR_DEVICENOTREG, 4); }
    didev *d = calloc(1, sizeof *d);
    d->type = type;
    d->obj = fam_com_new(&DID8_iface, d);
    MEM32(A32(2)) = d->obj;
    fam_input_init();
    fprintf(stderr, "[dinput] CreateDevice(%s)\n", type == DEV_KEYBOARD ? "keyboard" : "mouse");
    RET(DI_OK, 4);
}
static void di_EnumDevices(void) { RET(DI_OK, 5); }  /* (this, type, cb, ref, flags): no game controllers listed */
static void di_GetDeviceStatus(void) { const uint8_t *g = APTR(1); RET(guid_is(g, 0x6F1D2B61u) || guid_is(g, 0x6F1D2B60u) ? DI_OK : 1u, 2); }
static void di_Initialize(void) { RET(DI_OK, 3); }

static didev *dev_of(uint32_t o) { return fam_com_host(o); }
static void dd_Acquire(void)   { dev_of(A32(0))->acquired = 1; RET(DI_OK, 1); }
static void dd_Unacquire(void) { didev *d = dev_of(A32(0)); d->acquired = 0; if (d->type == DEV_KEYBOARD) fam_input_flush(); RET(DI_OK, 1); }
static void dd_SetDataFormat(void) { RET(DI_OK, 2); }
static void dd_SetCooperativeLevel(void) { RET(DI_OK, 3); }
static void dd_SetEventNotification(void) { RET(DI_OK, 2); }
static void dd_Poll(void) { RET(DI_OK, 1); }
static void dd_SetProperty(void) {                   /* (this, REFGUID prop, DIPROPHEADER*) */
    didev *d = dev_of(A32(0));
    if (A32(1) == 1 /* DIPROP_BUFFERSIZE */) d->buffer_size = MEM32(A32(2) + 16);
    RET(DI_OK, 3);
}
static void dd_GetCapabilities(void) {               /* (this, DIDEVCAPS*) */
    didev *d = dev_of(A32(0));
    uint32_t c = A32(1);
    MEM32(c + 4) = 1;                                /* DIDC_ATTACHED */
    MEM32(c + 8) = d->type == DEV_KEYBOARD ? 0x13 : 0x12;   /* DI8DEVTYPE_KEYBOARD / MOUSE */
    MEM32(c + 12) = d->type == DEV_MOUSE ? 3 : 0;    /* axes */
    MEM32(c + 16) = d->type == DEV_MOUSE ? 4 : 128;  /* buttons */
    RET(DI_OK, 2);
}
static void dd_GetDeviceState(void) {                /* (this, size, data*) */
    didev *d = dev_of(A32(0));
    uint32_t n = A32(1);
    fam_input_pump();
    if (d->type == DEV_KEYBOARD) {
        uint8_t k[256]; fam_input_keys(k);
        memcpy(APTR(2), k, n < 256 ? n : 256);
    } else {
        int32_t dx, dy, dz; uint8_t b;
        fam_input_mouse(&dx, &dy, &dz, &b);
        uint8_t *p = APTR(2);
        memset(p, 0, n);
        if (n >= 12) { memcpy(p, &dx, 4); memcpy(p + 4, &dy, 4); memcpy(p + 8, &dz, 4); }
        for (uint32_t i = 0; i < 4 && 12 + i < n; i++) p[12 + i] = (b >> i) & 1 ? 0x80 : 0;
    }
    RET(DI_OK, 3);
}
static void dd_GetDeviceData(void) {                 /* (this, cbObjectData, rgdod*, inout count*, flags) */
    didev *d = dev_of(A32(0));
    uint32_t sz = A32(1), out = A32(2), cnt = MEM32(A32(3)), n = 0;
    fam_input_pump();
    if (d->type == DEV_KEYBOARD) {
        fam_key_event ev;
        int peek = (A32(4) & 1) != 0;                /* DIGDD_PEEK: not consumed */
        while (n < cnt || (!out && n < 1024)) {
            if (peek) break;
            if (!fam_input_next_key(&ev)) break;
            if (out) {
                uint32_t r = out + n * sz;
                memset(hle_ptr(r), 0, sz);
                MEM32(r) = ev.dik; MEM32(r + 4) = ev.down ? 0x80 : 0; MEM32(r + 8) = ev.time; MEM32(r + 12) = ev.seq;
            }
            n++;
        }
    }
    MEM32(A32(3)) = n;
    RET(DI_OK, 5);
}
static void dd_Release(void) {
    uint32_t n = fam_com_release(A32(0));
    RET(n, 1);
}

static void s_DirectInput8Create(void) {             /* (hinst, version, riid, out**, outer) */
    static uint32_t obj;
    if (!obj) {
        static const fam_method di[] = {
            { IDIRECTINPUT8A_QueryInterface, di_QueryInterface }, { IDIRECTINPUT8A_AddRef, di_AddRef },
            { IDIRECTINPUT8A_Release, di_Release }, { IDIRECTINPUT8A_CreateDevice, di_CreateDevice },
            { IDIRECTINPUT8A_EnumDevices, di_EnumDevices }, { IDIRECTINPUT8A_GetDeviceStatus, di_GetDeviceStatus },
            { IDIRECTINPUT8A_Initialize, di_Initialize }, { 0, 0 } };
        static const fam_method dd[] = {
            { IDIRECTINPUTDEVICE8A_QueryInterface, di_QueryInterface }, { IDIRECTINPUTDEVICE8A_AddRef, di_AddRef },
            { IDIRECTINPUTDEVICE8A_Release, dd_Release }, { IDIRECTINPUTDEVICE8A_GetCapabilities, dd_GetCapabilities },
            { IDIRECTINPUTDEVICE8A_SetProperty, dd_SetProperty }, { IDIRECTINPUTDEVICE8A_Acquire, dd_Acquire },
            { IDIRECTINPUTDEVICE8A_Unacquire, dd_Unacquire }, { IDIRECTINPUTDEVICE8A_GetDeviceState, dd_GetDeviceState },
            { IDIRECTINPUTDEVICE8A_GetDeviceData, dd_GetDeviceData }, { IDIRECTINPUTDEVICE8A_SetDataFormat, dd_SetDataFormat },
            { IDIRECTINPUTDEVICE8A_SetEventNotification, dd_SetEventNotification },
            { IDIRECTINPUTDEVICE8A_SetCooperativeLevel, dd_SetCooperativeLevel }, { IDIRECTINPUTDEVICE8A_Poll, dd_Poll },
            { 0, 0 } };
        fam_iface_vtable(&DI8_iface, di);
        fam_iface_vtable(&DID8_iface, dd);
        obj = fam_com_new(&DI8_iface, NULL);
    } else fam_com_addref(obj);
    MEM32(A32(3)) = obj;
    RET(DI_OK, 5);
}

/* ---- IJoy.dll (DX7): one joystick, the controller ----
 * DIJOYSTATE2: X/Y left stick, Z left trigger, Rx/Ry right stick, Rz right
 * trigger (triggers 0..10000), POV 0 the d-pad, buttons in the Xbox 360
 * order (A B X Y LB RB Back Start L3 R3) plus the triggers as buttons 10/11. */
static const uint8_t k_joy_guid[16] = { 'F','A','M','I','L','I','A','-','P','A','D','-','0','0','0','1' };
static const uint8_t k_joy_product[16] = { 0x5E,0x04,0x8E,0x02, 0,0, 0,0, 0,0,0x50,0x49,0x44,0x56,0x49,0x44 };
static void guid_axis(uint8_t *g, uint32_t d1) {          /* the A36D02xx-C9F3-11CF-BFC7-444553540000 family */
    static const uint8_t tail[12] = { 0xF3,0xC9,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
    memcpy(g, &d1, 4); memcpy(g + 4, tail, 12);
}
#define GUID_XAXIS 0xA36D02E0u
#define GUID_YAXIS 0xA36D02E1u
#define GUID_ZAXIS 0xA36D02E2u
#define GUID_RZAXIS 0xA36D02E3u
#define GUID_BUTTON 0xA36D02F0u
#define GUID_POV 0xA36D02F2u
#define GUID_RXAXIS 0xA36D02F4u
#define GUID_RYAXIS 0xA36D02F5u
typedef struct { uint32_t guid, ofs, type; const char *name; } joy_obj;
static const joy_obj k_joy_objs[] = {
    { GUID_XAXIS, 0, 0x002, "Left Stick X" }, { GUID_YAXIS, 4, 0x102, "Left Stick Y" }, { GUID_ZAXIS, 8, 0x202, "LT" },
    { GUID_RXAXIS, 12, 0x302, "Right Stick X" }, { GUID_RYAXIS, 16, 0x402, "Right Stick Y" }, { GUID_RZAXIS, 20, 0x502, "RT" },
    { GUID_POV, 32, 0x010, "D-Pad" },
    { GUID_BUTTON, 48, 0x004, "A" }, { GUID_BUTTON, 49, 0x104, "B" }, { GUID_BUTTON, 50, 0x204, "X" }, { GUID_BUTTON, 51, 0x304, "Y" },
    { GUID_BUTTON, 52, 0x404, "LB" }, { GUID_BUTTON, 53, 0x504, "RB" }, { GUID_BUTTON, 54, 0x604, "Back" },
    { GUID_BUTTON, 55, 0x704, "Start" }, { GUID_BUTTON, 56, 0x804, "L3" }, { GUID_BUTTON, 57, 0x904, "R3" },
    { GUID_BUTTON, 58, 0xA04, "LT Button" }, { GUID_BUTTON, 59, 0xB04, "RT Button" },
};
#define JOY_NOBJ (int)(sizeof k_joy_objs / sizeof *k_joy_objs)

static void di7_EnumDevices(void) {                 /* (this, type, cb, ref, flags) */
    uint32_t type = A32(1) & 0xFF;
    fam_input_init();
    if ((type == 0 || type == 4 /* DIDEVTYPE_JOYSTICK */) && fam_pad_present()) {
        uint8_t *di = hle_heap_alloc(0x244, 1);          /* DIDEVICEINSTANCEA */
        uint32_t v = hle_va(di);
        MEM32(v) = 0x244;
        memcpy(di + 4, k_joy_guid, 16); memcpy(di + 20, k_joy_product, 16);
        MEM32(v + 36) = 0x0404;                           /* DIDEVTYPE_JOYSTICK, DIDEVTYPEJOYSTICK_GAMEPAD */
        strcpy((char *)di + 40, "Controller"); strcpy((char *)di + 300, "Xbox 360 Controller");
        uint32_t a[2] = { v, A32(3) };
        hle_call_guest(A32(2), 2, a);                     /* IJoy copies the instance */
        hle_heap_free(di);
        fprintf(stderr, "[dinput] joystick listed for IJoy\n");
    }
    RET(DI_OK, 5);
}
static void di7_CreateDeviceEx(void) {              /* (this, REFGUID, riid, device**, outer) */
    if (memcmp(APTR(1), k_joy_guid, 16)) {           /* keyboard or mouse: as DirectInput 8 */
        uint32_t a[4] = { A32(0), A32(1), A32(3), A32(4) };
        const uint8_t *g = hle_ptr(a[1]);
        int type = guid_is(g, 0x6F1D2B61u) ? DEV_KEYBOARD : guid_is(g, 0x6F1D2B60u) ? DEV_MOUSE : 0;
        if (!type) RET(DIERR_DEVICENOTREG, 5);
        didev *d = calloc(1, sizeof *d); d->type = type; d->obj = fam_com_new(&DID8_iface, d);
        MEM32(a[2]) = d->obj;
        RET(DI_OK, 5);
    }
    didev *d = calloc(1, sizeof *d);
    d->type = DEV_JOY;
    d->obj = fam_com_new(&DIJ_iface, d);
    MEM32(A32(3)) = d->obj;
    RET(DI_OK, 5);
}
static void jd_GetCapabilities(void) {               /* (this, DIDEVCAPS*) */
    uint32_t c = A32(1);
    MEM32(c + 4) = fam_pad_present() ? 1 : 0;        /* DIDC_ATTACHED; no force feedback */
    MEM32(c + 8) = 0x0404;
    MEM32(c + 12) = 6; MEM32(c + 16) = 12; MEM32(c + 20) = 1;   /* axes, buttons, POVs */
    RET(DI_OK, 2);
}
static void jd_EnumObjects(void) {                   /* (this, cb, ref, flags) */
    uint8_t *o = hle_heap_alloc(0x13C, 1);           /* DIDEVICEOBJECTINSTANCEA */
    uint32_t v = hle_va(o);
    for (int i = 0; i < JOY_NOBJ; i++) {
        memset(o, 0, 0x13C);
        MEM32(v) = 0x13C;
        guid_axis(o + 4, k_joy_objs[i].guid);
        MEM32(v + 20) = k_joy_objs[i].ofs;
        MEM32(v + 24) = k_joy_objs[i].type;
        MEM32(v + 28) = (k_joy_objs[i].type & 3) ? 0x100 : 0;   /* axes: DIDOI_ASPECTPOSITION */
        strcpy((char *)o + 32, k_joy_objs[i].name);
        uint32_t a[2] = { v, A32(2) };
        if (!hle_call_guest(A32(1), 2, a)) break;         /* DIENUM_STOP */
    }
    hle_heap_free(o);
    RET(DI_OK, 4);
}
static void jd_GetProperty(void) {                   /* (this, REFGUID prop, DIPROPHEADER*) */
    uint32_t h = A32(2);
    if (A32(1) == 4 /* DIPROP_RANGE */) { MEM32(h + 16) = (uint32_t)-10000; MEM32(h + 20) = 10000; }
    else if (MEM32(h) >= 20) MEM32(h + 16) = 0;
    RET(DI_OK, 3);
}
static void jd_GetDeviceState(void) {                /* (this, size, DIJOYSTATE2*) */
    uint32_t n = A32(1), p = A32(2);
    fam_input_pump();
    uint8_t st[272] = {0};
    fam_pad pad; fam_pad_read(&pad);
    int32_t ax[6] = { pad.lx, pad.ly, pad.lt, pad.rx, pad.ry, pad.rt };
    memcpy(st, ax, sizeof ax);
    uint32_t b = pad.buttons;
    int up = (b >> FAM_PB_UP) & 1, dn = (b >> FAM_PB_DOWN) & 1, lf = (b >> FAM_PB_LEFT) & 1, rt = (b >> FAM_PB_RIGHT) & 1;
    int32_t pov = -1;                                 /* hundredths of a degree clockwise from up; -1 centred */
    if (up && rt) pov = 4500; else if (dn && rt) pov = 13500; else if (dn && lf) pov = 22500; else if (up && lf) pov = 31500;
    else if (up) pov = 0; else if (rt) pov = 9000; else if (dn) pov = 18000; else if (lf) pov = 27000;
    memcpy(st + 32, &pov, 4);
    for (int i = 1; i < 4; i++) { int32_t c = -1; memcpy(st + 32 + 4 * i, &c, 4); }
    for (int i = 0; i <= FAM_PB_RT; i++) st[48 + i] = (b >> i) & 1 ? 0x80 : 0;
    memcpy(hle_ptr(p), st, n < sizeof st ? n : sizeof st);
    RET(DI_OK, 3);
}
static void jd_Ok1(void) { RET(DI_OK, 1); }
static void jd_Ok2(void) { RET(DI_OK, 2); }
static void jd_Ok3(void) { RET(DI_OK, 3); }
static void jd_NoFF(void) { RET(0x80004001u /* E_NOTIMPL */, 3); }


static void s_DirectInputCreateEx(void) {            /* (hinst, version, riid, out**, outer) */
    static uint32_t obj;
    if (!obj) {
        static const fam_method di[] = {
            { IDIRECTINPUT7A_QueryInterface, di_QueryInterface }, { IDIRECTINPUT7A_AddRef, di_AddRef },
            { IDIRECTINPUT7A_Release, di_Release }, { IDIRECTINPUT7A_EnumDevices, di7_EnumDevices },
            { IDIRECTINPUT7A_Initialize, di_Initialize }, { IDIRECTINPUT7A_CreateDeviceEx, di7_CreateDeviceEx },
            { IDIRECTINPUT7A_GetDeviceStatus, di_GetDeviceStatus }, { 0, 0 } };
        fam_iface_vtable(&DI7_iface, di);
        static const fam_method jd[] = {
            { IDIRECTINPUTDEVICE8A_QueryInterface, di_QueryInterface }, { IDIRECTINPUTDEVICE8A_AddRef, di_AddRef },
            { IDIRECTINPUTDEVICE8A_Release, dd_Release }, { IDIRECTINPUTDEVICE8A_GetCapabilities, jd_GetCapabilities },
            { IDIRECTINPUTDEVICE8A_EnumObjects, jd_EnumObjects }, { IDIRECTINPUTDEVICE8A_GetProperty, jd_GetProperty },
            { IDIRECTINPUTDEVICE8A_SetProperty, jd_Ok3 }, { IDIRECTINPUTDEVICE8A_Acquire, jd_Ok1 },
            { IDIRECTINPUTDEVICE8A_Unacquire, jd_Ok1 }, { IDIRECTINPUTDEVICE8A_GetDeviceState, jd_GetDeviceState },
            { IDIRECTINPUTDEVICE8A_SetDataFormat, jd_Ok2 }, { IDIRECTINPUTDEVICE8A_SetCooperativeLevel, jd_Ok3 },
            { IDIRECTINPUTDEVICE8A_SetEventNotification, jd_Ok2 }, { IDIRECTINPUTDEVICE8A_Poll, jd_Ok1 },
            { IDIRECTINPUTDEVICE8A_CreateEffect, jd_NoFF }, { IDIRECTINPUTDEVICE8A_SendForceFeedbackCommand, jd_Ok2 }, { 0, 0 } };
        DIJ_iface.iface = "IDirectInputDevice7A(joystick)";   /* shims register by name: not the keyboard/mouse ones */
        fam_iface_vtable(&DIJ_iface, jd);
        obj = fam_com_new(&DI7_iface, NULL);
    } else fam_com_addref(obj);
    MEM32(A32(3)) = obj;
    RET(DI_OK, 5);
}

static const win32hle_shim g_dinput[] = {
    { "dinput8.dll!DirectInput8Create", s_DirectInput8Create },
    { "dinput.dll!DirectInputCreateEx", s_DirectInputCreateEx },
    { 0, 0 }
};
void fam_register_dinput(void) { win32hle_register(g_dinput); }
