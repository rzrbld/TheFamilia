/*
 * dinput8.c - DirectInput 8 (keyboard, mouse) for LS3DF.dll, and a
 * DirectInputCreateEx (DX7) for IJoy.dll that reports no joysticks: the
 * controller reaches the game through the keyboard/mouse mapping in input.c,
 * so it never appears twice.
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

#define DI_OK 0u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_DEVICENOTREG 0x80040154u
#define DI_BUFFEROVERFLOW 1u

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2 };
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

/* IJoy.dll (DX7): an interface whose device enumeration finds nothing. */
static void s_DirectInputCreateEx(void) {            /* (hinst, version, riid, out**, outer) */
    static uint32_t obj;
    if (!obj) {
        static const fam_method di[] = {
            { IDIRECTINPUT7A_QueryInterface, di_QueryInterface }, { IDIRECTINPUT7A_AddRef, di_AddRef },
            { IDIRECTINPUT7A_Release, di_Release }, { IDIRECTINPUT7A_EnumDevices, di_EnumDevices },
            { IDIRECTINPUT7A_Initialize, di_Initialize }, { 0, 0 } };
        fam_iface_vtable(&DI7_iface, di);
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
