/*
 * dsound8.c - DirectSound 8 and 3D sound for LS3DF.dll on top of win32hle's
 * software mixer (dsound.c).
 *
 * win32hle answers DirectSound 1 and keys each buffer's host state by its
 * guest object. This layer keeps those objects but installs DS8 vtables on
 * them: most slots tail-call win32hle's shim with the stack untouched, while
 * QueryInterface hands out real IDirectSound3DBuffer / IDirectSound3DListener
 * objects (win32hle's own QueryInterface returns the same object for any IID,
 * which would send 3D calls into 2D slots). EAX (IKsPropertySet) is refused.
 *
 * 3D model: DirectSound's inverse-distance rolloff between min and max
 * distance, and a pan from the source direction along the listener's right
 * vector; applied as volume/pan on the underlying buffer, combined with the
 * volume the game set itself. Doppler and cones are not modelled.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "com.h"
#include "dsound_slots.h"
extern const char *g_cur_import;

FAM_DEFINE_IFACE(DS8, IDirectSound8, IDIRECTSOUND8)
FAM_DEFINE_IFACE(DSB8, IDirectSoundBuffer8, IDIRECTSOUNDBUFFER8)
FAM_DEFINE_IFACE(DS3D, IDirectSound3DBuffer, IDIRECTSOUND3DBUFFER)
FAM_DEFINE_IFACE(DSL, IDirectSound3DListener, IDIRECTSOUND3DLISTENER)

#define DS_OK 0u
#define DSBCAPS_CTRL3D 0x10u
#define DS3DMODE_NORMAL 0u
#define DS3DMODE_HEADRELATIVE 1u
#define DS3DMODE_DISABLE 2u

typedef struct {
    uint32_t buf;              /* win32hle buffer object */
    uint32_t obj3d;            /* our 3D interface object (0 until queried) */
    int app_volume, app_pan;   /* what the game itself set */
    int is3d;
    float pos[3], minD, maxD;
    uint32_t mode;
    int playing;               /* in g_play[] */
} sbuf;

/* Indexed by win32hle's buffer slot (stored at object +8), same capacity as
 * its mixer (MAX_BUFS). Listener moves only re-spatialize the 3D buffers in
 * g_play, the ones started with Play and not yet seen stopped. */
