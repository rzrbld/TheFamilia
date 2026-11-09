/*
 * d3d8_stream.c - GPU streaming ring for vertex/index data that lives in
 * guest memory (dynamic buffers, DrawPrimitiveUP).
 *
 * Drawing such data from client pointers makes the GLES driver copy it on
 * every draw, and for indexed draws first scan the indices for the vertex
 * range. Here the exact range the D3D call names (minIndex, numVertices) is
 * copied into a persistently mapped buffer (GL_EXT_buffer_storage) and drawn
 * with glDrawRangeElementsBaseVertex, so the driver neither scans nor copies.
 * Three segments rotate per presented frame; a segment is reused only after
 * the fence of the frame that last used it has signalled. FAMILIA_STREAM=0
 * turns it off (client pointers, as before).
 *
 * Also: a cache of buffer bindings (glBindBuffer is called per draw).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "gl_funcs.h"
#include "d3d8_internal.h"

typedef void *(GL_APIENTRY *PFN_map)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef void (GL_APIENTRY *PFN_storage)(GLenum, GLsizeiptr, const void *, GLbitfield);
typedef void *(GL_APIENTRY *PFN_fence)(GLenum, GLbitfield);
typedef GLenum (GL_APIENTRY *PFN_wait)(void *, GLbitfield, uint64_t);
typedef void (GL_APIENTRY *PFN_delsync)(void *);
typedef void (GL_APIENTRY *PFN_rangebv)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint);

static PFN_fence p_fence; static PFN_wait p_wait; static PFN_delsync p_delsync;
PFN_rangebv fam_glDrawRangeElementsBaseVertex;

#define SEG (4u << 20)
static struct {
    int on;
    GLuint buf;
    uint8_t *map;
    uint32_t seg, pos, end;
    void *fence[3];
    uint64_t bytes, fallbacks, waits;
} R;

static GLuint g_bound_array = ~0u, g_bound_element = ~0u;

void fam_bind_array(GLuint b) { if (g_bound_array != b) { glBindBuffer(GL_ARRAY_BUFFER, b); g_bound_array = b; } }
void fam_bind_element(GLuint b) { if (g_bound_element != b) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b); g_bound_element = b; } }
unsigned fam_bound_array_buffer(void) { return g_bound_array; }
void fam_bind_invalidate(void) { g_bound_array = g_bound_element = ~0u; }
unsigned fam_bound_element_buffer(void) { return g_bound_element; }
void fam_attr_forget(unsigned b);
void fam_bind_forget(GLuint b) {          /* a deleted buffer: GL unbinds it */
    fam_attr_forget(b);
    if (g_bound_array == b) g_bound_array = 0;
    if (g_bound_element == b) g_bound_element = 0;
}

void fam_stream_init(void) {
    fam_glDrawRangeElementsBaseVertex = (PFN_rangebv)SDL_GL_GetProcAddress("glDrawRangeElementsBaseVertex");
    if (!fam_glDrawRangeElementsBaseVertex) fam_glDrawRangeElementsBaseVertex = (PFN_rangebv)SDL_GL_GetProcAddress("glDrawRangeElementsBaseVertexEXT");
    /* Off by default: with the persistently mapped ring the Mali driver
     * (H700, libmali r20) allocates memory with the draws that read it and
     * never frees it (about 0.3 MB/s standing, far more driving; the 1 GB
     * H700 ran out of memory after half an hour of play). Renewing the buffer
     * or modifying it through GL did not stop it. FAMILIA_STREAM=1 (the
     * launcher's "GPU streaming") turns it on: about 2 FPS faster. */
    if (!fam_config_int("stream", 0)) { fprintf(stderr, "[stream] off (FAMILIA_STREAM=1 turns it on)\n"); return; }
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    PFN_storage storage = (PFN_storage)SDL_GL_GetProcAddress("glBufferStorageEXT");
    PFN_map map = (PFN_map)SDL_GL_GetProcAddress("glMapBufferRange");
    p_fence = (PFN_fence)SDL_GL_GetProcAddress("glFenceSync");
    p_wait = (PFN_wait)SDL_GL_GetProcAddress("glClientWaitSync");
    p_delsync = (PFN_delsync)SDL_GL_GetProcAddress("glDeleteSync");
    if (!ext || !strstr(ext, "GL_EXT_buffer_storage") || !storage || !map || !p_fence || !p_wait || !p_delsync
        || !fam_glDrawRangeElementsBaseVertex) {
        fprintf(stderr, "[stream] unavailable (needs GL_EXT_buffer_storage, sync objects, DrawRangeElementsBaseVertex)\n");
        return;
    }
    while (glGetError() != GL_NO_ERROR) {}
    glGenBuffers(1, &R.buf);
    fam_bind_array(R.buf);
    const GLbitfield fl = 0x0002 | 0x0040 | 0x0080;     /* MAP_WRITE | PERSISTENT | COHERENT */
    storage(GL_ARRAY_BUFFER, (GLsizeiptr)(3u * SEG), NULL, fl);
    void *m = glGetError() == GL_NO_ERROR ? map(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(3u * SEG), fl) : NULL;
    if (!m || glGetError() != GL_NO_ERROR) {
        glDeleteBuffers(1, &R.buf); fam_bind_forget(R.buf); R.buf = 0;
        while (glGetError() != GL_NO_ERROR) {}
        fprintf(stderr, "[stream] unavailable: persistent map failed\n");
        return;
    }
    R.map = m; R.seg = 0; R.pos = 0; R.end = SEG; R.on = 1;
    fprintf(stderr, "[stream] GPU streaming ring: 3 x %u MiB, persistent\n", SEG >> 20);
}

