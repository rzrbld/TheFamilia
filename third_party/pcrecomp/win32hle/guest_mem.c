/*
 * guest_mem.c - the memory a guest can see: page reservations (VirtualAlloc,
 * the PE images, thread stacks), the heap (HeapAlloc, Local/Global, COM
 * objects, surfaces) and per-thread scratch. See win32hle.h.
 *
 * Flat (HLE_FLAT: any 32-bit host): a VA is a host address, so pages
 * are mmaps where the guest asks for them and the heap is malloc.
 *
 * Based (a 64-bit host): the guest's 4 GB is one mapping at g_mem_base, and
 * nothing outside it has a VA. Pages are ranges of that mapping, tracked here
 * and left inaccessible until reserved, as on Windows; the heap is size
 * classes carved from reservations. Host pages may be larger than the guest's
 * 4 KB (16 KB on Apple silicon), so reservations go in 64 KB units, the
 * Windows allocation granularity.
 */
#define _GNU_SOURCE   /* MAP_ANONYMOUS */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include "win32hle.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define HDR   16u            /* [size][magic][class][-] before every heap block */
#define MAGIC 0x48454150u    /* 'HEAP' */
#define GRAN  0x10000u       /* the reservation unit */

#ifdef HLE_FLAT
/* ================================================================ flat */

void hle_mem_init(void) {}

uint32_t hle_vm_reserve(uint32_t va, uint32_t size) {
    void *p = mmap(va ? (void *)(uintptr_t)(va & ~0xFFFFu) : NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | (va ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED) return 0;
    if (va && (uint32_t)(uintptr_t)p != (va & ~0xFFFFu)) { munmap(p, size); return 0; }
    return (uint32_t)(uintptr_t)p;
}
void hle_vm_release(uint32_t va, uint32_t size) { munmap((void *)(uintptr_t)va, size); }
void hle_vm_zero(uint32_t va, uint32_t size)    { madvise((void *)(uintptr_t)va, size, MADV_DONTNEED); }

void *hle_heap_alloc(uint32_t n, int zero) {
    uint8_t *p = (uint8_t *)(zero ? calloc(1, (size_t)n + HDR) : malloc((size_t)n + HDR));
    if (!p) return NULL;
    ((uint32_t *)p)[0] = n, ((uint32_t *)p)[1] = MAGIC;
    return p + HDR;
}
void hle_heap_free(void *u) {
    if (!u) return;
    if (!hle_heap_ok(u)) { fprintf(stderr, "[win32hle] free of a block this heap did not hand out: %p\n", u); return; }
    ((uint32_t *)((uint8_t *)u - HDR))[1] = 0;
    free((uint8_t *)u - HDR);
}
void *hle_heap_realloc(void *u, uint32_t n, int zero) {
    if (!u) return hle_heap_alloc(n, zero);
    uint32_t old = hle_heap_size(u);
    uint8_t *p = (uint8_t *)realloc((uint8_t *)u - HDR, (size_t)n + HDR);
    if (!p) return NULL;
    ((uint32_t *)p)[0] = n;
    if (zero && n > old) memset(p + HDR + old, 0, n - old);
    return p + HDR;
}

#else
/* =============================================================== based */

#define SPAN      0x100000000ull       /* the guest's whole 32-bit space */
#define ANY_LO    0x10000000u          /* "anywhere" starts above the images */
#define ANY_HI    0x7F000000u          /* under GetSystemInfo's highest application address */
#define SHIM_LO   0x7F000000u          /* host_lite.c's synthetic import VAs: never mapped */
#define SHIM_HI   0x80000000u
#define MAX_RANGES 8192

static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t lo, hi; } g_rng[MAX_RANGES];   /* reserved, sorted by lo */
static int g_nrng;
static size_t g_host_page;

uint32_t hle_va_bad(const void *p) {
    hle_fatal("host pointer %p handed to the guest (not in guest memory at %p)", p, (void *)g_mem_base);
    return 0;
}