#define MAXB 4096
static sbuf g_b[MAXB];
static uint16_t g_play[MAXB];
static int g_nplay;
int hle_dsound_is_playing(uint32_t self);
int hle_dsound_alive(uint32_t self);
static struct { float pos[3], front[3], top[3], rolloff, dist_factor; uint32_t obj; } g_l = {
    {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1.f, 1.f, 0 };

/* win32hle shims by name. hle_resolve is a linear strcmp search over every
 * registered shim; positional-sound updates call this constantly, so each
 * name (always a string literal here) is resolved once and cached by its
 * address. */
static uint32_t shim(const char *name) {
    static struct { const char *name; uint32_t va; } cache[32];
    static int n;
    for (int i = 0; i < n; i++) if (cache[i].name == name) return cache[i].va;
    uint32_t v = hle_resolve(name);
    if (!v) hle_fatal("dsound8: no shim %s", name);
    if (n < 32) { cache[n].name = name; cache[n].va = v; n++; }
    return v;
}
static void call2(const char *name, uint32_t obj, uint32_t a) { uint32_t args[2] = { obj, a }; hle_call_guest(shim(name), 2, args); }

static sbuf *find(uint32_t buf) {
    uint32_t slot = buf ? MEM32(buf + 8) : MAXB;
    return slot < MAXB && g_b[slot].buf == buf ? &g_b[slot] : NULL;
}
static void play_remove(sbuf *s) {
    if (!s->playing) return;
    uint16_t idx = (uint16_t)(s - g_b);
    for (int i = 0; i < g_nplay; i++) if (g_play[i] == idx) { g_play[i] = g_play[--g_nplay]; break; }
    s->playing = 0;
}
static void play_add(sbuf *s) {
    if (s->playing || !s->is3d || g_nplay >= MAXB) return;
    g_play[g_nplay++] = (uint16_t)(s - g_b);
    s->playing = 1;
}
static sbuf *track(uint32_t buf, int is3d) {
    sbuf *s = find(buf);
    if (s) return s;
    uint32_t slot = MEM32(buf + 8);
    if (slot >= MAXB) hle_fatal("dsound8: buffer slot %u out of range", slot);
    s = &g_b[slot];
    if (s->buf) play_remove(s);               /* a stale entry for a freed object */
    memset(s, 0, sizeof *s);
    s->buf = buf; s->is3d = is3d; s->minD = 1.f; s->maxD = 1e9f; s->mode = DS3DMODE_NORMAL;
    return s;
}

/* recompute volume/pan for one buffer */
static void spatialize(sbuf *s) {
    int vol = s->app_volume, pan = s->app_pan;
    if (s->is3d && s->mode != DS3DMODE_DISABLE) {
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = s->mode == DS3DMODE_HEADRELATIVE ? s->pos[i] : s->pos[i] - g_l.pos[i];
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * g_l.dist_factor;
        float de = dist > s->maxD ? s->maxD : dist;
        float g = de <= s->minD ? 1.f : s->minD / (s->minD + g_l.rolloff * (de - s->minD));
        int att = g <= 1e-5f ? -10000 : (int)(2000.f * log10f(g));
        vol = vol + att < -10000 ? -10000 : vol + att;
        /* right = top x front (DirectSound is left-handed) */
        const float *t = g_l.top, *f = g_l.front;
        float r[3] = { t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2], t[0] * f[1] - t[1] * f[0] };
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        float side = 0.f;
        if (s->mode == DS3DMODE_HEADRELATIVE) side = dist > 1e-4f ? d[0] / (dist / g_l.dist_factor) : 0.f;
        else if (rl > 0 && dist > 1e-4f) side = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (rl * dist / g_l.dist_factor);
        pan += (int)(side * 3000.f);
        if (pan < -10000) pan = -10000; if (pan > 10000) pan = 10000;
    }
    call2("IDirectSoundBuffer::SetVolume", s->buf, (uint32_t)vol);
    call2("IDirectSoundBuffer::SetPan", s->buf, (uint32_t)pan);
}
static void spatialize_all(void) {
    for (int i = 0; i < g_nplay; ) {
        sbuf *s = &g_b[g_play[i]];
        if (!s->buf || !hle_dsound_is_playing(s->buf)) { s->playing = 0; g_play[i] = g_play[--g_nplay]; continue; }
        spatialize(s);
        i++;
    }
}

/* ---- forwarding: jump into win32hle's shim with the guest stack as is ---- */
/* recomp_lookup_import returns win32hle's generic shim trampoline and arms it
 * with the target's index, so it must be looked up right before each call
 * (a cached pointer would re-enter whichever shim is running: this one). */
#define FWD(fn, name) static void fn(void) { static uint32_t va; if (!va) va = shim(name); recomp_lookup_import(va)(); }
FWD(f_ds_SetCooperativeLevel, "IDirectSound::SetCooperativeLevel")
FWD(f_ds_Compact, "IDirectSound::Compact")
FWD(f_ds_GetSpeakerConfig, "IDirectSound::GetSpeakerConfig")
FWD(f_ds_SetSpeakerConfig, "IDirectSound::SetSpeakerConfig")
FWD(f_ds_Initialize, "IDirectSound::Initialize")
FWD(f_ds_AddRef, "IDirectSound::AddRef")
FWD(f_ds_Release, "IDirectSound::Release")
FWD(f_b_AddRef, "IDirectSoundBuffer::AddRef")
FWD(f_b_GetCaps, "IDirectSoundBuffer::GetCaps")
FWD(f_b_GetCurrentPosition, "IDirectSoundBuffer::GetCurrentPosition")
FWD(f_b_GetFormat, "IDirectSoundBuffer::GetFormat")
FWD(f_b_GetFrequency, "IDirectSoundBuffer::GetFrequency")
FWD(f_b_GetStatus_raw, "IDirectSoundBuffer::GetStatus")
FWD(f_b_Initialize, "IDirectSoundBuffer::Initialize")
FWD(f_b_Lock, "IDirectSoundBuffer::Lock")
FWD(f_b_Play_raw, "IDirectSoundBuffer::Play")
FWD(f_b_SetCurrentPosition, "IDirectSoundBuffer::SetCurrentPosition")
FWD(f_b_SetFormat, "IDirectSoundBuffer::SetFormat")
FWD(f_b_SetFrequency, "IDirectSoundBuffer::SetFrequency")
FWD(f_b_Stop_raw, "IDirectSoundBuffer::Stop")
FWD(f_b_Unlock, "IDirectSoundBuffer::Unlock")
FWD(f_b_Restore, "IDirectSoundBuffer::Restore")
FWD(f_b_Release_raw, "IDirectSoundBuffer::Release")

