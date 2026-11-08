/*
 * d3d8_resource.c - textures, surfaces, vertex and index buffers.
 *
 * Every lockable resource keeps its bytes in guest memory (the engine reads
 * and writes them through Lock). GL copies are refreshed lazily: textures at
 * bind time (dirty bit per level), buffers at draw time (dirty byte range).
 * D3D pixel layouts are converted to GLES ones on upload.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_funcs.h"
#include "d3d8_slots.h"
#include "d3d8_internal.h"

FAM_DEFINE_IFACE(Tex, IDirect3DTexture8, IDIRECT3DTEXTURE8)
FAM_DEFINE_IFACE(Surf, IDirect3DSurface8, IDIRECT3DSURFACE8)
FAM_DEFINE_IFACE(VB, IDirect3DVertexBuffer8, IDIRECT3DVERTEXBUFFER8)
FAM_DEFINE_IFACE(IB, IDirect3DIndexBuffer8, IDIRECT3DINDEXBUFFER8)

/* ------------------------------------------------------------- formats */
typedef struct { uint32_t fmt; int bpp; int alpha; } fmt_info;
static const fmt_info k_formats[] = {
    { D3DFMT_A8R8G8B8, 32, 1 }, { D3DFMT_X8R8G8B8, 32, 0 }, { D3DFMT_R5G6B5, 16, 0 },
    { D3DFMT_A1R5G5B5, 16, 1 }, { D3DFMT_X1R5G5B5, 16, 0 }, { D3DFMT_A4R4G4B4, 16, 1 },
    { D3DFMT_L8, 8, 0 }, { D3DFMT_A8, 8, 1 }, { D3DFMT_A8L8, 16, 1 },
};
static const fmt_info *fmt_of(uint32_t f) {
    for (size_t i = 0; i < sizeof k_formats / sizeof *k_formats; i++) if (k_formats[i].fmt == f) return &k_formats[i];
    return NULL;
}
int fam_texture_format_supported(uint32_t usage, uint32_t rtype, uint32_t fmt) {
    if (usage & D3DUSAGE_DEPTHSTENCIL) return fmt == D3DFMT_D16 || fmt == D3DFMT_D24S8 || fmt == D3DFMT_D24X8;
    if (usage & D3DUSAGE_RENDERTARGET) return fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_R5G6B5;
    (void)rtype;
    return fmt_of(fmt) != NULL;
}

/* ---------------------------------------------------------- guest memory */
static uint32_t guest_alloc(uint32_t n) { void *p = hle_heap_alloc(n ? n : 4, 1); return p ? hle_va(p) : 0; }
static void guest_free(uint32_t va) { if (va) hle_heap_free(hle_ptr(va)); }

fam_texture *fam_texture_of(uint32_t obj) { if (!obj) return NULL; fam_texture *t = fam_com_host(obj); return t && t->kind == FAM_RES_TEXTURE ? t : NULL; }
fam_surface *fam_surface_of(uint32_t obj) { if (!obj) return NULL; fam_surface *s = fam_com_host(obj); return s && s->kind == FAM_RES_SURFACE ? s : NULL; }
fam_buffer  *fam_buffer_of(uint32_t obj)  { if (!obj) return NULL; fam_buffer *b = fam_com_host(obj); return b && (b->kind == FAM_RES_VB || b->kind == FAM_RES_IB) ? b : NULL; }

