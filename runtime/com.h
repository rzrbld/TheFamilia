/*
 * com.h - guest-visible COM objects for the port's DirectX shims.
 *
 * An object lives in guest memory: [vtable VA][refcount][host pointer slot]...
 * The vtable is an array of shim VAs. Every slot starts as a stub that logs
 * "<Iface>::<Method> unimplemented" once, pops the method's stdcall
 * arguments (counted from the public header, tools/com_vtables.py) and
 * returns S_OK; real implementations override slots by index.
 */
#pragma once
#include <stdint.h>
#include "win32hle.h"

#define S_OK          0u
#define E_FAIL        0x80004005u
#define E_NOINTERFACE 0x80004002u
#define E_OUTOFMEMORY 0x8007000Eu
#define E_INVALIDARG  0x80070057u

typedef struct {
    const char *iface;
    int nslots;
    const char *const *names;
    const unsigned char *argc;
    const recomp_func_t *stubs;    /* one generated stub per slot */
    uint32_t vtable;               /* guest VA, built on first use */
} fam_iface;

typedef struct { int slot; recomp_func_t fn; } fam_method;

/* Build (once) the vtable for an interface, overriding the listed slots. */
uint32_t fam_iface_vtable(fam_iface *it, const fam_method *impl);
/* A new object: vtable, refcount 1, host pointer, and `extra` zeroed bytes. */
uint32_t fam_com_new(fam_iface *it, void *host);
void    *fam_com_host(uint32_t obj);      /* the host pointer stored at +8 */
uint32_t fam_com_addref(uint32_t obj);
uint32_t fam_com_release(uint32_t obj);   /* returns the new count; the caller frees at 0 */
void     fam_com_unimpl(fam_iface *it, int slot);

/* Declare the per-slot stubs and the fam_iface P_iface for interface I
 * (slot header from tools/com_vtables.py, UP = upper-case name). */
#define FAM_STUB_DEF(P, n) static void P##_stub_##n(void) { fam_com_unimpl(&P##_iface, n); }
#define FAM_STUB_PTR(P, n) P##_stub_##n,
#define FAM_DEFINE_IFACE(P, I, UP)                                                  \
    static fam_iface P##_iface;                                                     \
    UP##_FOR_EACH(FAM_STUB_DEF, P)                                                  \
    static const recomp_func_t P##_stubs[] = { UP##_FOR_EACH(FAM_STUB_PTR, P) };    \
    static fam_iface P##_iface = { #I, UP##_SLOTS, I##_slot_names, I##_slot_argc, P##_stubs, 0 };
