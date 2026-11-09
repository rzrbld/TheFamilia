/*
 * frame.c - per-Present work: frame timing statistics (FAMILIA_STATS=1
 * prints a line every 5 s with fps, frame-time percentiles and draw counts).
 */
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "d3d8_internal.h"
#include "input.h"

#define WIN 512
uint64_t fam_swap_ticks;
#include <sys/syscall.h>
#include <unistd.h>
static double g_ft[WIN];
static int g_nft;
static uint64_t g_last, g_window_start, g_window_frames, g_window_draws;

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

#include "gl_funcs.h"
void fam_apply_write_masks(void);
/* FAMILIA_SHOT=N: every N seconds write the back buffer to userdata/shot_NNN.ppm */
const char *fam_user_dir(void);
static void screenshot(void) {
    static int every = -1, n; static uint64_t last;
    if (every < 0) every = fam_config_int("shot", 0);
    if (!every) return;
    uint64_t now = SDL_GetTicks64();
    if (now - last < (uint64_t)every * 1000) return;
    last = now;
    int w = g_dev.out_w, h = g_dev.out_h;
    uint8_t *px = malloc((size_t)w * h * 4);
    glReadPixels(g_dev.out_x, g_dev.out_y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char path[1024]; snprintf(path, sizeof path, "%s/shot_%03d.ppm", fam_user_dir(), n++);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
        fclose(f);
    }
    free(px);
}
/* FAMILIA_FPS=1: frames per second in the top-left corner. A 3x5 pixel font
 * drawn as scissored clears (no textures, no shaders); GL state restored. */