int fam_stream_on(void) { return R.on; }

/* at Present: fence this frame's segment, move to the next one once the GPU is done with it */
extern int fam_glt_on;
void fam_glt_frame_end(uint64_t f); void fam_glt_wait_segment(int s, uint64_t f);
uint32_t fam_stream_epoch;            /* bumped whenever the ring moves to another segment */
void fam_stream_frame(void) {
    if (!R.on) return;
    fam_stream_epoch++;
    if (fam_glt_on) {                 /* fences live on the GL thread (gl_thread.c) */
        static uint64_t frame;
        fam_glt_frame_end(frame);
        frame++;
        R.seg = (uint32_t)(frame % 3);
        fam_glt_wait_segment((int)R.seg, frame);
        R.pos = R.seg * SEG; R.end = R.pos + SEG;
        return;
    }
    if (R.fence[R.seg]) p_delsync(R.fence[R.seg]);
    R.fence[R.seg] = p_fence(0x9117 /* GL_SYNC_GPU_COMMANDS_COMPLETE */, 0);
    R.seg = (R.seg + 1) % 3;
    if (R.fence[R.seg]) {
        GLenum st = p_wait(R.fence[R.seg], 0, 0);
        if (st != 0x911A && st != 0x911C) { R.waits++; p_wait(R.fence[R.seg], 0x1 /* FLUSH */, 1000000000ull); }
        p_delsync(R.fence[R.seg]); R.fence[R.seg] = NULL;
    }
    R.pos = R.seg * SEG; R.end = R.pos + SEG;
}

/* Copies n bytes into the ring at an offset aligned to `align`; returns the
 * offset, or -1 when the segment is full (the caller falls back). */
static uint64_t g_mid_rotations;
/* room for one draw's vertices and indices together: rotating between the two
 * would leave the vertices in a segment whose fence was set before the draw */
void fam_stream_need(uint32_t n) {
    if (!R.on || n > SEG / 2) return;
    if (R.pos + n + 64 > R.end) { fam_stream_frame(); g_mid_rotations++; }
}
long fam_stream_put(const void *src, uint32_t n, uint32_t align) {
    uint32_t off = (R.pos + align - 1) / align * align;
    if (off + n > R.end && n <= SEG / 2) {          /* segment full before Present (loading, long frames): */
        fam_stream_frame();                          /* fence it and continue in the next one */
        g_mid_rotations++;
        off = (R.pos + align - 1) / align * align;
    }
    if (off + n > R.end) { R.fallbacks++; return -1; }
    memcpy(R.map + off, src, n);
    R.pos = off + n;
    R.bytes += n;
    return (long)off;
}
GLuint fam_stream_buffer(void) { return R.buf; }

void fam_stream_report(double frames) {
    static uint64_t lb, lf, lw;
    if (!R.on || frames <= 0) return;
    fprintf(stderr, "[stream] per frame: %.0f KB streamed | fallbacks %llu waits %llu mid-frame rotations %llu (total)\n",
            (double)(R.bytes - lb) / 1024.0 / frames, (unsigned long long)R.fallbacks, (unsigned long long)R.waits,
            (unsigned long long)g_mid_rotations);
    lb = R.bytes; lf = R.fallbacks; lw = R.waits; (void)lf; (void)lw;
}