static uint32_t g_ds_vt, g_b_vt;
static void adopt_buffer(uint32_t obj, int is3d) { MEM32(obj) = g_b_vt; track(obj, is3d); }

static void f_ds_QueryInterface(void) { if (A32(2)) MEM32(A32(2)) = A32(0); MEM32(A32(0) + 4)++; RET(DS_OK, 3); }
static void f_ds_CreateSoundBuffer(void) {            /* (this, DSBUFFERDESC*, out**, outer) */
    uint32_t desc = A32(1), out = A32(2);
    int is3d = desc && (MEM32(desc + 4) & DSBCAPS_CTRL3D);
    uint32_t a[4] = { A32(0), desc, out, A32(3) };
    uint32_t r = hle_call_guest(shim("IDirectSound::CreateSoundBuffer"), 4, a);
    if (r == DS_OK && out && MEM32(out)) adopt_buffer(MEM32(out), is3d);
    {   /* diagnostics: every failure, and the first few successes */
        static int nok;
        uint32_t wfx = desc ? MEM32(desc + 16) : 0;
        if (r != DS_OK || nok++ < 6)
            fprintf(stderr, "[dsound8] CreateSoundBuffer flags %08X bytes %u fmt tag %u ch %u rate %u bits %u -> %08X\n",
                    desc ? MEM32(desc + 4) : 0, desc ? MEM32(desc + 8) : 0, wfx ? MEM16(wfx) : 0, wfx ? MEM16(wfx + 2) : 0,
                    wfx ? MEM32(wfx + 4) : 0, wfx ? MEM16(wfx + 14) : 0, r);
    }
    RET(r, 4);
}
static void f_ds_DuplicateSoundBuffer8(void) {        /* (this, src, out**) */
    uint32_t a[3] = { A32(0), A32(1), A32(2) };
    uint32_t r = hle_call_guest(shim("IDirectSound::DuplicateSoundBuffer"), 3, a);
    if (r != DS_OK) fprintf(stderr, "[dsound8] DuplicateSoundBuffer(0x%08X) -> %08X\n", A32(1), r);
    if (r == DS_OK && MEM32(A32(2))) { sbuf *src = find(A32(1)); adopt_buffer(MEM32(A32(2)), src ? src->is3d : 0); }
    RET(r, 3);
}
/* GetCaps: win32hle reports mixing voices but no 3D voices. LS3D sizes its 3D
 * sound pool from dwFreeHw3DAllBuffers ("0 free 3D channels claimed") and
 * then cannot create positional sounds, leaving NULL buffers it later uses.
 * This layer implements 3D (see spatialize), so report 3D voices too.
 * DSCAPS offsets: 44 MaxHw3DAll, 48 MaxHw3DStatic, 52 MaxHw3DStreaming,
 * 56 FreeHw3DAll, 60 FreeHw3DStatic, 64 FreeHw3DStreaming. */
static void f_ds_GetCaps(void) {                       /* (this, DSCAPS*) */
    uint32_t c = A32(1);
    uint32_t a[2] = { A32(0), c };
    uint32_t r = hle_call_guest(shim("IDirectSound::GetCaps"), 2, a);
    if (r == DS_OK && c && MEM32(c) >= 68)
        for (uint32_t off = 44; off <= 64; off += 4) MEM32(c + off) = 64;
    RET(r, 2);
}
static void f_ds_VerifyCertification(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DS_OK, 2); }   /* DS_UNCERTIFIED */