static const uint16_t k_glyph[] = {      /* 5 rows top->bottom, one octal digit (3 bits, 4 = left) each */
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
};
static uint16_t glyph_of(char c) {
    if (c >= '0' && c <= '9') return k_glyph[c - '0'];
    switch (c) { case 'F': return 074744; case 'P': return 075744; case 'S': return 074717; }
    return 0;
}
static void glyph_bits(uint16_t g, int row, int out[3]) {
    static const int shift[5] = { 12, 9, 6, 3, 0 };
    int r = (g >> shift[row]) & 7;
    out[0] = r & 4; out[1] = r & 2; out[2] = r & 1;
}
static void fps_overlay(double fps) {
    char txt[16]; snprintf(txt, sizeof txt, "%d FPS", (int)(fps + 0.5));
    const int sc = g_dev.out_h >= 960 ? 4 : 2, cw = 4 * sc, x0 = g_dev.out_x + 4, h = g_dev.out_y + g_dev.out_h;
    int len = (int)strlen(txt);
    /* no state queries (each would stall the GL thread): the renderer keeps
     * the scissor test off between draws and sets clear color and scissor
     * box before using them (Clear, viewport) */
    glEnable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glScissor(x0 - 2, h - 4 - 5 * sc - 2, len * cw + 2, 5 * sc + 4);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(1.f, 0.85f, 0.2f, 1);
    for (int i = 0; i < len; i++) {
        uint16_t g = glyph_of(txt[i]);
        for (int row = 0; row < 5; row++) {
            int bits[3]; glyph_bits(g, row, bits);
            for (int col = 0; col < 3; col++) if (bits[col]) {
                glScissor(x0 + i * cw + col * sc, h - 4 - (row + 1) * sc, sc, sc);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
    }
    glDisable(GL_SCISSOR_TEST);
    fam_apply_write_masks();
    g_dev.state_dirty |= FAM_DIRTY_VP;          /* the scissor box: re-applied with the viewport */
}
/* PGO training runs (FAMILIA_GCOV_DUMP=SECONDS): write the profile counters
 * once that long after start, then keep running. Weak: absent in normal builds. */
#ifdef FAMILIA_PGO_GEN
extern void __gcov_dump(void);        /* a strong reference pulls it out of libgcov */
#else
extern void __gcov_dump(void) __attribute__((weak));
#endif
static void gcov_hook(void) {
    static int state = 0; static uint64_t t0;
    if (state == 2 || !__gcov_dump) return;
    uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    if (!state) { const char *e = getenv("FAMILIA_GCOV_DUMP"); if (!e) { state = 2; return; } t0 = now; state = 1; }
    const char *e = getenv("FAMILIA_GCOV_DUMP");
    if ((double)(now - t0) / (double)f >= atof(e)) { __gcov_dump(); fprintf(stderr, "[pgo] profile written\n"); state = 2; }
}
void fam_present_hook(void) {
    gcov_hook();
    static int crash_ok;           /* after SDL video/audio and the GL driver have installed theirs */
    if (!crash_ok) { void fam_crash_install(void); fam_crash_install(); crash_ok = 1; }
    static int fps_on = -1; static double fps_shown; static uint64_t fps_t0, fps_n;
    if (fps_on < 0) fps_on = fam_config_int("fps", 0);
    if (fps_on) {
        uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
        if (!fps_t0) fps_t0 = now;
        fps_n++;
        if (now - fps_t0 >= f / 2) { fps_shown = (double)fps_n * (double)f / (double)(now - fps_t0); fps_t0 = now; fps_n = 0; }
        fps_overlay(fps_shown);
    }
    screenshot();
    /* FAMILIA_LIMIT=30|60: cap presents per second (sleep, then a short spin) */
    static int limit = -1; static uint64_t next;
    if (limit < 0) limit = fam_config_int("limit", 0);
    if (limit > 0) {
        uint64_t f = SDL_GetPerformanceFrequency(), step = f / (uint64_t)limit, now = SDL_GetPerformanceCounter();
        if (!next || now > next + step) next = now;
        else {
            while ((int64_t)(next - now) > (int64_t)(f / 500)) { SDL_Delay((Uint32)((next - now) * 1000 / f) - 1); now = SDL_GetPerformanceCounter(); }
            while (now < next) now = SDL_GetPerformanceCounter();
        }
        next += step;
    }
    fam_input_pump();
    {   /* development: FAMILIA_SECTORDBG=1 - how often the camera's sound sector changes */
        static int dbg = -1; if (dbg < 0) dbg = fam_config_int("sectordbg", 0);
        if (dbg) {
            static uint32_t last, changes, frames; uint32_t s = MEM32(0x101C4D00u);
            if (s != last) changes++, last = s;
            if (++frames % 150 == 0) {
                fprintf(stderr, "[sectordbg] %u changes in 150 frames, sector %08X sounds %u\n", changes, s, s ? MEM32(s + 0x160) : 0);
                changes = 0;
            }
        }
    }
    static int stats = -1;
    if (stats < 0) stats = fam_config_int("stats", 0);
    uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    if (g_last && g_nft < WIN) g_ft[g_nft++] = (double)(now - g_last) * 1000.0 / (double)f;
    if (g_last) { void fam_probe_frame(double); fam_probe_frame((double)(now - g_last) * 1000.0 / (double)f); }
    g_last = now;
    if (!g_window_start) g_window_start = now;
    g_window_frames++;
    if (stats && (now - g_window_start) > 5 * f && g_nft > 1) {
        double s = (double)(now - g_window_start) / (double)f, sorted[WIN];
        memcpy(sorted, g_ft, sizeof(double) * (size_t)g_nft);
        qsort(sorted, (size_t)g_nft, sizeof(double), cmpd);
        long rss_kb = 0, hwm_kb = 0;            /* resident memory, current and peak */
        FILE *st = fopen("/proc/self/status", "r");
        if (st) { char ln[128]; while (fgets(ln, sizeof ln, st)) { sscanf(ln, "VmRSS: %ld", &rss_kb); sscanf(ln, "VmHWM: %ld", &hwm_kb); } fclose(st); }
        /* where the wall time of a frame goes: game-thread CPU, waiting for
         * the machine lock, and inside the buffer swap */
        static long main_tid; static uint64_t last_cpu_ticks, last_wait, last_swap;
        if (!main_tid) main_tid = (long)syscall(SYS_gettid);
        uint64_t cpu_ticks = 0;
        { char sp[64]; snprintf(sp, sizeof sp, "/proc/self/task/%ld/stat", main_tid);
          FILE *tf = fopen(sp, "r");
          if (tf) { char buf[512]; size_t n = fread(buf, 1, sizeof buf - 1, tf); buf[n] = 0; fclose(tf);
                    char *q = strrchr(buf, ')'); unsigned long ut = 0, stt = 0;
                    if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &stt) == 2) cpu_ticks = ut + stt; } }
        extern volatile uint64_t hle_mach_wait_ns;
        double hz = (double)sysconf(_SC_CLK_TCK), frames = (double)g_window_frames;
        fprintf(stderr, "[stats] per frame: game-thread cpu %.1f ms, lock wait %.1f ms, swap %.1f ms | ",
                (double)(cpu_ticks - last_cpu_ticks) / hz * 1000.0 / frames,
                (double)(hle_mach_wait_ns - last_wait) / 1e6 / frames,
                (double)(fam_swap_ticks - last_swap) * 1000.0 / (double)f / frames);
        last_cpu_ticks = cpu_ticks; last_wait = hle_mach_wait_ns; last_swap = fam_swap_ticks;
        extern volatile uint64_t hle_mach_takes, hle_yields;
        static uint64_t last_takes, last_yields;
        fprintf(stderr, "[stats] rss %ld MB peak %ld MB | lock takes/s %.0f yields/s %.0f | ", rss_kb >> 10, hwm_kb >> 10,
                (double)(hle_mach_takes - last_takes) / s, (double)(hle_yields - last_yields) / s);
        last_takes = hle_mach_takes; last_yields = hle_yields;
        {   /* CPU clock and temperature: the H700 throttles above 60 C */
            long khz = 0, mc = 0; FILE *q;
            if ((q = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r"))) { if (fscanf(q, "%ld", &khz) != 1) khz = 0; fclose(q); }
            if ((q = fopen("/sys/class/thermal/thermal_zone0/temp", "r"))) { if (fscanf(q, "%ld", &mc) != 1) mc = 0; fclose(q); }
            if (khz || mc) fprintf(stderr, "cpu %ld MHz %.1f C | ", khz / 1000, mc / 1000.0);
        }
        if (getenv("HLE_SHIMPROF")) {          /* the shims that took the most time this window, per frame */
            extern uint64_t hle_shim_ns[8192]; extern uint32_t hle_shim_calls[8192];
            int hle_shim_count(void); const char *hle_shim_name_at(int);
            static uint64_t last_ns[8192]; static uint32_t last_c[8192];
            int n = hle_shim_count(); if (n > 8192) n = 8192;
            int top[12] = {0}; double tv[12] = {0}; double tot = 0;
            for (int i = 0; i < n; i++) {
                double v = (double)(hle_shim_ns[i] - last_ns[i]); tot += v;
                for (int k = 0; k < 12; k++) if (v > tv[k]) { for (int m = 11; m > k; m--) tv[m] = tv[m - 1], top[m] = top[m - 1]; tv[k] = v; top[k] = i; break; }
            }
            double fr = (double)g_window_frames;
            fprintf(stderr, "[shims] per frame %.2f ms:", tot / 1e6 / fr);
            for (int k = 0; k < 12 && tv[k] > 0; k++)
                fprintf(stderr, " %s %.2f ms/%.0f", hle_shim_name_at(top[k]), tv[k] / 1e6 / fr, (hle_shim_calls[top[k]] - last_c[top[k]]) / fr);
            fprintf(stderr, "\n");
            for (int i = 0; i < n; i++) last_ns[i] = hle_shim_ns[i], last_c[i] = hle_shim_calls[i];
        }
        {   /* development: FAMILIA_PEEK=addr,addr,... prints those guest words */
            const char *pk = getenv("FAMILIA_PEEK");
            if (pk) {
                char b[256]; snprintf(b, sizeof b, "%s", pk);
                fprintf(stderr, "[peek]");
                for (char *t = strtok(b, ","); t; t = strtok(NULL, ",")) { uint32_t a = (uint32_t)strtoul(t, NULL, 16); fprintf(stderr, " %08X=%08X", a, MEM32(a)); }
                fprintf(stderr, " | ");
            }
        }
        {   /* what grows over a session: lists the game or the port walks or searches */
            extern uint64_t hle_vm_reserves, hle_vm_releases, hle_vm_ns; int hle_vm_ranges(void);
            extern int fam_tex_alive, fam_buf_alive; uint64_t fam_uc_nodes(void); extern uint32_t fam_sounds_last, fam_fftree_removes;
            static uint64_t lr, lf, lns, lnodes;
            double fr = (double)g_window_frames;
            fprintf(stderr, "[growth] heap ranges %d, large allocs %.1f frees %.1f %.2f ms/frame | textures %d buffers %d | "
                    "frame nodes updated %.0f/frame | sounds %u | name tree %d (inserts %u)\n",
                    hle_vm_ranges(), (hle_vm_reserves - lr) / fr, (hle_vm_releases - lf) / fr, (hle_vm_ns - lns) / 1e6 / fr,
                    fam_tex_alive, fam_buf_alive, (fam_uc_nodes() - lnodes) / fr, fam_sounds_last, (int)(MEM32(0x100C0044u) - fam_fftree_removes), MEM32(0x100C0044u));
            lr = hle_vm_reserves; lf = hle_vm_releases; lns = hle_vm_ns; lnodes = fam_uc_nodes();
            /* where the memory is: the guest heap's parts, the host's malloc, the process's kinds of pages */
            extern uint64_t hle_mem_small_carved, hle_mem_small_used, hle_mem_large_live; uint64_t hle_mem_lcache_bytes(void);
            struct mallinfo2 mi = mallinfo2();
            char st[2048] = ""; FILE *sf = fopen("/proc/self/status", "r");
            long anon = -1, file = -1, shm = -1;
            if (sf) { while (fgets(st, sizeof st, sf)) { sscanf(st, "RssAnon: %ld", &anon); sscanf(st, "RssFile: %ld", &file); sscanf(st, "RssShmem: %ld", &shm); } fclose(sf); }
            fprintf(stderr, "[mem] guest small %.0f MB carved %.0f MB in use | guest large %.0f MB live, %.0f MB cached | host malloc %.0f MB in use %.0f MB mmapped | rss anon %ld file %ld shmem %ld MB\n",
                    hle_mem_small_carved / 1048576.0, hle_mem_small_used / 1048576.0, hle_mem_large_live / 1048576.0, hle_mem_lcache_bytes() / 1048576.0,
                    mi.uordblks / 1048576.0, mi.hblkhd / 1048576.0, anon / 1024, file / 1024, shm / 1024);
        }
        fprintf(stderr, "%.1f fps | frame ms p50 %.1f p95 %.1f p99 %.1f max %.1f | draws/frame %.0f\n",
                (double)g_window_frames / s, sorted[g_nft / 2], sorted[g_nft * 95 / 100], sorted[g_nft * 99 / 100], sorted[g_nft - 1],
                (double)(g_draw_stats.draws - g_window_draws) / (double)g_window_frames);
        { void fam_probe_report(double); fam_probe_report((double)g_window_frames); }
        { void fam_stream_report(double); fam_stream_report((double)g_window_frames); }
        { static fam_draw_stats last; double fr = (double)g_window_frames;
          fprintf(stderr, "[gpu] per frame: tex uploads %.1f (%.0f KB) | buffer updates %.0f KB | vertex ranges reused %.0f | programs built %llu (total %llu)\n",
                  (double)(g_draw_stats.tex_uploads - last.tex_uploads) / fr, (double)(g_draw_stats.tex_bytes - last.tex_bytes) / 1024.0 / fr,
                  (double)(g_draw_stats.buf_bytes - last.buf_bytes) / 1024.0 / fr, (double)(g_draw_stats.vreuse - last.vreuse) / fr,
                  (unsigned long long)(g_draw_stats.progs - last.progs), (unsigned long long)g_draw_stats.progs);
          last = g_draw_stats; }
        g_window_start = now; g_window_frames = 0; g_nft = 0; g_window_draws = g_draw_stats.draws;
    }
}