static void mem_init_once(void) {
    g_host_page = (size_t)getpagesize();
    /* Nothing is accessible until reserved, page zero included: a guest NULL
     * (or a VA nobody handed out) faults, as it would on Windows. */
#ifdef RECOMP_MEM_BASE
    /* The lift adds RECOMP_MEM_BASE as a constant (recomp_types.h): the block
     * goes there or nowhere. Asked for, not forced, so nothing the host has
     * mapped is replaced (Linux says so with NOREPLACE; macOS honours a free
     * hint). */
    void *want = (void *)(uintptr_t)(RECOMP_MEM_BASE);
    void *p = mmap(want, SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
#ifdef __linux__
                   | MAP_FIXED_NOREPLACE
#endif
                   , -1, 0);
    if (p != want) hle_fatal("cannot reserve the guest's 4 GB at %p (RECOMP_MEM_BASE): the range is in use", want);
#else
    void *p = mmap(NULL, SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) hle_fatal("cannot reserve the guest's 4 GB");
#endif
    g_mem_base = (ptrdiff_t)p;
}
void hle_mem_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, mem_init_once);
}

static uint8_t *host(uint32_t va) { return (uint8_t *)g_mem_base + va; }

/* Fresh zero pages over [va, va+size), host-page aligned, with prot. */
static int remap(uint32_t va, uint64_t size, int prot) {
    void *p = mmap(host(va), (size_t)size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | (prot ? 0 : MAP_NORESERVE), -1, 0);
    return p == (void *)host(va) ? 0 : -1;
}

/* [familia] The table is searched by binary search and free space found next-fit from
 * a moving cursor: with thousands of live large blocks (texture levels, buffers) the
 * linear scans made every large allocation and free slower the longer a game ran. */
static int rng_ge(uint64_t lo) {                         /* the first range with .lo >= lo */
    int a = 0, b = g_nrng;
    while (a < b) { int m = (a + b) / 2; if (g_rng[m].lo < lo) a = m + 1; else b = m; }
    return a;
}
static int overlaps(uint64_t lo, uint64_t hi) {
    int i = rng_ge(lo);
    if (i > 0 && g_rng[i - 1].hi > lo) return 1;
    return i < g_nrng && g_rng[i].lo < hi;
}
static void add_range(uint64_t lo, uint64_t hi) {
    int i = rng_ge(lo);
    memmove(&g_rng[i + 1], &g_rng[i], sizeof g_rng[0] * (size_t)(g_nrng - i));
    g_rng[i].lo = lo, g_rng[i].hi = hi;
    g_nrng++;
}

/* The first gap of size at or above `from` and below `to`, past the shim range. */
static uint64_t gap_from(uint64_t from, uint64_t to, uint32_t size) {
    uint64_t at = from;
    int i = rng_ge(from);
    if (i > 0 && g_rng[i - 1].hi > at) at = g_rng[i - 1].hi;
    for (; ; i++) {
        uint64_t next = i < g_nrng ? g_rng[i].lo : ANY_HI;
        if (at < SHIM_HI && at + size > SHIM_LO) at = SHIM_HI;
        if (at + size > to) return 0;
        if (next > at && next - at >= size && !(at < SHIM_HI && at + size > SHIM_LO)) return at;
        if (i >= g_nrng) return 0;
        if (g_rng[i].hi > at) at = g_rng[i].hi;
    }
}
/* Next fit: from the cursor up, then once from the bottom. */
static uint64_t g_rover = ANY_LO;
static uint32_t find_gap(uint32_t size) {
    uint64_t at = gap_from(g_rover, ANY_HI, size);
    if (!at) at = gap_from(ANY_LO, ANY_HI, size);
    if (!at) return 0;
    g_rover = at + size;
    return (uint32_t)at;
}

uint64_t hle_vm_reserves, hle_vm_releases, hle_vm_ns;   /* [familia] growth report: large blocks */
int hle_vm_ranges(void) { return g_nrng; }
static uint64_t vm_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint32_t vm_reserve_locked_(uint32_t va, uint32_t size);
static uint32_t vm_reserve_locked(uint32_t va, uint32_t size) {
    uint64_t t = vm_now(); uint32_t r = vm_reserve_locked_(va, size); hle_vm_ns += vm_now() - t; hle_vm_reserves++; return r;
}
static uint32_t vm_reserve_locked_(uint32_t va, uint32_t size) {
    if (!size) return 0;
    uint64_t lo = va & ~(uint64_t)(GRAN - 1);
    uint64_t hi = ((uint64_t)(va ? va : 0) + size + GRAN - 1) & ~(uint64_t)(GRAN - 1);
    if (!va) {
        uint64_t n = ((uint64_t)size + GRAN - 1) & ~(uint64_t)(GRAN - 1);
        if (n >= SPAN) return 0;
        lo = find_gap((uint32_t)n);
        if (!lo) return 0;
        hi = lo + n;
    } else if (lo < GRAN || hi > SPAN || overlaps(lo, hi)) {
        return 0;                                         /* page zero stays unmapped */
    }
    if (g_nrng >= MAX_RANGES || remap((uint32_t)lo, hi - lo, PROT_READ | PROT_WRITE)) return 0;
    add_range(lo, hi);
    return (uint32_t)lo;
}