/* buffers */
static const uint32_t IID_3DBUFFER = 0x279AFA86u, IID_3DLISTENER = 0x279AFA84u, IID_DSB = 0x279AFA85u, IID_DSB8 = 0x6825A449u;
static void f_b_QueryInterface(void) {                /* (this, riid, out**) */
    uint32_t iid = MEM32(A32(1)), self = A32(0), out = A32(2);
    sbuf *s = find(self);
    if (iid == IID_DSB || iid == IID_DSB8 || iid == 0 /* IUnknown */) { MEM32(self + 4)++; MEM32(out) = self; RET(DS_OK, 3); }
    /* 3D interfaces share their buffer's reference count, as in DirectSound:
     * querying one adds a reference to the buffer, releasing it drops one. */
    if (iid == IID_3DBUFFER && !s) {          /* any buffer with our vtable is a real one: track it now */
        static int once; if (!once++) fprintf(stderr, "[dsound8] 3D interface asked of an untracked buffer 0x%08X: tracking it\n", self);
        s = track(self, 1);
    }
    if (iid == IID_3DBUFFER && s) {
        if (!s->obj3d) s->obj3d = fam_com_new(&DS3D_iface, (void *)(uintptr_t)self);
        s->is3d = 1;
        MEM32(self + 4)++;
        MEM32(out) = s->obj3d; RET(DS_OK, 3);
    }
    if (iid == IID_3DLISTENER) {                       /* asked of the primary buffer */
        if (!g_l.obj) g_l.obj = fam_com_new(&DSL_iface, (void *)(uintptr_t)self);
        else { uint64_t h = (uint64_t)self; memcpy(hle_ptr(g_l.obj + 8), &h, 8); }
        MEM32(self + 4)++;
        MEM32(out) = g_l.obj; RET(DS_OK, 3);
    }
    {   /* refused: say which interface, once per IID */
        static uint32_t seen[16]; static int ns; int k = 0;
        while (k < ns && seen[k] != iid) k++;
        if (k == ns && ns < 16) { seen[ns++] = iid; fprintf(stderr, "[dsound8] QueryInterface: no interface %08X on buffer 0x%08X\n", iid, self); }
    }
    if (out) MEM32(out) = 0;
    RET(E_NOINTERFACE, 3);
}
static void f_b_SetVolume(void) {
    sbuf *s = find(A32(0));
    if (s) { s->app_volume = (int)A32(1); spatialize(s); RET(DS_OK, 2); }
    call2("IDirectSoundBuffer::SetVolume", A32(0), A32(1)); RET(DS_OK, 2);
}
static void f_b_SetPan(void) {
    sbuf *s = find(A32(0));
    if (s) { s->app_pan = (int)A32(1); spatialize(s); RET(DS_OK, 2); }
    call2("IDirectSoundBuffer::SetPan", A32(0), A32(1)); RET(DS_OK, 2);
}
static void f_b_GetVolume(void) { sbuf *s = find(A32(0)); MEM32(A32(1)) = s ? (uint32_t)s->app_volume : 0; RET(DS_OK, 2); }
static void f_b_GetPan(void)    { sbuf *s = find(A32(0)); MEM32(A32(1)) = s ? (uint32_t)s->app_pan : 0; RET(DS_OK, 2); }
static void f_b_Release(void) {
    uint32_t self = A32(0);
    if (MEM32(self + 4) == 0) {                       /* over-release: never wrap the count */
        static int once; if (!once++) fprintf(stderr, "[dsound8] Release of a buffer with no references (0x%08X)\n", self);
        RET(0, 1);
    }
    if (MEM32(self + 4) == 1) {
        static int nfree;
        if (nfree++ < 400) fprintf(stderr, "[dsound8] free buffer 0x%08X (%s)\n", self, g_cur_import ? g_cur_import : "?");
        uint64_t z = 0;
        sbuf *s = find(self);
        if (s) { if (s->obj3d) memcpy(hle_ptr(s->obj3d + 8), &z, 8); s->buf = 0; }
        if (g_l.obj && (uint32_t)(uintptr_t)fam_com_host(g_l.obj) == self) memcpy(hle_ptr(g_l.obj + 8), &z, 8);
    }
    f_b_Release_raw();
}
static void f_b_Play(void) {                          /* (this, reserved, priority, flags) */
    sbuf *s = find(A32(0));
    if (s && s->is3d) { spatialize(s); play_add(s); }  /* position applied before the first sample */
    f_b_Play_raw();
}
static void f_b_Stop(void) {
    sbuf *s = find(A32(0));
    if (s) play_remove(s);
    f_b_Stop_raw();
}
static void f_b_GetStatus(void) {                     /* (this, DWORD *status) */
    if (!hle_dsound_alive(A32(0))) {                    /* released earlier: reads as stopped, like Windows */
        if (A32(1)) MEM32(A32(1)) = 0;
        RET(DS_OK, 2);
    }
    f_b_GetStatus_raw();
}
static void f_b_SetFX(void) { RET(0x80004001u /* E_NOTIMPL */, 4); }

