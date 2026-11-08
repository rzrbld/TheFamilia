/*
 * movie.c - the game's videos (logo1/logo2/intro .avi: Indeo 5 + MS ADPCM;
 * logo3 .bik: Bink) played with a minimal FFmpeg build shipped in
 * libs.aarch64.
 *
 * Game.exe plays every video through PlayLogo (0x55D460): name.avi, else
 * name.bik, letterboxed to the video's aspect, until a key or the end. That
 * function is replaced here (config/native.json): LS3D's DirectShow/Bink
 * path (IShow) is never reached. Frames are converted to RGBA, drawn as one
 * textured quad and presented; the sound goes into the DirectSound mixer as
 * a stream. FAMILIA_MOVIES=0 skips them (PlayLogo returns false, as for a
 * missing file).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include "gl_funcs.h"
#include "d3d8_internal.h"
#include "win32hle.h"
#include "input.h"

const char *fam_game_dir(void);
void fam_gl_invalidate_caches(void);
extern int fam_glt_on;
void fam_glt_swap(void);

/* the file for name + ext in the game directory, any letter case */
static int find_file(const char *name, const char *ext, char *out, size_t n) {
    char want[256];
    snprintf(want, sizeof want, "%s.%s", name, ext);
    DIR *d = opendir(fam_game_dir());
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, want)) { snprintf(out, n, "%s/%s", fam_game_dir(), e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}

/* ---- drawing: one textured quad ---- */
static GLuint g_prog, g_tex, g_vbo;
static int g_tw, g_th;
static GLuint compile(GLenum t, const char *src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}
static void gl_setup(int w, int h) {
    if (!g_prog) {
        const char *vs = "attribute vec2 a_pos; attribute vec2 a_uv; varying vec2 v_uv;"
                         "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }";
        const char *fs = "precision mediump float; varying vec2 v_uv; uniform sampler2D s_tex;"
                         "void main() { gl_FragColor = vec4(texture2D(s_tex, v_uv).rgb, 1.0); }";
        g_prog = glCreateProgram();
        glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
        glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
        glBindAttribLocation(g_prog, 0, "a_pos");
        glBindAttribLocation(g_prog, 1, "a_uv");
        glLinkProgram(g_prog);
        static const float quad[] = { -1, -1, 0, 1,   1, -1, 1, 1,   -1, 1, 0, 0,   1, 1, 1, 0 };
        glGenBuffers(1, &g_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    }
    if (!g_tex) glGenTextures(1, &g_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    if (g_tw != w || g_th != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_tw = w; g_th = h;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
void fam_present_hook(void);
static void present(void) {
    fam_present_hook();                       /* FPS counter, screenshots, stats, as for the game's frames */
    if (fam_glt_on) fam_glt_swap(); else SDL_GL_SwapWindow(g_dev.window);
}
/* the frame into the letterbox band PlayLogo computes for a 640x480 screen */
static void draw_frame(const uint8_t *rgba, int w, int h, int band) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, g_dev.drawable_w, g_dev.drawable_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int x, y, vw, vh;
    fam_bb_to_gl(0, band / 2, g_dev.bb_w ? g_dev.bb_w : 640, (g_dev.bb_h ? g_dev.bb_h : 480) - band, &x, &y, &vw, &vh);
    glViewport(x, y, vw, vh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "s_tex"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    for (int i = 0; i < 9; i++) glDisableVertexAttribArray((GLuint)i);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (const void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    present();
}

static int skip_pressed(void) {
    fam_input_pump();
    uint8_t keys[256];
    fam_input_keys(keys);
    return (keys[0x01] | keys[0x1C] | keys[0x39]) & 0x80;    /* Esc, Enter, Space */
}

/* plays one file; returns 0 if it could not be opened */
static int play_file(const char *path, float sx, float sy) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return 0;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return 0; }
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vi < 0) { avformat_close_input(&fmt); return 0; }
    AVCodecContext *vc = NULL, *ac = NULL;
    const AVCodec *c = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
    if (!c) { fprintf(stderr, "[movie] %s: no decoder for the video\n", path); avformat_close_input(&fmt); return 0; }
    vc = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(vc, fmt->streams[vi]->codecpar);
    if (avcodec_open2(vc, c, NULL) < 0) { avcodec_free_context(&vc); avformat_close_input(&fmt); return 0; }
    SwrContext *swr = NULL;
    int sid = -1;
    if (ai >= 0 && (c = avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id))) {
        ac = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(ac, fmt->streams[ai]->codecpar);
        if (avcodec_open2(ac, c, NULL) < 0) avcodec_free_context(&ac);
        else {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, ac->sample_rate,
                                    &ac->ch_layout, ac->sample_fmt, ac->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) swr_free(&swr);
            else sid = hle_audio_stream_open(ac->sample_rate, 2);
        }
    }
    int w = vc->width, h = vc->height;
    struct SwsContext *sws = NULL;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    int band = (int)(480 * (1.0f - sy / sx * (4.0f / 3.0f)));
    if (band < 0) band = 0;
    gl_setup(w, h);
    fprintf(stderr, "[movie] %s: %dx%d %s%s\n", path, w, h, avcodec_get_name(vc->codec_id), ac ? ", sound" : "");
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    uint64_t t0 = SDL_GetTicks64();
    double tb = av_q2d(fmt->streams[vi]->time_base);
    int skipped = 0, held = 0;
    int16_t *pcm = NULL; int pcm_cap = 0;
    while (!skipped && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == ai && ac && sid >= 0) {
            if (avcodec_send_packet(ac, pkt) >= 0)
                while (avcodec_receive_frame(ac, fr) >= 0) {
                    int n = swr_get_out_samples(swr, fr->nb_samples);
                    if (n > pcm_cap) { pcm_cap = n; pcm = realloc(pcm, (size_t)n * 4); }
                    int got = swr_convert(swr, (uint8_t **)&pcm, n, (const uint8_t **)fr->extended_data, fr->nb_samples);
                    if (got > 0) hle_audio_stream_push(sid, pcm, got);
                }
        } else if (pkt->stream_index == vi) {
            if (avcodec_send_packet(vc, pkt) >= 0)
                while (!skipped && avcodec_receive_frame(vc, fr) >= 0) {
                    sws = sws_getCachedContext(sws, w, h, (enum AVPixelFormat)fr->format, w, h, AV_PIX_FMT_RGBA,
                                               SWS_BILINEAR, NULL, NULL, NULL);
                    uint8_t *dst[1] = { rgba }; int ls[1] = { w * 4 };
                    sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, h, dst, ls);
                    double pts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp * tb : 0.0;
                    /* wait for the frame's time; keep the sound no more than ~1.5 s ahead */
                    for (;;) {
                        if (skip_pressed()) { if (held) { skipped = 1; break; } } else held = 1;
                        double now = (double)(SDL_GetTicks64() - t0) / 1000.0;
                        if (now >= pts) break;
                        SDL_Delay(2);
                    }
                    if (!skipped) draw_frame(rgba, w, h, band);
                }
        }
        av_packet_unref(pkt);
        while (sid >= 0 && ac && hle_audio_stream_queued(sid) > ac->sample_rate * 3 / 2 && !skipped) {
            if (skip_pressed() && held) skipped = 1;
            SDL_Delay(5);
        }
    }
    /* let the last of the sound play unless skipped */
    while (!skipped && sid >= 0 && hle_audio_stream_queued(sid) > 0) { if (skip_pressed() && held) break; SDL_Delay(10); }
    if (sid >= 0) hle_audio_stream_close(sid);
    av_frame_free(&fr); av_packet_free(&pkt);
    sws_freeContext(sws); swr_free(&swr); free(rgba); free(pcm);
    avcodec_free_context(&vc); avcodec_free_context(&ac);
    avformat_close_input(&fmt);
    fam_gl_invalidate_caches();
    return 1;
}

/* bool __fastcall PlayLogo(const char *name, float sx, float sy) -- Game.exe 0x55D460: ecx = name, ret 8 */
void fam_native_play_logo(void) {
    const char *name = (const char *)ADDR(g_ecx);
    float sx, sy; uint32_t a = MEM32(g_esp + 4), b = MEM32(g_esp + 8);
    memcpy(&sx, &a, 4); memcpy(&sy, &b, 4);
    int ok = 0;
    const char *e = getenv("FAMILIA_MOVIES");
    if (!e || atoi(e)) {
        char path[1024];
        if (find_file(name, "avi", path, sizeof path)) ok = play_file(path, sx, sy);
        if (!ok && find_file(name, "bik", path, sizeof path)) ok = play_file(path, sx, sy);
        if (!ok) fprintf(stderr, "[movie] %s: not played\n", name);
    }
    g_eax = (g_eax & 0xFFFFFF00u) | (ok ? 1u : 0u);
    g_esp += 4 + 8;
}