/* ------------------------------------------------------------- surfaces */
static fam_surface *surface_new(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, int lockable) {
    fam_surface *s = calloc(1, sizeof *s);
    s->kind = FAM_RES_SURFACE;
    s->width = w; s->height = h; s->format = fmt; s->usage = usage; s->pool = pool;
    const fmt_info *fi = fmt_of(fmt);
    s->pitch = w * (uint32_t)(fi ? fi->bpp / 8 : 4);
    s->size = s->pitch * h;
    if (lockable) s->mem = guest_alloc(s->size);
    s->obj = fam_com_new(&Surf_iface, s);
    return s;
}
static void s_GetDesc(void) {                        /* (this, D3DSURFACE_DESC*) */
    fam_surface *s = fam_surface_of(A32(0));
    fam_D3DSURFACE_DESC *d = APTR(1);
    memset(d, 0, sizeof *d);
    d->Format = s->format; d->Type = 1 /* D3DRTYPE_SURFACE */; d->Usage = s->usage; d->Pool = s->pool;
    d->Size = s->size; d->Width = s->width; d->Height = s->height;
    RET(D3D_OK, 2);
}
static void s_LockRect(void) {                       /* (this, D3DLOCKED_RECT*, RECT*, flags) */
    fam_surface *s = fam_surface_of(A32(0));
    if (!s->mem) {
        static int once; if (!once++) fprintf(stderr, "[d3d8] LockRect on a non-lockable surface (%ux%u%s)\n",
                                               s->width, s->height, s->is_backbuffer ? ", back buffer" : "");
        RET(D3DERR_INVALIDCALL, 4);
    }
    uint32_t off = 0;
    if (A32(2)) { int32_t *r = APTR(2); const fmt_info *fi = fmt_of(s->format); off = (uint32_t)r[1] * s->pitch + (uint32_t)r[0] * (uint32_t)(fi ? fi->bpp / 8 : 4); }
    MEM32(A32(1)) = s->pitch;
    MEM32(A32(1) + 4) = s->mem + off;
    RET(D3D_OK, 4);
}
static void s_UnlockRect(void) {
    fam_surface *s = fam_surface_of(A32(0));
    s->dirty = 1;
    if (s->parent) s->parent->dirty_levels |= 1u << s->level;
    RET(D3D_OK, 1);
}
static void s_AddRef(void) { fam_surface *s = fam_surface_of(A32(0)); RET(fam_com_addref(s->parent ? s->parent->obj : A32(0)), 1); }
static void s_Release(void) {
    fam_surface *s = fam_surface_of(A32(0));
    if (s->parent) { RET(fam_com_release(s->parent->obj), 1); }   /* levels share their texture's count */
    uint32_t n = fam_com_release(A32(0));
    if (!n && !s->is_backbuffer && !s->is_depth) { guest_free(s->mem); hle_com_free(s->obj); free(s); }
    RET(n, 1);
}
static void s_GetContainer(void) {                   /* (this, riid, void**) */
    fam_surface *s = fam_surface_of(A32(0));
    if (!s->parent) RET(E_NOINTERFACE, 3);
    fam_com_addref(s->parent->obj);
    MEM32(A32(2)) = s->parent->obj;
    RET(D3D_OK, 3);
}
static void s_GetDevice(void) { fam_com_addref(g_dev.obj); MEM32(A32(1)) = g_dev.obj; RET(D3D_OK, 2); }

/* ------------------------------------------------------------- textures */
static uint32_t level_dim(uint32_t d, uint32_t l) { d >>= l; return d ? d : 1; }