/* 3D buffer: the underlying buffer VA is kept in the object's host slot */
static sbuf *of3d(void) { uint32_t b = (uint32_t)(uintptr_t)fam_com_host(A32(0)); return b ? find(b) : NULL; }
static float fa(int i) { float f; uint32_t u = A32(i); memcpy(&f, &u, 4); return f; }
static void d3_SetPosition(void) { sbuf *s = of3d(); if (s) { s->pos[0] = fa(1); s->pos[1] = fa(2); s->pos[2] = fa(3); spatialize(s); } RET(DS_OK, 5); }
static void d3_SetMinDistance(void) { sbuf *s = of3d(); if (s) { s->minD = fa(1) > 0 ? fa(1) : 1e-3f; spatialize(s); } RET(DS_OK, 3); }
static void d3_SetMaxDistance(void) { sbuf *s = of3d(); if (s) { s->maxD = fa(1); spatialize(s); } RET(DS_OK, 3); }
static void d3_SetMode(void) { sbuf *s = of3d(); if (s) { s->mode = A32(1); spatialize(s); } RET(DS_OK, 3); }
static void d3_GetMode(void) { sbuf *s = of3d(); MEM32(A32(1)) = s ? s->mode : 0; RET(DS_OK, 2); }
static void d3_GetPosition(void) { sbuf *s = of3d(); if (s) memcpy(APTR(1), s->pos, 12); RET(DS_OK, 2); }
static void d3_GetMinDistance(void) { sbuf *s = of3d(); if (s) memcpy(APTR(1), &s->minD, 4); RET(DS_OK, 2); }
static void d3_GetMaxDistance(void) { sbuf *s = of3d(); if (s) memcpy(APTR(1), &s->maxD, 4); RET(DS_OK, 2); }
static void d3_SetAllParameters(void) {   /* DS3DBUFFER: size, pos(3), vel(3), cone in/out, orient(3), outvol, min, max, mode */
    sbuf *s = of3d(); uint32_t p = A32(1);
    if (s) { memcpy(s->pos, hle_ptr(p + 4), 12); memcpy(&s->minD, hle_ptr(p + 52), 4); memcpy(&s->maxD, hle_ptr(p + 56), 4); s->mode = MEM32(p + 60); spatialize(s); }
    RET(DS_OK, 3);
}
static void d3_AddRef(void) {
    uint32_t b = (uint32_t)(uintptr_t)fam_com_host(A32(0));
    RET(b ? ++MEM32(b + 4) : 0, 1);
}
static void d3_Release(void) {                       /* the parent buffer's Release, same stack shape */
    uint32_t b = (uint32_t)(uintptr_t)fam_com_host(A32(0));
    { static int n3; if (n3++ < 200) fprintf(stderr, "[dsound8] 3D Release 0x%08X -> buffer 0x%08X refs %u\n", A32(0), b, b ? MEM32(b + 4) : 0); }
    if (!b) RET(0, 1);
    MEM32(g_esp + 4) = b;
    f_b_Release();
}