uint32_t hle_vm_reserve(uint32_t va, uint32_t size) {
    hle_mem_init();
    pthread_mutex_lock(&g_mem_lock);
    uint32_t r = vm_reserve_locked(va, size);
    pthread_mutex_unlock(&g_mem_lock);
    return r;
}

/* Release the reservation that starts at va (size is the caller's note of
 * it; the table knows). Its pages go back to inaccessible and zero. */
static void vm_release_locked_(uint32_t va);
static void vm_release_locked(uint32_t va) { uint64_t t = vm_now(); vm_release_locked_(va); hle_vm_ns += vm_now() - t; hle_vm_releases++; }
static void vm_release_locked_(uint32_t va) {
    int i = rng_ge(va);
    if (i >= g_nrng || g_rng[i].lo != va) return;
    remap((uint32_t)g_rng[i].lo, g_rng[i].hi - g_rng[i].lo, PROT_NONE);
    memmove(&g_rng[i], &g_rng[i + 1], sizeof g_rng[0] * (size_t)(g_nrng - i - 1));
    g_nrng--;
}
void hle_vm_release(uint32_t va, uint32_t size) {
    (void)size;
    pthread_mutex_lock(&g_mem_lock);
    vm_release_locked(va);
    pthread_mutex_unlock(&g_mem_lock);
}

/* Decommit: the range reads as zero when touched again. Whole host pages are
 * replaced (dropping their memory); the partial ones at either end cleared. */
void hle_vm_zero(uint32_t va, uint32_t size) {
    uint64_t lo = va, hi = (uint64_t)va + size;
    uint64_t plo = (lo + g_host_page - 1) & ~(uint64_t)(g_host_page - 1), phi = hi & ~(uint64_t)(g_host_page - 1);
    if (plo >= phi) { memset(host(va), 0, size); return; }
    memset(host((uint32_t)lo), 0, (size_t)(plo - lo));
    remap((uint32_t)plo, phi - plo, PROT_READ | PROT_WRITE);
    memset(host((uint32_t)phi), 0, (size_t)(hi - phi));
}

/* ---- the heap ----
 * Small blocks (header included, up to 32 KB) in size classes, four to each
 * power of two, carved from 1 MB reservations and kept on a free list per
 * class once freed. Large ones are reservations of their own, returned on
 * free. A free small block's first word links the list (a VA). */
#define SMALL_MAX  0x8000u
#define CARVE      0x100000u
#define NCLASS     64
#define LARGE      0xFFu

static uint32_t g_free[NCLASS];
uint64_t hle_mem_small_carved, hle_mem_small_used, hle_mem_large_live;   /* [familia] memory report */
uint64_t hle_mem_lcache_bytes(void);
static uint32_t g_carve, g_carve_end;

static uint32_t class_size(int c) {       /* c 0..: 32, 48, 64, 80, 96, 112, 128, 160, 192, ... */
    if (c < 7) return 32u + 16u * (uint32_t)c;
    int k = c - 7;                         /* 128 * 2^(k/4) * (1 + (k%4+1)/4) */
    uint32_t p = 128u << (k / 4);
    return p + p / 4u * (uint32_t)(k % 4 + 1);
}
static int class_of(uint32_t total) {
    for (int c = 0; c < NCLASS; c++) if (class_size(c) >= total) return c;
    return -1;
}

static uint32_t small_alloc(int c) {
    uint32_t va = g_free[c];
    if (va) { g_free[c] = *(uint32_t *)host(va + HDR); return va; }
    uint32_t sz = class_size(c);
    if (g_carve + sz > g_carve_end || !g_carve) {
        uint32_t r = vm_reserve_locked(0, CARVE);
        if (!r) return 0;
        g_carve = r, g_carve_end = r + CARVE;
        hle_mem_small_carved += CARVE;
    }
    va = g_carve;
    g_carve += sz;
    return va;
}

