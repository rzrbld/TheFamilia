/* d3d8_internal.h - state shared by d3d8.c, d3d8_resource.c and d3d8_draw.c. */
#pragma once
#include <stdint.h>
#include <SDL2/SDL.h>
#include "d3d8_enums.h"
#include "d3d8_types.h"
#include "com.h"
#include <math.h>

#define FAM_MAX_STAGES   4          /* LS3D blends at most a few stages; GLES2 has >= 8 units */
#define FAM_MAX_STREAMS  1
#define FAM_MAX_LIGHTS   8
#define FAM_NUM_TRANSFORMS 512      /* D3DTRANSFORMSTATETYPE indices; WORLD is 256 */
#define FAM_TSS_COUNT    32
#define FAM_RS_COUNT     256

/* resource kinds */
enum { FAM_RES_TEXTURE = 1, FAM_RES_SURFACE, FAM_RES_VB, FAM_RES_IB, FAM_RES_CUBE };

typedef struct fam_texture fam_texture;
typedef struct fam_surface {
    int kind;                    /* FAM_RES_SURFACE */
    uint32_t obj;                /* guest COM object */
    uint32_t width, height, format, usage, pool;
    uint32_t pitch, size;
    uint32_t mem;                /* guest VA of the lockable copy (0 for depth/back buffers) */
    fam_texture *parent;         /* level of this texture, or NULL */
    int level;
    int is_backbuffer, is_depth;
    int dirty;
} fam_surface;

struct fam_texture {
    int kind;                    /* FAM_RES_TEXTURE */
    uint32_t obj;
    uint32_t width, height, levels, format, usage, pool;
    fam_surface **lv;            /* one surface per level */
    uint32_t gl;                 /* GL texture name */
    uint32_t dirty_levels;       /* bit per level needing upload */
    uint32_t gl_wrap_u, gl_wrap_v, gl_min, gl_mag;   /* cached sampler state */
    uint32_t gl_alloc_levels;           /* levels with GL storage (re-uploads use TexSubImage) */
    int has_alpha;
    /* CPU copies dropped once settled (d3d8_resource.c, tex_evict) */
    uint64_t touched;                   /* device frame of the last lock, copy or upload */
    int evicted;                        /* the levels' copies are dropped; read back before use */
    fam_texture *prev, *next;           /* all textures, for the eviction scan */
};

typedef struct {
    int kind;                    /* FAM_RES_VB / FAM_RES_IB */
    uint32_t obj;
    uint32_t size, usage, fvf_or_format, pool;
    uint32_t mem;                /* guest VA of the data */
    uint32_t gl;                 /* GL buffer name, 0 when drawn from client memory */
    uint32_t dirty_lo, dirty_hi; /* byte range written since the last upload */
    uint32_t lock_off, lock_size;
    uint32_t rewrites;           /* uploads after the first; past a few the buffer is streamed instead */
    uint32_t version;            /* bumped at each Unlock: streamed copies of older data are stale */
} fam_buffer;

typedef struct {
    uint32_t obj;                    /* the guest IDirect3DDevice8 */
    SDL_Window *window;
    SDL_GLContext gl;
    int gles_major;
    char gl_version[128], gl_renderer[128];
    int ext_bgra, ext_depth24, ext_packed_ds, ext_uint_index, ext_npot, ext_s3tc;
    int drawable_w, drawable_h, bb_w, bb_h;
    int out_x, out_y, out_w, out_h;  /* where the back buffer lands in the drawable (4:3, centred) */
    fam_D3DPRESENT_PARAMETERS pp;
    uint32_t hwnd, behavior;

    uint32_t rs[FAM_RS_COUNT];
    uint32_t tss[FAM_MAX_STAGES][FAM_TSS_COUNT];
    float    xf[FAM_NUM_TRANSFORMS][16];
    fam_D3DVIEWPORT8 vp;
    fam_D3DMATERIAL8 material;
    fam_D3DLIGHT8 light[FAM_MAX_LIGHTS];
    int light_enabled[FAM_MAX_LIGHTS];
    uint32_t state_dirty;            /* FAM_DIRTY_* */

    uint32_t tex[FAM_MAX_STAGES];    /* bound guest texture objects */
    uint32_t stream_vb, stream_stride;
    uint32_t ib, base_vertex;
    uint32_t fvf;                    /* current vertex shader handle (FVF code) */
    uint32_t backbuffer_obj, depth_obj;
    uint32_t rt_obj, zs_obj;         /* current render target / depth surface */
    int in_scene;
    uint64_t frame;
} fam_device;