/* listener */
static void l_SetPosition(void) { g_l.pos[0] = fa(1); g_l.pos[1] = fa(2); g_l.pos[2] = fa(3); if (!(A32(4) & 1)) spatialize_all(); RET(DS_OK, 5); }
static void l_SetOrientation(void) {
    g_l.front[0] = fa(1); g_l.front[1] = fa(2); g_l.front[2] = fa(3); g_l.top[0] = fa(4); g_l.top[1] = fa(5); g_l.top[2] = fa(6);
    if (!(A32(7) & 1)) spatialize_all();
    RET(DS_OK, 8);
}
static void l_SetRolloffFactor(void) { g_l.rolloff = fa(1); if (!(A32(2) & 1)) spatialize_all(); RET(DS_OK, 3); }
static void l_SetDistanceFactor(void) { g_l.dist_factor = fa(1) > 0 ? fa(1) : 1.f; if (!(A32(2) & 1)) spatialize_all(); RET(DS_OK, 3); }
/* DS3DLISTENER: size, pos(3), vel(3), front(3), top(3), distance, rolloff, doppler */
static void l_GetAllParameters(void) {
    uint32_t p = A32(1); float z[3] = {0, 0, 0}, dop = 1.f;
    MEM32(p) = 64;
    memcpy(hle_ptr(p + 4), g_l.pos, 12); memcpy(hle_ptr(p + 16), z, 12);
    memcpy(hle_ptr(p + 28), g_l.front, 12); memcpy(hle_ptr(p + 40), g_l.top, 12);
    memcpy(hle_ptr(p + 52), &g_l.dist_factor, 4); memcpy(hle_ptr(p + 56), &g_l.rolloff, 4); memcpy(hle_ptr(p + 60), &dop, 4);
    RET(DS_OK, 2);
}
static void l_SetAllParameters(void) {               /* (this, DS3DLISTENER*, apply) */
    uint32_t p = A32(1);
    memcpy(g_l.pos, hle_ptr(p + 4), 12); memcpy(g_l.front, hle_ptr(p + 28), 12); memcpy(g_l.top, hle_ptr(p + 40), 12);
    memcpy(&g_l.dist_factor, hle_ptr(p + 52), 4); memcpy(&g_l.rolloff, hle_ptr(p + 56), 4);
    if (g_l.dist_factor <= 0) g_l.dist_factor = 1.f;
    if (!(A32(2) & 1)) spatialize_all();
    RET(DS_OK, 3);
}
static void d3_QueryInterface(void) {                /* to the buffer's own QueryInterface */
    uint32_t b = (uint32_t)(uintptr_t)fam_com_host(A32(0));
    if (!b) { if (A32(2)) MEM32(A32(2)) = 0; RET(E_NOINTERFACE, 3); }
    MEM32(g_esp + 4) = b;
    f_b_QueryInterface();
}
static void d3_Ignore3(void) { RET(DS_OK, 5); }       /* SetVelocity / SetConeOrientation (x, y, z, apply) */
static void l_Commit(void) { spatialize_all(); RET(DS_OK, 1); }
static void l_GetPosition(void) { memcpy(APTR(1), g_l.pos, 12); RET(DS_OK, 2); }