/* [familia] Freed large heap blocks are kept resident and reused (bounded),
 * instead of being unmapped and mapped fresh: a game that allocates and frees
 * a big temporary buffer every frame otherwise page-faults through all of it
 * (on the device that showed up as ~24% of CPU inside memset). */
#define LCACHE_N   64
#define LCACHE_MAX (64u << 20)
static struct { uint32_t va, span; } g_lcache[LCACHE_N];
static int g_nlcache;
static uint64_t g_lcache_bytes;
static uint32_t range_span(uint32_t va);
uint64_t hle_mem_lcache_bytes(void) { return g_lcache_bytes; }
static uint32_t range_span(uint32_t va) {
    int i = rng_ge(va);
    return i < g_nrng && g_rng[i].lo == va ? (uint32_t)(g_rng[i].hi - g_rng[i].lo) : 0;
}
static uint32_t lcache_take(uint32_t total) {            /* g_mem_lock held */
    int best = -1;
    for (int i = 0; i < g_nlcache; i++)
        if (g_lcache[i].span >= total && (uint64_t)g_lcache[i].span <= (uint64_t)total * 2 + GRAN &&
            (best < 0 || g_lcache[i].span < g_lcache[best].span)) best = i;
    if (best < 0) return 0;
    uint32_t va = g_lcache[best].va;
    g_lcache_bytes -= g_lcache[best].span;
    g_lcache[best] = g_lcache[--g_nlcache];
    return va;
}
static int lcache_put(uint32_t va) {                     /* g_mem_lock held */
    uint32_t span = range_span(va);
    if (!span || g_nlcache >= LCACHE_N || g_lcache_bytes + span > LCACHE_MAX) return 0;
    g_lcache[g_nlcache].va = va; g_lcache[g_nlcache].span = span; g_nlcache++;
    g_lcache_bytes += span;
    return 1;
}

void *hle_heap_alloc(uint32_t n, int zero) {
    hle_mem_init();
    if (n > 0xFFFFFFFFu - GRAN - HDR) return NULL;
    uint32_t total = n + HDR, va;
    int c = total <= SMALL_MAX ? class_of(total) : -1;
    pthread_mutex_lock(&g_mem_lock);
    va = c >= 0 ? small_alloc(c) : lcache_take(total);
    if (!va && c < 0) va = vm_reserve_locked(0, total);
    if (va) { if (c >= 0) hle_mem_small_used += class_size(c); else hle_mem_large_live += range_span(va); }
    pthread_mutex_unlock(&g_mem_lock);
    if (!va) return NULL;
    uint32_t *h = (uint32_t *)host(va);
    h[0] = n, h[1] = MAGIC, h[2] = c >= 0 ? (uint32_t)c : LARGE, h[3] = 0;
    if (zero) memset(h + 4, 0, n);
    return h + 4;
}

static uint32_t capacity(const uint32_t *h) {
    return h[2] == LARGE ? ((h[0] + HDR + GRAN - 1) & ~(GRAN - 1)) - HDR : class_size((int)h[2]) - HDR;
}

void hle_heap_free(void *u) {
    if (!u) return;
    if (!hle_heap_ok(u)) { fprintf(stderr, "[win32hle] free of a block this heap did not hand out: %p\n", u); return; }
    uint32_t *h = (uint32_t *)u - 4;
    uint32_t va = hle_va(h);
    h[1] = 0;
    pthread_mutex_lock(&g_mem_lock);
    if (h[2] == LARGE) { hle_mem_large_live -= range_span(va); if (!lcache_put(va)) vm_release_locked(va); }
    else { hle_mem_small_used -= class_size((int)h[2]); *(uint32_t *)u = g_free[h[2]]; g_free[h[2]] = va; }
    pthread_mutex_unlock(&g_mem_lock);
}

/* [familia] The end of the large heap block holding va (0 if va is not in one):
 * for checks that snapshot a whole array. */
uint32_t hle_heap_block_end(uint32_t va) {
    pthread_mutex_lock(&g_mem_lock);
    uint32_t end = 0;
    int i = rng_ge(va);
    if (!(i < g_nrng && g_rng[i].lo == va)) i--;                  /* the range starting at or below va */
    if (i >= 0 && i < g_nrng && g_rng[i].lo <= va && va < g_rng[i].hi) {
        const uint32_t *h = (const uint32_t *)host((uint32_t)g_rng[i].lo);
        if (h[1] == MAGIC && h[2] == LARGE) end = (uint32_t)g_rng[i].lo + HDR + h[0];
    }
    pthread_mutex_unlock(&g_mem_lock);
    return end;
}

