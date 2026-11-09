/*
 * probe.c - inclusive timers around chosen translated functions.
 *
 * tools/native_hooks.py wraps each function listed in config/probes.json
 * (build/gen/probes.c names them). With FAMILIA_PROBE=1 every call reads the
 * ARM generic timer on entry and exit (a few ns); recursion is timed once, at
 * the outermost call. fam_probe_report() prints, per frame: calls, inclusive
 * milliseconds and the share of calls that returned nonzero in al (bool
 * results, e.g. PrepareLights relit the vertices).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "recomp_types.h"

extern const char *const fam_probe_names[];
extern const int fam_probe_count;

#define MAXP 64
int fam_probe_on;
static struct { uint64_t calls, ticks, yes; int depth; } g_p[MAXP], g_last[MAXP];

static inline uint64_t now_ticks(void) {
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
#endif
}
static double tick_hz(void) {
#if defined(__aarch64__)
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return (double)f;
#else
    return 1e9;
#endif
}

uint64_t fam_probe_in(int id) {
    g_p[id].calls++;
    return g_p[id].depth++ ? 0 : now_ticks();
}
void fam_probe_out(int id, uint64_t t) {
    if (--g_p[id].depth == 0) g_p[id].ticks += now_ticks() - t;
    if (g_eax & 0xff) g_p[id].yes++;
}

static double g_spike_ms;
void fam_probe_init(void) {
    const char *e = getenv("FAMILIA_PROBE");
    fam_probe_on = e && atoi(e) && fam_probe_count > 0;
    const char *sp = getenv("FAMILIA_SPIKE");           /* frame time (ms) above which a frame is reported */
    if (sp && atof(sp) > 0 && fam_probe_count > 0) { g_spike_ms = atof(sp); fam_probe_on = 1; }
    if (fam_probe_on) fprintf(stderr, "[probe] %d timed functions\n", fam_probe_count);
}

void fam_probe_report(double frames) {
    if (!fam_probe_on || frames <= 0) return;
    double ms = 1000.0 / tick_hz();
    fprintf(stderr, "[probe] per frame:");
    for (int i = 0; i < fam_probe_count && i < MAXP; i++) {
        uint64_t c = g_p[i].calls - g_last[i].calls, t = g_p[i].ticks - g_last[i].ticks, y = g_p[i].yes - g_last[i].yes;
        if (c) fprintf(stderr, " | %s: %.0f calls %.2f ms%s", fam_probe_names[i], (double)c / frames,
                       (double)t * ms / frames, "");
        if (c && y) fprintf(stderr, " (%.0f%% true)", 100.0 * (double)y / (double)c);
        g_last[i] = g_p[i];
    }
    fprintf(stderr, "\n");
}

/* Once per presented frame: when the frame took longer than FAMILIA_SPIKE ms,
 * print what the timed functions and file I/O spent in that frame. */
extern uint64_t hle_io_read_ns, hle_io_read_bytes, hle_io_open_ns, hle_io_opens, fam_glt_wait_ns, hle_find_ns, hle_finds;
void fam_probe_frame(double frame_ms) {
    static uint64_t last[MAXP], lr_ns, lr_b, lo_ns, lo_n, lw, lf_ns, lf_n;
    if (g_spike_ms <= 0) return;
    if (frame_ms > g_spike_ms) {
        double ms = 1000.0 / tick_hz();
        int idx[MAXP]; double val[MAXP]; int n = 0;
        for (int i = 0; i < fam_probe_count && i < MAXP; i++) {
            double v = (double)(g_p[i].ticks - last[i]) * ms;
            if (v >= 1.0) { idx[n] = i; val[n] = v; n++; }
        }
        for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (val[b] > val[a]) { double t = val[a]; val[a] = val[b]; val[b] = t; int k = idx[a]; idx[a] = idx[b]; idx[b] = k; }
        fprintf(stderr, "[spike] %.1f ms frame | file read %.1f ms (%llu KB) open %.1f ms (%llu files)", frame_ms,
                (double)(hle_io_read_ns - lr_ns) / 1e6, (unsigned long long)((hle_io_read_bytes - lr_b) >> 10),
                (double)(hle_io_open_ns - lo_ns) / 1e6, (unsigned long long)(hle_io_opens - lo_n));
        fprintf(stderr, " find %.1f ms (%llu)", (double)(hle_find_ns - lf_ns) / 1e6, (unsigned long long)(hle_finds - lf_n));
        { char ops[512]; void fam_glt_sync_report(char *, size_t); fam_glt_sync_report(ops, sizeof ops);
          fprintf(stderr, " | GL sync wait %.1f ms:%s", (double)(fam_glt_wait_ns - lw) / 1e6, ops); }
        for (int a = 0; a < n && a < 8; a++) fprintf(stderr, " | %s %.1f", fam_probe_names[idx[a]], val[a]);
        fprintf(stderr, "\n");
    }
    if (frame_ms <= g_spike_ms) { char ops[512]; void fam_glt_sync_report(char *, size_t); fam_glt_sync_report(ops, sizeof ops); }
    for (int i = 0; i < fam_probe_count && i < MAXP; i++) last[i] = g_p[i].ticks;
    lf_ns = hle_find_ns; lf_n = hle_finds; lr_ns = hle_io_read_ns; lr_b = hle_io_read_bytes; lo_ns = hle_io_open_ns; lo_n = hle_io_opens; lw = fam_glt_wait_ns;
}