static void t_CreateTexture(void) {   /* dev (this, w, h, levels, usage, fmt, pool, IDirect3DTexture8**) */
    uint32_t w = A32(1), h = A32(2), lv = A32(3), usage = A32(4), fmt = A32(5), pool = A32(6);
    if (!fmt_of(fmt) && !(usage & D3DUSAGE_RENDERTARGET)) {
        fprintf(stderr, "[d3d8] CreateTexture: unsupported format 0x%X (%ux%u)\n", fmt, w, h);
        RET(D3DERR_INVALIDCALL, 8);
    }
    uint32_t full = 1; for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) full++;
    if (!lv || lv > full) lv = full;
    fam_texture *t = calloc(1, sizeof *t);
    t->kind = FAM_RES_TEXTURE;
    t->width = w; t->height = h; t->levels = lv; t->format = fmt; t->usage = usage; t->pool = pool;
    t->has_alpha = fmt_of(fmt) ? fmt_of(fmt)->alpha : 0;
    t->lv = calloc(lv, sizeof *t->lv);
    t->obj = fam_com_new(&Tex_iface, t);
    for (uint32_t i = 0; i < lv; i++) {
        t->lv[i] = surface_new(level_dim(w, i), level_dim(h, i), fmt, usage, pool, 1);
        t->lv[i]->parent = t; t->lv[i]->level = (int)i;
    }
    t->dirty_levels = (1u << lv) - 1;
    MEM32(A32(7)) = t->obj;
    RET(D3D_OK, 8);
}
static void t_GetLevelDesc(void) {                   /* (this, level, desc*) */
    fam_texture *t = fam_texture_of(A32(0));
    if (A32(1) >= t->levels) RET(D3DERR_INVALIDCALL, 3);
    fam_surface *s = t->lv[A32(1)];
    fam_D3DSURFACE_DESC *d = APTR(2);
    memset(d, 0, sizeof *d);
    d->Format = s->format; d->Type = 1; d->Usage = s->usage; d->Pool = s->pool;
    d->Size = s->size; d->Width = s->width; d->Height = s->height;
    RET(D3D_OK, 3);
}
static void t_GetLevelCount(void) { RET(fam_texture_of(A32(0))->levels, 1); }
static void t_GetSurfaceLevel(void) {                /* (this, level, surf**) */
    fam_texture *t = fam_texture_of(A32(0));
    if (A32(1) >= t->levels) RET(D3DERR_INVALIDCALL, 3);
    fam_com_addref(t->obj);
    MEM32(A32(2)) = t->lv[A32(1)]->obj;
    RET(D3D_OK, 3);
}
static void t_LockRect(void) {                       /* (this, level, D3DLOCKED_RECT*, RECT*, flags) */
    fam_texture *t = fam_texture_of(A32(0));
    uint32_t l = A32(1);
    if (l >= t->levels) RET(D3DERR_INVALIDCALL, 5);
    fam_surface *s = t->lv[l];
    uint32_t off = 0;
    if (A32(3)) { int32_t *r = APTR(3); off = (uint32_t)r[1] * s->pitch + (uint32_t)r[0] * (uint32_t)(fmt_of(s->format)->bpp / 8); }
    MEM32(A32(2)) = s->pitch;
    MEM32(A32(2) + 4) = s->mem + off;
    RET(D3D_OK, 5);
}
static void t_UnlockRect(void) {
    fam_texture *t = fam_texture_of(A32(0));
    if (A32(1) < t->levels) t->dirty_levels |= 1u << A32(1);
    RET(D3D_OK, 2);
}
static void t_AddRef(void) { RET(fam_com_addref(A32(0)), 1); }
static void t_Release(void) {
    fam_texture *t = fam_texture_of(A32(0));
    uint32_t n = fam_com_release(A32(0));
    if (!n) {
        for (int s = 0; s < FAM_MAX_STAGES; s++) if (g_dev.tex[s] == t->obj) g_dev.tex[s] = 0;
        if (t->gl) { fam_texture_unbind_gl(t->gl); glDeleteTextures(1, &t->gl); }
        for (uint32_t i = 0; i < t->levels; i++) { guest_free(t->lv[i]->mem); hle_com_free(t->lv[i]->obj); free(t->lv[i]); }
        free(t->lv);
        hle_com_free(t->obj);
        free(t);
    }
    RET(n, 1);
}
static void t_GetType(void) { RET(3 /* D3DRTYPE_TEXTURE */, 1); }
static void t_SetPriority(void) { RET(0, 2); }
static void t_GetPriority(void) { RET(0, 1); }
static void t_PreLoad(void) { RET(0, 1); }
static void t_SetLOD(void) { RET(0, 2); }
static void t_GetLOD(void) { RET(0, 1); }
static void t_AddDirtyRect(void) { fam_texture_of(A32(0))->dirty_levels |= 1; RET(D3D_OK, 2); }

/* D3D pixels -> GLES upload (scratch buffer reused) */
static uint8_t *g_conv; static size_t g_conv_cap;
static void *conv_buf(size_t n) { if (n > g_conv_cap) { free(g_conv); g_conv = malloc(n); g_conv_cap = n; } return g_conv; }