/* [familia] Trim, at a mission change (the previous mission's data has just
 * been freed): the freed large blocks kept for reuse go back to the system,
 * and so do the whole pages inside free small blocks of four pages or more
 * (header and list link stay). Each free block is trimmed once (a mark in its
 * header's spare word, cleared when it is handed out again). */
#define TRIM_MARK 0x5452494Du   /* 'TRIM' */
uint64_t hle_mem_trimmed;
void hle_heap_trim(void) {
    hle_mem_init();
    pthread_mutex_lock(&g_mem_lock);
    for (int i = 0; i < g_nlcache; i++) vm_release_locked(g_lcache[i].va);
    g_nlcache = 0; g_lcache_bytes = 0;
    uint64_t pg = g_host_page;
    for (int c = 0; c < NCLASS; c++) {
        uint32_t sz = class_size(c);
        if (sz < 4 * pg) continue;
        for (uint32_t va = g_free[c]; va; va = *(uint32_t *)host(va + HDR)) {
            uint32_t *h = (uint32_t *)host(va);
            if (h[3] == TRIM_MARK) continue;
            uint64_t lo = ((uint64_t)va + HDR + 4 + pg - 1) & ~(pg - 1), hi = ((uint64_t)va + sz) & ~(pg - 1);
            if (hi > lo) { madvise(host((uint32_t)lo), (size_t)(hi - lo), MADV_DONTNEED); hle_mem_trimmed += hi - lo; }
            h[3] = TRIM_MARK;
        }
    }
    pthread_mutex_unlock(&g_mem_lock);
}

void *hle_heap_realloc(void *u, uint32_t n, int zero) {
    if (!u) return hle_heap_alloc(n, zero);
    if (!hle_heap_ok(u)) return NULL;
    uint32_t *h = (uint32_t *)u - 4, old = h[0];
    if (n <= capacity(h)) {                /* in place (a large block's capacity shrinks with it: harmless) */
        if (zero && n > old) memset((uint8_t *)u + old, 0, n - old);
        h[0] = n;
        return u;
    }
    void *p = hle_heap_alloc(n, zero);
    if (!p) return NULL;
    memcpy(p, u, old < n ? old : n);
    hle_heap_free(u);
    return p;
}
#endif

/* ================================================================ both */

int hle_heap_ok(const void *u) {
    return u && ((const uint32_t *)((const uint8_t *)u - HDR))[1] == MAGIC;
}
uint32_t hle_heap_size(const void *u) {
    return hle_heap_ok(u) ? ((const uint32_t *)((const uint8_t *)u - HDR))[0] : 0;
}
void hle_heap_resize(void *u, uint32_t n) {
    if (hle_heap_ok(u) && n <= hle_heap_size(u)) ((uint32_t *)((uint8_t *)u - HDR))[0] = n;
}

void *hle_alloc(uint32_t n) { return hle_heap_alloc(n, 1); }
void  hle_free(void *p)     { hle_heap_free(p); }

uint32_t hle_guest_strdup(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    char *p = (char *)hle_heap_alloc((uint32_t)n, 0);
    if (!p) return 0;
    memcpy(p, s, n);
    return hle_va(p);
}

/* ---- scratch ----
 * 256 KB a thread, reserved on its first use and kept. */
#define SCRATCH 0x40000u
static __thread uint32_t g_scr_lo, g_scr_top;

uint32_t hle_scratch_mark(void) { return g_scr_top; }
uint32_t hle_scratch(uint32_t n) {
    if (!g_scr_lo) {
        void *p = hle_heap_alloc(SCRATCH, 1);
        if (!p) hle_fatal("no guest memory for scratch");
        g_scr_lo = g_scr_top = hle_va(p);
    }
    uint32_t va = (g_scr_top + 15u) & ~15u;
    if (va + n > g_scr_lo + SCRATCH) hle_fatal("guest scratch exhausted (%u bytes)", n);
    g_scr_top = va + n;
    memset(hle_ptr(va), 0, n);
    return va;
}
uint32_t hle_scratch_copy(const void *src, uint32_t n) {
    uint32_t va = hle_scratch(n);
    memcpy(hle_ptr(va), src, n);
    return va;
}
void hle_scratch_release(uint32_t mark) { if (g_scr_lo) g_scr_top = mark ? mark : g_scr_lo; }