enum { FAM_DIRTY_XF = 1, FAM_DIRTY_RS = 2, FAM_DIRTY_TSS = 4, FAM_DIRTY_VP = 8, FAM_DIRTY_LIGHT = 16, FAM_DIRTY_MAT = 32 };

extern fam_device g_dev;
typedef struct { uint64_t draws, prims, uploads, tex_uploads, tex_bytes, progs, buf_bytes, vreuse; } fam_draw_stats;
/* d3d8_stream.c: buffer-binding cache, streaming ring */
void fam_bind_array(unsigned b); void fam_bind_element(unsigned b); void fam_bind_forget(unsigned b);
extern fam_draw_stats g_draw_stats;

/* d3d8.c */
void fam_update_output_rect(void);   /* after a size change */
/* back-buffer pixel rect (top-left origin) -> GL window rect (bottom-left origin) */
static inline void fam_bb_to_gl(int x, int y, int w, int h, int *gx, int *gy, int *gw, int *gh) {
    float sx = (float)g_dev.out_w / (float)g_dev.bb_w, sy = (float)g_dev.out_h / (float)g_dev.bb_h;
    *gx = g_dev.out_x + (int)(x * sx);
    *gy = g_dev.out_y + (int)((g_dev.bb_h - y - h) * sy);
    *gw = (int)(w * sx + 0.5f); *gh = (int)(h * sy + 0.5f);
}
void fam_fill_caps(fam_D3DCAPS8 *c);
uint32_t fam_d3d_object(void);
/* gl_load.c */
int fam_gl_load(void);
/* config.c */
int fam_config_int(const char *key, int def);
/* d3d8_resource.c */
void fam_resources_init(void);
int  fam_texture_format_supported(uint32_t usage, uint32_t rtype, uint32_t fmt);
fam_texture *fam_texture_of(uint32_t obj);
fam_buffer  *fam_buffer_of(uint32_t obj);
fam_surface *fam_surface_of(uint32_t obj);
void fam_texture_bind(fam_texture *t, int unit);
uint32_t fam_buffer_gl(fam_buffer *b, uint32_t target);  /* upload pending bytes, return GL name */
/* d3d8_draw.c */
void fam_draw_init(void);
/* math */
static inline void fam_mat_identity(float *m) {
    for (int i = 0; i < 16; i++) m[i] = (i % 5) == 0 ? 1.f : 0.f;
}

/* Device methods are implemented across the files; each exports a table
 * ({slot, fn} ... {0,0}) and d3d8_device.c concatenates them for CreateDevice. */
#define FAM_DEV(slot, fn) { IDIRECT3DDEVICE8_##slot, fn },
const fam_method *fam_device_method_table(void);
extern const fam_method fam_dev_state_methods[], fam_dev_resource_methods[], fam_dev_draw_methods[], fam_dev_sb_methods[];
/* d3d8_stateblock.c: return 1 when a state block is recording and took the call */
int fam_sb_rec_rs(uint32_t s, uint32_t v);
int fam_sb_rec_tss(uint32_t st, uint32_t t, uint32_t v);
int fam_sb_rec_xf(uint32_t s, const float *m);
int fam_sb_rec_tex(uint32_t st, uint32_t t);
int fam_sb_rec_light(uint32_t i, const fam_D3DLIGHT8 *l);
int fam_sb_rec_light_en(uint32_t i, int en);
int fam_sb_rec_vp(const fam_D3DVIEWPORT8 *v);
int fam_sb_rec_mat(const fam_D3DMATERIAL8 *m);
int fam_sb_rec_fvf(uint32_t f);
int fam_sb_rec_stream(uint32_t vb, uint32_t stride);
int fam_sb_rec_ib(uint32_t ib, uint32_t base);
uint32_t fam_vs_fvf(uint32_t handle);     /* FVF code for a SetVertexShader handle */
