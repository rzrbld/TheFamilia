/*
 * vorbis.c - vorbisfile.dll for LS3DF.dll on the system libvorbisfile.
 *
 * The guest's OggVorbis_File (i386 layout, opaque to LS3DF) only needs to
 * identify the stream: its first dword holds a handle to the native one.
 * Guest I/O callbacks (read/seek/close/tell, cdecl) are called back into
 * lifted code through a per-stream guest buffer. All exports are cdecl, so
 * shims return without popping arguments. ov_time_tell returns a double in
 * x87 ST0, which for lifted code is the global FPU stack.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisfile.h>
#include "win32hle.h"
#define RECOMP_GENERATED_CODE   /* for _st/_fp_top aliases used by fp_push */
#include "recomp_types.h"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef esi
#undef edi
#undef ebp

#define MAXS 64
#define IOBUF 32768
typedef struct {
    int used;
    OggVorbis_File vf;
    uint32_t ds, cb_read, cb_seek, cb_close, cb_tell;
    uint32_t gbuf;          /* guest scratch for reads */
    uint32_t ginfo;         /* guest vorbis_info copy */
} stream;
static stream g_s[MAXS];

static size_t io_read(void *ptr, size_t size, size_t nmemb, void *src) {
    stream *s = src;
    size_t want = size * nmemb, done = 0;
    while (done < want) {
        uint32_t n = (uint32_t)(want - done > IOBUF ? IOBUF : want - done);
        uint32_t a[4] = { s->gbuf, 1, n, s->ds };
        uint32_t got = hle_call_guest(s->cb_read, 4, a);
        if (!got || got > n) break;
        memcpy((uint8_t *)ptr + done, hle_ptr(s->gbuf), got);
        done += got;
        if (got < n) break;
    }
    return size ? done / size : 0;
}
static int io_seek(void *src, ogg_int64_t off, int whence) {
    stream *s = src;
    if (!s->cb_seek) return -1;
    uint32_t a[4] = { s->ds, (uint32_t)off, (uint32_t)((uint64_t)off >> 32), (uint32_t)whence };
    return (int)hle_call_guest(s->cb_seek, 4, a);
}
static int io_close(void *src) {
    stream *s = src;
    if (!s->cb_close) return 0;
    uint32_t a[1] = { s->ds };
    return (int)hle_call_guest(s->cb_close, 1, a);
}
static long io_tell(void *src) {
    stream *s = src;
    if (!s->cb_tell) return -1;
    uint32_t a[1] = { s->ds };
    return (long)(int32_t)hle_call_guest(s->cb_tell, 1, a);
}

static stream *of(uint32_t gvf) {
    uint32_t h = MEM32(gvf);
    return h >= 1 && h <= MAXS && g_s[h - 1].used ? &g_s[h - 1] : NULL;
}

/* cdecl: arguments start at esp+4, the caller pops them */
#define CA(i) MEM32(g_esp + 4 + 4 * (i))
#define CRET(v) do { g_eax = (uint32_t)(v); g_esp += 4; return; } while (0)

static void v_open_callbacks(void) {   /* (ds, vf, initial, ibytes, {read, seek, close, tell}) */
    int i = 0;
    while (i < MAXS && g_s[i].used) i++;
    if (i == MAXS) CRET(OV_EFAULT);
    stream *s = &g_s[i];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->ds = CA(0); s->cb_read = CA(4); s->cb_seek = CA(5); s->cb_close = CA(6); s->cb_tell = CA(7);
    if (!s->gbuf) s->gbuf = hle_va(hle_heap_alloc(IOBUF, 0));
    uint32_t gvf = CA(1);
    MEM32(gvf) = (uint32_t)(i + 1);
    ov_callbacks cb = { io_read, s->cb_seek ? io_seek : NULL, io_close, s->cb_tell ? io_tell : NULL };
    int r = ov_open_callbacks(s, &s->vf, CA(3) ? (const char *)hle_ptr(CA(2)) : NULL, (long)(int32_t)CA(3), cb);
    if (r < 0) { s->used = 0; MEM32(gvf) = 0; fprintf(stderr, "[vorbis] ov_open_callbacks failed (%d)\n", r); }
    CRET(r);
}
static void v_clear(void) {
    stream *s = of(CA(0));
    if (s) { ov_clear(&s->vf); s->used = 0; MEM32(CA(0)) = 0; }
    CRET(0);
}
static void v_info(void) {             /* (vf, link) -> vorbis_info* (i386 layout, 32 bytes) */
    stream *s = of(CA(0));
    if (!s) CRET(0);
    vorbis_info *vi = ov_info(&s->vf, (int)CA(1));
    if (!vi) CRET(0);
    if (!s->ginfo) s->ginfo = hle_va(hle_heap_alloc(32, 1));
    uint32_t g = s->ginfo;
    MEM32(g) = (uint32_t)vi->version; MEM32(g + 4) = (uint32_t)vi->channels; MEM32(g + 8) = (uint32_t)vi->rate;
    MEM32(g + 12) = (uint32_t)vi->bitrate_upper; MEM32(g + 16) = (uint32_t)vi->bitrate_nominal;
    MEM32(g + 20) = (uint32_t)vi->bitrate_lower; MEM32(g + 24) = (uint32_t)vi->bitrate_window; MEM32(g + 28) = 0;
    CRET(g);
}
static void v_read(void) {             /* (vf, buffer, length, bigendianp, word, sgned, int *bitstream) */
    stream *s = of(CA(0));
    if (!s) CRET(OV_EINVAL);
    int bs = 0;
    long r = ov_read(&s->vf, (char *)hle_ptr(CA(1)), (int)CA(2), (int)CA(3), (int)CA(4), (int)CA(5), &bs);
    if (CA(6)) MEM32(CA(6)) = (uint32_t)bs;
    CRET(r);
}
static void v_time_seek(void) {        /* (vf, double s) */
    stream *s = of(CA(0));
    double t; uint64_t b = (uint64_t)CA(1) | ((uint64_t)CA(2) << 32); memcpy(&t, &b, 8);
    CRET(s ? ov_time_seek(&s->vf, t) : OV_EINVAL);
}
static void v_time_tell(void) {        /* (vf) -> double in ST0 */
    stream *s = of(CA(0));
    fp_push_impl(g_st, &g_fp_top, s ? ov_time_tell(&s->vf) : 0.0);
    g_esp += 4;
}

static const win32hle_shim g_vorbis[] = {
    { "vorbisfile.dll!ov_open_callbacks", v_open_callbacks }, { "vorbisfile.dll!ov_clear", v_clear },
    { "vorbisfile.dll!ov_info", v_info }, { "vorbisfile.dll!ov_read", v_read },
    { "vorbisfile.dll!ov_time_seek", v_time_seek }, { "vorbisfile.dll!ov_time_tell", v_time_tell },
    { 0, 0 }
};
void fam_register_vorbis(void) { win32hle_register(g_vorbis); }