static void upload_level(fam_texture *t, uint32_t l) {
    fam_surface *s = t->lv[l];
    const uint8_t *src = hle_ptr(s->mem);
    uint32_t w = s->width, h = s->height, n = w * h;
    GLenum ifmt, fmt, type; const void *px = src;
    switch (t->format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: {
        int x = t->format == D3DFMT_X8R8G8B8;
        if (g_dev.ext_bgra && !x) { ifmt = fmt = GL_BGRA_EXT; type = GL_UNSIGNED_BYTE; break; }
        uint8_t *d = conv_buf((size_t)n * 4);
        for (uint32_t i = 0; i < n; i++) { d[i*4] = src[i*4+2]; d[i*4+1] = src[i*4+1]; d[i*4+2] = src[i*4]; d[i*4+3] = x ? 255 : src[i*4+3]; }
        ifmt = fmt = GL_RGBA; type = GL_UNSIGNED_BYTE; px = d; break;
    }
    case D3DFMT_R5G6B5: ifmt = fmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5; break;
    case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: {
        const uint16_t *p = (const uint16_t *)src; uint16_t *d = conv_buf((size_t)n * 2);
        uint16_t a = t->format == D3DFMT_X1R5G5B5;
        for (uint32_t i = 0; i < n; i++) d[i] = (uint16_t)((p[i] << 1) | (a ? 1 : (p[i] >> 15)));
        ifmt = fmt = GL_RGBA; type = GL_UNSIGNED_SHORT_5_5_5_1; px = d; break;
    }
    case D3DFMT_A4R4G4B4: {
        const uint16_t *p = (const uint16_t *)src; uint16_t *d = conv_buf((size_t)n * 2);
        for (uint32_t i = 0; i < n; i++) d[i] = (uint16_t)((p[i] << 4) | (p[i] >> 12));
        ifmt = fmt = GL_RGBA; type = GL_UNSIGNED_SHORT_4_4_4_4; px = d; break;
    }
    case D3DFMT_L8: ifmt = fmt = GL_LUMINANCE; type = GL_UNSIGNED_BYTE; break;
    case D3DFMT_A8: ifmt = fmt = GL_ALPHA; type = GL_UNSIGNED_BYTE; break;
    case D3DFMT_A8L8: ifmt = fmt = GL_LUMINANCE_ALPHA; type = GL_UNSIGNED_BYTE; break;
    default: return;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (t->gl_alloc_levels & (1u << l))
        glTexSubImage2D(GL_TEXTURE_2D, (GLint)l, 0, 0, (GLsizei)w, (GLsizei)h, fmt, type, px);
    else {
        glTexImage2D(GL_TEXTURE_2D, (GLint)l, (GLint)ifmt, (GLsizei)w, (GLsizei)h, 0, fmt, type, px);
        t->gl_alloc_levels |= 1u << l;
    }
    g_draw_stats.tex_uploads++; g_draw_stats.tex_bytes += (uint64_t)n * (type == GL_UNSIGNED_BYTE && (fmt == GL_RGBA || fmt == GL_BGRA_EXT) ? 4 : 2);
}

/* GL binding cache: which texture each unit holds and the active unit, so a
 * draw that keeps its textures issues no GL calls for them. */
static uint32_t g_bound[8];
static int g_active = -1;
void fam_resource_invalidate_gl(void) { for (int i = 0; i < 8; i++) g_bound[i] = ~0u; g_active = -1; }
static void active_unit(int unit) { if (g_active != unit) { glActiveTexture(GL_TEXTURE0 + unit); g_active = unit; } }
void fam_texture_unbind_gl(uint32_t gl) { for (int i = 0; i < 8; i++) if (g_bound[i] == gl) g_bound[i] = 0; }
void fam_texture_bind(fam_texture *t, int unit) {
    uint32_t want = t ? t->gl : 0;
    if (t && !t->gl) {
        active_unit(unit);
        glGenTextures(1, &t->gl);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        g_bound[unit] = t->gl;
        t->gl_wrap_u = t->gl_wrap_v = t->gl_min = t->gl_mag = 0; t->gl_alloc_levels = 0;
    } else if (g_bound[unit] != want || (t && t->dirty_levels)) {
        active_unit(unit);
        glBindTexture(GL_TEXTURE_2D, want);
        g_bound[unit] = want;
    } else {
        active_unit(unit);              /* sampler parameters that follow apply to this unit */
        return;
    }
    if (!t) return;
    if (t->dirty_levels) {
        for (uint32_t l = 0; l < t->levels; l++) if (t->dirty_levels & (1u << l)) upload_level(t, l);
        t->dirty_levels = 0;
    }
}

/* -------------------------------------------------------------- buffers */
static void b_new(int kind, fam_iface *it) {   /* dev (this, length, usage, fvf/format, pool, out**) */
    fam_buffer *b = calloc(1, sizeof *b);
    b->kind = kind; b->size = A32(1); b->usage = A32(2); b->fvf_or_format = A32(3); b->pool = A32(4);
    b->mem = guest_alloc(b->size);
    b->dirty_lo = 0; b->dirty_hi = b->size;
    b->obj = fam_com_new(it, b);
    MEM32(A32(5)) = b->obj;
    RET(b->mem ? D3D_OK : E_OUTOFMEMORY, 6);
}
static void d_CreateVertexBuffer(void) { b_new(FAM_RES_VB, &VB_iface); }
static void d_CreateIndexBuffer(void)  { b_new(FAM_RES_IB, &IB_iface); }
static void b_Lock(void) {                           /* (this, offset, size, BYTE**, flags) */
    fam_buffer *b = fam_buffer_of(A32(0));
    uint32_t off = A32(1), n = A32(2) ? A32(2) : b->size - A32(1);
    if (off + n > b->size) n = b->size - off;
    b->lock_off = off; b->lock_size = n;
    MEM32(A32(3)) = b->mem + off;
    RET(D3D_OK, 5);
}
static void b_Unlock(void) {
    fam_buffer *b = fam_buffer_of(A32(0));
    uint32_t lo = b->lock_off, hi = b->lock_off + b->lock_size;
    if (b->dirty_hi <= b->dirty_lo) { b->dirty_lo = lo; b->dirty_hi = hi; }
    else { if (lo < b->dirty_lo) b->dirty_lo = lo; if (hi > b->dirty_hi) b->dirty_hi = hi; }
    RET(D3D_OK, 1);
}
static void vb_GetDesc(void) {                       /* (this, D3DVERTEXBUFFER_DESC*) */
    fam_buffer *b = fam_buffer_of(A32(0));
    fam_D3DVERTEXBUFFER_DESC *d = APTR(1);
    d->Format = 100 /* D3DFMT_VERTEXDATA */; d->Type = 4; d->Usage = b->usage; d->Pool = b->pool; d->Size = b->size; d->FVF = b->fvf_or_format;
    RET(D3D_OK, 2);
}
static void ib_GetDesc(void) {
    fam_buffer *b = fam_buffer_of(A32(0));
    fam_D3DINDEXBUFFER_DESC *d = APTR(1);
    d->Format = b->fvf_or_format; d->Type = 5; d->Usage = b->usage; d->Pool = b->pool; d->Size = b->size;
    RET(D3D_OK, 2);
}
static void b_Release(void) {
    fam_buffer *b = fam_buffer_of(A32(0));
    uint32_t n = fam_com_release(A32(0));
    if (!n) {
        if (g_dev.stream_vb == b->obj) g_dev.stream_vb = 0;
        if (g_dev.ib == b->obj) g_dev.ib = 0;
        if (b->gl) { glDeleteBuffers(1, &b->gl); fam_bind_forget(b->gl); }
        guest_free(b->mem); hle_com_free(b->obj); free(b);
    }
    RET(n, 1);
}
static void b_GetType(void) { RET(fam_buffer_of(A32(0))->kind == FAM_RES_VB ? 4 : 5, 1); }

/* Static buffers live in GL buffers; dynamic ones are drawn from client
 * memory by the caller (returns 0) so a frequently refilled buffer never
 * makes the driver wait on a draw still reading it. */
static void bind_target(uint32_t target, GLuint b) { if (target == GL_ARRAY_BUFFER) fam_bind_array(b); else fam_bind_element(b); }
/* A "static" buffer the game keeps rewriting (software skinning, vertex
 * lighting into shared buffers) is drawn through the streaming ring instead:
 * glBufferSubData on a buffer the GPU may still read makes the driver copy it. */
int fam_stream_on(void);
static int stream_rewritten(fam_buffer *b) {
    if (b->rewrites < 4 || !fam_stream_on()) return 0;
    if (b->rewrites == 4) {
        fprintf(stderr, "[d3d8] %s %u KB rewritten every frame: streamed from now on\n", b->kind == FAM_RES_VB ? "VB" : "IB", b->size >> 10);
        b->rewrites++;
    }
    b->dirty_hi = b->dirty_lo = 0;
    return 1;
}
uint32_t fam_buffer_gl(fam_buffer *b, uint32_t target) {
    if (b->usage & D3DUSAGE_DYNAMIC) return 0;
    if (stream_rewritten(b)) return 0;
    if (!b->gl) { glGenBuffers(1, &b->gl); bind_target(target, b->gl); glBufferData(target, b->size, hle_ptr(b->mem), GL_STATIC_DRAW); b->dirty_hi = b->dirty_lo = 0; return b->gl; }
    bind_target(target, b->gl);
    if (b->dirty_hi > b->dirty_lo) {
        glBufferSubData(target, b->dirty_lo, b->dirty_hi - b->dirty_lo, (uint8_t *)hle_ptr(b->mem) + b->dirty_lo);
        g_draw_stats.buf_bytes += b->dirty_hi - b->dirty_lo;
        b->dirty_hi = b->dirty_lo = 0;
        if (fam_config_int("stream_hot", 1)) b->rewrites++;
    }
    return b->gl;
}

/* ----------------------------------------------- device-level methods */
static void d_SetTexture(void) {                     /* (this, stage, IDirect3DBaseTexture8*) */
    uint32_t s = A32(1);
    if (fam_sb_rec_tex(s, A32(2))) RET(D3D_OK, 3);
    if (s < FAM_MAX_STAGES) g_dev.tex[s] = A32(2);
    RET(D3D_OK, 3);
}
static void d_GetTexture(void) {
    uint32_t s = A32(1), t = s < FAM_MAX_STAGES ? g_dev.tex[s] : 0;
    if (t) fam_com_addref(t);
    MEM32(A32(2)) = t;
    RET(D3D_OK, 3);
}
static void d_SetStreamSource(void) {                /* (this, stream, vb, stride) */
    if (A32(1) == 0 && fam_sb_rec_stream(A32(2), A32(3))) RET(D3D_OK, 4);
    if (A32(1) == 0) { g_dev.stream_vb = A32(2); g_dev.stream_stride = A32(3); }
    RET(D3D_OK, 4);
}
static void d_SetIndices(void) { if (fam_sb_rec_ib(A32(1), A32(2))) RET(D3D_OK, 3); g_dev.ib = A32(1); g_dev.base_vertex = A32(2); RET(D3D_OK, 3); }
static void d_GetBackBuffer(void) {                  /* (this, index, type, surf**) */
    fam_surface *s = fam_surface_of(g_dev.backbuffer_obj);
    fam_com_addref(s->obj);
    MEM32(A32(3)) = s->obj;
    RET(D3D_OK, 4);
}
static void d_GetDepthStencilSurface(void) {
    if (!g_dev.depth_obj) RET(D3DERR_NOTFOUND, 2);
    fam_com_addref(g_dev.depth_obj); MEM32(A32(1)) = g_dev.depth_obj; RET(D3D_OK, 2);
}
static void d_GetRenderTarget(void) { fam_com_addref(g_dev.rt_obj); MEM32(A32(1)) = g_dev.rt_obj; RET(D3D_OK, 2); }
static void d_CreateImageSurface(void) {             /* (this, w, h, fmt, surf**) */
    fam_surface *s = surface_new(A32(1), A32(2), A32(3), 0, 2 /* SYSTEMMEM */, 1);
    MEM32(A32(4)) = s->obj;
    RET(D3D_OK, 5);
}
static void d_CopyRects(void) {                      /* (this, src, srcrects, n, dst, dstpoints) */
    fam_surface *s = fam_surface_of(A32(1)), *d = fam_surface_of(A32(4));
    if (!s || !d || !s->mem || !d->mem || s->format != d->format) {
        static int once; if (!once++) fprintf(stderr, "[d3d8] CopyRects between unsupported surfaces\n");
        RET(D3DERR_INVALIDCALL, 6);
    }
    uint32_t bpp = (uint32_t)fmt_of(s->format)->bpp / 8, n = A32(3) ? A32(3) : 1;
    for (uint32_t i = 0; i < n; i++) {
        int32_t r[4] = { 0, 0, (int32_t)s->width, (int32_t)s->height }, p[2] = { 0, 0 };
        if (A32(2)) memcpy(r, hle_ptr(A32(2) + i * 16), 16);
        if (A32(5)) memcpy(p, hle_ptr(A32(5) + i * 8), 8);
        for (int32_t y = r[1]; y < r[3]; y++)
            memcpy((uint8_t *)hle_ptr(d->mem) + (uint32_t)(p[1] + y - r[1]) * d->pitch + (uint32_t)p[0] * bpp,
                   (uint8_t *)hle_ptr(s->mem) + (uint32_t)y * s->pitch + (uint32_t)r[0] * bpp, (size_t)(r[2] - r[0]) * bpp);
    }
    if (d->parent) d->parent->dirty_levels |= 1u << d->level;
    RET(D3D_OK, 6);
}
static void d_UpdateTexture(void) {                  /* (this, srcTex, dstTex) */
    fam_texture *s = fam_texture_of(A32(1)), *d = fam_texture_of(A32(2));
    if (!s || !d) RET(D3DERR_INVALIDCALL, 3);
    for (uint32_t l = 0; l < s->levels && l < d->levels; l++)
        if (s->lv[l]->size == d->lv[l]->size) memcpy(hle_ptr(d->lv[l]->mem), hle_ptr(s->lv[l]->mem), s->lv[l]->size);
    d->dirty_levels = (1u << d->levels) - 1;
    RET(D3D_OK, 3);
}

void fam_resources_init(void) {
    static const fam_method tex[] = {
        { IDIRECT3DTEXTURE8_AddRef, t_AddRef }, { IDIRECT3DTEXTURE8_Release, t_Release },
        { IDIRECT3DTEXTURE8_GetDevice, s_GetDevice }, { IDIRECT3DTEXTURE8_SetPriority, t_SetPriority },
        { IDIRECT3DTEXTURE8_GetPriority, t_GetPriority }, { IDIRECT3DTEXTURE8_PreLoad, t_PreLoad },
        { IDIRECT3DTEXTURE8_GetType, t_GetType }, { IDIRECT3DTEXTURE8_SetLOD, t_SetLOD },
        { IDIRECT3DTEXTURE8_GetLOD, t_GetLOD }, { IDIRECT3DTEXTURE8_GetLevelCount, t_GetLevelCount },
        { IDIRECT3DTEXTURE8_GetLevelDesc, t_GetLevelDesc }, { IDIRECT3DTEXTURE8_GetSurfaceLevel, t_GetSurfaceLevel },
        { IDIRECT3DTEXTURE8_LockRect, t_LockRect }, { IDIRECT3DTEXTURE8_UnlockRect, t_UnlockRect },
        { IDIRECT3DTEXTURE8_AddDirtyRect, t_AddDirtyRect }, { 0, 0 } };
    static const fam_method surf[] = {
        { IDIRECT3DSURFACE8_AddRef, s_AddRef }, { IDIRECT3DSURFACE8_Release, s_Release },
        { IDIRECT3DSURFACE8_GetDevice, s_GetDevice }, { IDIRECT3DSURFACE8_GetContainer, s_GetContainer },
        { IDIRECT3DSURFACE8_GetDesc, s_GetDesc }, { IDIRECT3DSURFACE8_LockRect, s_LockRect },
        { IDIRECT3DSURFACE8_UnlockRect, s_UnlockRect }, { 0, 0 } };
    static const fam_method vb[] = {
        { IDIRECT3DVERTEXBUFFER8_AddRef, t_AddRef }, { IDIRECT3DVERTEXBUFFER8_Release, b_Release },
        { IDIRECT3DVERTEXBUFFER8_GetDevice, s_GetDevice }, { IDIRECT3DVERTEXBUFFER8_GetType, b_GetType },
        { IDIRECT3DVERTEXBUFFER8_Lock, b_Lock }, { IDIRECT3DVERTEXBUFFER8_Unlock, b_Unlock },
        { IDIRECT3DVERTEXBUFFER8_GetDesc, vb_GetDesc }, { 0, 0 } };
    static const fam_method ib[] = {
        { IDIRECT3DINDEXBUFFER8_AddRef, t_AddRef }, { IDIRECT3DINDEXBUFFER8_Release, b_Release },
        { IDIRECT3DINDEXBUFFER8_GetDevice, s_GetDevice }, { IDIRECT3DINDEXBUFFER8_GetType, b_GetType },
        { IDIRECT3DINDEXBUFFER8_Lock, b_Lock }, { IDIRECT3DINDEXBUFFER8_Unlock, b_Unlock },
        { IDIRECT3DINDEXBUFFER8_GetDesc, ib_GetDesc }, { 0, 0 } };
    fam_iface_vtable(&Tex_iface, tex);
    fam_iface_vtable(&Surf_iface, surf);
    fam_iface_vtable(&VB_iface, vb);
    fam_iface_vtable(&IB_iface, ib);
    if (!g_dev.backbuffer_obj) {
        fam_surface *bb = surface_new(g_dev.pp.BackBufferWidth, g_dev.pp.BackBufferHeight, g_dev.pp.BackBufferFormat, D3DUSAGE_RENDERTARGET, 0, 0);
        bb->is_backbuffer = 1;
        g_dev.backbuffer_obj = g_dev.rt_obj = bb->obj;
        if (g_dev.pp.EnableAutoDepthStencil) {
            fam_surface *z = surface_new(g_dev.pp.BackBufferWidth, g_dev.pp.BackBufferHeight, g_dev.pp.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, 0, 0);
            z->is_depth = 1;
            g_dev.depth_obj = g_dev.zs_obj = z->obj;
        }
    }
}

const fam_method fam_dev_resource_methods[] = {
    FAM_DEV(CreateTexture, t_CreateTexture) FAM_DEV(CreateVertexBuffer, d_CreateVertexBuffer)
    FAM_DEV(CreateIndexBuffer, d_CreateIndexBuffer) FAM_DEV(SetTexture, d_SetTexture) FAM_DEV(GetTexture, d_GetTexture)
    FAM_DEV(SetStreamSource, d_SetStreamSource) FAM_DEV(SetIndices, d_SetIndices) FAM_DEV(GetBackBuffer, d_GetBackBuffer)
    FAM_DEV(GetDepthStencilSurface, d_GetDepthStencilSurface) FAM_DEV(GetRenderTarget, d_GetRenderTarget)
    FAM_DEV(CreateImageSurface, d_CreateImageSurface) FAM_DEV(CopyRects, d_CopyRects) FAM_DEV(UpdateTexture, d_UpdateTexture)
    { 0, 0 }
};