static void build_vtables(void) {
    if (g_ds_vt) return;
    static const fam_method ds[] = {
        { IDIRECTSOUND8_QueryInterface, f_ds_QueryInterface }, { IDIRECTSOUND8_AddRef, f_ds_AddRef },
        { IDIRECTSOUND8_Release, f_ds_Release }, { IDIRECTSOUND8_CreateSoundBuffer, f_ds_CreateSoundBuffer },
        { IDIRECTSOUND8_GetCaps, f_ds_GetCaps }, { IDIRECTSOUND8_DuplicateSoundBuffer, f_ds_DuplicateSoundBuffer8 },
        { IDIRECTSOUND8_SetCooperativeLevel, f_ds_SetCooperativeLevel }, { IDIRECTSOUND8_Compact, f_ds_Compact },
        { IDIRECTSOUND8_GetSpeakerConfig, f_ds_GetSpeakerConfig }, { IDIRECTSOUND8_SetSpeakerConfig, f_ds_SetSpeakerConfig },
        { IDIRECTSOUND8_Initialize, f_ds_Initialize }, { IDIRECTSOUND8_VerifyCertification, f_ds_VerifyCertification },
        { 0, 0 } };
    static const fam_method b[] = {
        { IDIRECTSOUNDBUFFER8_QueryInterface, f_b_QueryInterface }, { IDIRECTSOUNDBUFFER8_AddRef, f_b_AddRef },
        { IDIRECTSOUNDBUFFER8_Release, f_b_Release }, { IDIRECTSOUNDBUFFER8_GetCaps, f_b_GetCaps },
        { IDIRECTSOUNDBUFFER8_GetCurrentPosition, f_b_GetCurrentPosition }, { IDIRECTSOUNDBUFFER8_GetFormat, f_b_GetFormat },
        { IDIRECTSOUNDBUFFER8_GetVolume, f_b_GetVolume }, { IDIRECTSOUNDBUFFER8_GetPan, f_b_GetPan },
        { IDIRECTSOUNDBUFFER8_GetFrequency, f_b_GetFrequency }, { IDIRECTSOUNDBUFFER8_GetStatus, f_b_GetStatus },
        { IDIRECTSOUNDBUFFER8_Initialize, f_b_Initialize }, { IDIRECTSOUNDBUFFER8_Lock, f_b_Lock },
        { IDIRECTSOUNDBUFFER8_Play, f_b_Play }, { IDIRECTSOUNDBUFFER8_SetCurrentPosition, f_b_SetCurrentPosition },
        { IDIRECTSOUNDBUFFER8_SetFormat, f_b_SetFormat }, { IDIRECTSOUNDBUFFER8_SetVolume, f_b_SetVolume },
        { IDIRECTSOUNDBUFFER8_SetPan, f_b_SetPan }, { IDIRECTSOUNDBUFFER8_SetFrequency, f_b_SetFrequency },
        { IDIRECTSOUNDBUFFER8_Stop, f_b_Stop }, { IDIRECTSOUNDBUFFER8_Unlock, f_b_Unlock },
        { IDIRECTSOUNDBUFFER8_Restore, f_b_Restore }, { IDIRECTSOUNDBUFFER8_SetFX, f_b_SetFX },
        { 0, 0 } };
    static const fam_method d3[] = {
        { IDIRECTSOUND3DBUFFER_QueryInterface, d3_QueryInterface },
        { IDIRECTSOUND3DBUFFER_SetVelocity, d3_Ignore3 }, { IDIRECTSOUND3DBUFFER_SetConeOrientation, d3_Ignore3 },
        { IDIRECTSOUND3DBUFFER_AddRef, d3_AddRef }, { IDIRECTSOUND3DBUFFER_Release, d3_Release },
        { IDIRECTSOUND3DBUFFER_SetPosition, d3_SetPosition }, { IDIRECTSOUND3DBUFFER_GetPosition, d3_GetPosition },
        { IDIRECTSOUND3DBUFFER_SetMinDistance, d3_SetMinDistance }, { IDIRECTSOUND3DBUFFER_GetMinDistance, d3_GetMinDistance },
        { IDIRECTSOUND3DBUFFER_SetMaxDistance, d3_SetMaxDistance }, { IDIRECTSOUND3DBUFFER_GetMaxDistance, d3_GetMaxDistance },
        { IDIRECTSOUND3DBUFFER_SetMode, d3_SetMode }, { IDIRECTSOUND3DBUFFER_GetMode, d3_GetMode },
        { IDIRECTSOUND3DBUFFER_SetAllParameters, d3_SetAllParameters }, { 0, 0 } };
    static const fam_method l[] = {
        { IDIRECTSOUND3DLISTENER_QueryInterface, d3_QueryInterface },
        { IDIRECTSOUND3DLISTENER_GetAllParameters, l_GetAllParameters }, { IDIRECTSOUND3DLISTENER_SetAllParameters, l_SetAllParameters },
        { IDIRECTSOUND3DLISTENER_AddRef, d3_AddRef }, { IDIRECTSOUND3DLISTENER_Release, d3_Release },
        { IDIRECTSOUND3DLISTENER_SetPosition, l_SetPosition }, { IDIRECTSOUND3DLISTENER_GetPosition, l_GetPosition },
        { IDIRECTSOUND3DLISTENER_SetOrientation, l_SetOrientation }, { IDIRECTSOUND3DLISTENER_SetRolloffFactor, l_SetRolloffFactor },
        { IDIRECTSOUND3DLISTENER_SetDistanceFactor, l_SetDistanceFactor },
        { IDIRECTSOUND3DLISTENER_CommitDeferredSettings, l_Commit }, { 0, 0 } };
    g_ds_vt = fam_iface_vtable(&DS8_iface, ds);
    g_b_vt = fam_iface_vtable(&DSB8_iface, b);
    fam_iface_vtable(&DS3D_iface, d3);
    fam_iface_vtable(&DSL_iface, l);
}

static void s_DirectSoundCreate8(void) {   /* (guid, IDirectSound8**, outer) */
    build_vtables();
    uint32_t a[3] = { A32(0), A32(1), A32(2) };
    uint32_t r = hle_call_guest(shim("DirectSoundCreate"), 3, a);
    if (r == DS_OK && A32(1) && MEM32(A32(1))) MEM32(MEM32(A32(1))) = g_ds_vt;
    fprintf(stderr, "[dsound8] DirectSoundCreate8 -> 0x%08X\n", r);
    RET(r, 3);
}

static const win32hle_shim g_ds8[] = { { "dsound.dll#11", s_DirectSoundCreate8 }, { "DirectSoundCreate8", s_DirectSoundCreate8 }, { 0, 0 } };
void fam_register_dsound8(void) { win32hle_register(g_ds8); }