/* ---- printf with the guest's arguments ----
 * wsprintfA and a game's own debug printf hand over a format and their
 * arguments on the guest stack. A va_list cannot be made from guest memory
 * off i386, and a %s there is a VA, so each conversion is read from args and
 * formatted on its own. Returns the length written (out is always ended). */
int hle_guest_format(char *out, size_t n, const char *fmt, uint32_t args) {
    size_t k = 0;
    if (!n) return 0;
    out[0] = 0;
    while (fmt && *fmt && k + 1 < n) {
        if (*fmt != '%') { out[k++] = *fmt++; continue; }
        if (fmt[1] == '%') { out[k++] = '%'; fmt += 2; continue; }
        char spec[32];
        size_t s = 0;
        spec[s++] = *fmt++;
        while (*fmt && strchr("-+ #0", *fmt) && s < 20) spec[s++] = *fmt++;
        for (int part = 0; part < 2; part++) {              /* width, then .precision */
            if (part) { if (*fmt != '.') break; spec[s++] = *fmt++; }
            if (*fmt == '*') {                               /* the number, cut at 28 like the digits: ll, the letter and the end must fit */
                s += (size_t)snprintf(spec + s, sizeof spec - s, "%d", (int32_t)MEM32(args)), args += 4, fmt++;
                if (s > 28) s = 28;
            }
            else while (*fmt >= '0' && *fmt <= '9' && s < 28) spec[s++] = *fmt++;
            if (s >= 28) break;
        }
        int wide = 0;                                        /* 64-bit: ll, I64 */
        for (;;) {
            if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L') { if (fmt[0] == 'l' && fmt[1] == 'l') wide = 1, fmt++; fmt++; }
            else if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') wide = 1, fmt += 3;
            else if (fmt[0] == 'I' && fmt[1] == '3' && fmt[2] == '2') fmt += 3;
            else break;
        }
        char c = *fmt ? *fmt++ : 0, piece[512];
        int r = 0;
        if (!c) break;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (wide) {
                uint64_t v = (uint64_t)MEM32(args) | (uint64_t)MEM32(args + 4) << 32;
                args += 8;
                spec[s++] = 'l', spec[s++] = 'l', spec[s++] = c, spec[s] = 0;
                r = c == 'd' || c == 'i' ? snprintf(piece, sizeof piece, spec, (long long)v) : snprintf(piece, sizeof piece, spec, (unsigned long long)v);
            } else {
                uint32_t v = MEM32(args);
                args += 4;
                spec[s++] = c, spec[s] = 0;
                r = c == 'd' || c == 'i' ? snprintf(piece, sizeof piece, spec, (int)(int32_t)v) : snprintf(piece, sizeof piece, spec, (unsigned)v);
            }
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            uint64_t bits = (uint64_t)MEM32(args) | (uint64_t)MEM32(args + 4) << 32;
            double d;
            memcpy(&d, &bits, 8);
            args += 8;
            spec[s++] = c, spec[s] = 0;
            r = snprintf(piece, sizeof piece, spec, d);
            break;
        }
        case 's': case 'S': {
            const char *str = (const char *)hle_ptr(MEM32(args));
            args += 4;
            spec[s++] = 's', spec[s] = 0;
            r = snprintf(piece, sizeof piece, spec, str ? str : "(null)");
            break;
        }
        case 'p':
            r = snprintf(piece, sizeof piece, "%08X", MEM32(args));
            args += 4;
            break;
        default:                                             /* not a conversion: as written */
            spec[s++] = c, spec[s] = 0;
            r = snprintf(piece, sizeof piece, "%s", spec);
            break;
        }
        if (r < 0) r = 0;
        if ((size_t)r >= sizeof piece) r = (int)sizeof piece - 1;
        for (int i = 0; i < r && k + 1 < n; i++) out[k++] = piece[i];
    }
    out[k] = 0;
    return (int)k;
}

uint32_t hle_guest_str_or_atom(const char *s) {
    if ((uintptr_t)s < 0x10000u) return (uint32_t)(uintptr_t)s;
#ifndef HLE_FLAT
    uintptr_t off = (uintptr_t)s - (uintptr_t)g_mem_base;
    if ((uint64_t)off >> 32) return hle_scratch_copy(s, (uint32_t)strlen(s) + 1);
#endif
    return hle_va(s);
}
