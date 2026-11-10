/*
 * readahead.c - learned read-ahead for the game's archives, per mission.
 *
 * The game reads its .dta archives in small pieces (about 20 KB) at scattered
 * offsets, and on the first visit to a part of the city each piece costs an
 * SD card access (about 3 ms; the archives are hundreds of MB on FAT32), all
 * on the game thread. This module remembers which 64 KB blocks of each
 * archive the game has read, separately for the menus and startup ("*") and
 * for each mission (conf/readahead2.txt). A low-priority thread reads a list's
 * blocks into the kernel's page cache ahead of the game: the startup list at
 * start, and a mission's list when that mission loads (the game opens
 * missions\<name>\scene2.bin from the archives: rw_data's dtaOpen, hooked
 * here), so a mission's streaming data is in RAM before the drive reaches it,
 * and blocks of missions not being played are not read at all.
 *
 * A mission change is also when the previous mission's data has just been
 * freed: the guest heap is trimmed then (hle_heap_trim: the freed-block
 * cache, and the pages inside large free blocks, go back to the system).
 *
 * The thread steps aside whenever the game itself read within the last
 * 30 ms, stops when free memory runs low (the page cache is the kernel's
 * anyway: it drops what it needs), and reads at most FAMILIA_READAHEAD_MB
 * (default 256) per list. FAMILIA_READAHEAD=0 turns it off.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include "familia.h"
#include "win32hle.h"
#include "recomp_types.h"

int fam_config_int(const char *key, int def);
const char *fam_game_dir(void);
void hle_heap_trim(void);

#define BLOCK (64u * 1024u)
#define MAXA 32
#define MAXK 64                     /* lists: "*" and the missions */
typedef struct {
    char name[64];
    uint64_t size;
    uint32_t nblocks;
} archive;
static archive g_a[MAXA];
static int g_na;
typedef struct {
    char key[48];
    uint8_t *seen[MAXA];            /* a bit per block of each archive (NULL: none yet) */
} list;
static list g_k[MAXK];
static int g_nk, g_cur;             /* the list reads are recorded in */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_job = -1;              /* the list the worker should read next */
static volatile uint64_t g_last_game_read;   /* ns, CLOCK_MONOTONIC */
static volatile int g_dirty;
static int g_on = -1;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static int archive_index(const char *name, uint64_t size) {     /* g_mu held */
    for (int i = 0; i < g_na; i++) if (!strcasecmp(g_a[i].name, name)) return g_a[i].size == size ? i : -1;
    if (g_na == MAXA || strlen(name) >= sizeof g_a[0].name) return -1;
    archive *a = &g_a[g_na];
    snprintf(a->name, sizeof a->name, "%s", name);
    a->size = size; a->nblocks = (uint32_t)((size + BLOCK - 1) / BLOCK);
    return g_na++;
}
static int list_index(const char *key) {                        /* g_mu held */
    for (int i = 0; i < g_nk; i++) if (!strcmp(g_k[i].key, key)) return i;
    if (g_nk == MAXK) return -1;
    snprintf(g_k[g_nk].key, sizeof g_k[0].key, "%s", key);
    return g_nk++;
}
static uint8_t *bits(int k, int a) {                            /* g_mu held */
    if (!g_k[k].seen[a]) g_k[k].seen[a] = calloc((g_a[a].nblocks + 7) / 8, 1);
    return g_k[k].seen[a];
}

/* fd -> archive, checked by device and inode (a closed fd's number is reused) */
static struct { int fd; dev_t dev; ino_t ino; int a; } g_fd[64];
static int archive_of_fd(int fd) {                              /* g_mu held */
    struct stat st;
    if (fstat(fd, &st)) return -1;
    unsigned h = (unsigned)fd & 63;
    if (g_fd[h].fd == fd && g_fd[h].dev == st.st_dev && g_fd[h].ino == st.st_ino) return g_fd[h].a;
    int a = -1;
    char lp[64], fn[1024]; snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd);
    ssize_t k = readlink(lp, fn, sizeof fn - 1);
    if (k > 4) {
        fn[k] = 0;
        const char *b = strrchr(fn, '/'); b = b ? b + 1 : fn;
        size_t l = strlen(b);
        if (l > 4 && !strcasecmp(b + l - 4, ".dta")) a = archive_index(b, (uint64_t)st.st_size);
    }
    g_fd[h].fd = fd; g_fd[h].dev = st.st_dev; g_fd[h].ino = st.st_ino; g_fd[h].a = a;
    return a;
}

/* every ReadFile of the game (kernel32_ext.c) */
void hle_read_hook(int fd, uint64_t off, uint32_t len) {
    if (g_on <= 0) return;
    g_last_game_read = now_ns();
    pthread_mutex_lock(&g_mu);
    int a = archive_of_fd(fd);
    if (a >= 0 && g_cur >= 0) {
        uint8_t *s = bits(g_cur, a);
        for (uint64_t b = off / BLOCK; s && b <= (off + len - 1) / BLOCK && b < g_a[a].nblocks; b++)
            if (!(s[b >> 3] & (1u << (b & 7)))) { s[b >> 3] |= (uint8_t)(1u << (b & 7)); g_dirty = 1; }
    }
    pthread_mutex_unlock(&g_mu);
}

/* conf/readahead2.txt: "@key" lines, each followed by "archive size hexbitmap" lines */
static void list_path(char *p, size_t n) { snprintf(p, n, "%s/readahead2.txt", fam_user_dir()); }
static void load_lists(void) {
    char p[1024]; list_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) {                         /* the earlier single list mixed all missions: start over */
        char old[1024]; snprintf(old, sizeof old, "%s/readahead.txt", fam_user_dir()); unlink(old);
        return;
    }
    char tok[64]; unsigned long long size; int k = -1;
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') { k = list_index(tok + 1); continue; }
        if (fscanf(f, "%llu ", &size) != 1) break;
        int a = archive_index(tok, size);
        uint8_t *s = (a >= 0 && k >= 0) ? bits(k, a) : NULL;
        for (uint32_t i = 0; s && i < (g_a[a].nblocks + 7) / 8; i++) { unsigned v; if (fscanf(f, "%2x", &v) != 1) break; s[i] = (uint8_t)v; }
        int c; while ((c = fgetc(f)) != EOF && c != '\n') {}
    }
    fclose(f);
}
static void save_lists(void) {
    char p[1024], t[1100]; list_path(p, sizeof p); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w");
    if (!f) return;
    pthread_mutex_lock(&g_mu);
    for (int k = 0; k < g_nk; k++) {
        fprintf(f, "@%s\n", g_k[k].key);
        for (int a = 0; a < g_na; a++) {
            if (!g_k[k].seen[a]) continue;
            fprintf(f, "%s %llu ", g_a[a].name, (unsigned long long)g_a[a].size);
            for (uint32_t i = 0; i < (g_a[a].nblocks + 7) / 8; i++) fprintf(f, "%02x", g_k[k].seen[a][i]);
            fputc('\n', f);
        }
    }
    g_dirty = 0;
    pthread_mutex_unlock(&g_mu);
    if (fclose(f) == 0) rename(t, p);
}

static long mem_available_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 1 << 20;
    char l[128]; long kb = -1;
    while (fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %ld", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? 1 << 20 : kb / 1024;
}

/* read one list's blocks ahead; stops early when another list is asked for */
static void read_list(int k) {
    uint64_t budget = (uint64_t)fam_config_int("readahead_mb", 256) << 20, done = 0, blocks = 0;
    uint64_t t0 = now_ns();
    static uint8_t buf[BLOCK];
    pthread_mutex_lock(&g_mu); int na = g_na; char key[48]; snprintf(key, sizeof key, "%s", g_k[k].key); pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < na && done < budget; i++) {
        uint8_t *s = g_k[k].seen[i];
        if (!s) continue;
        char path[1100]; snprintf(path, sizeof path, "%s/%s", fam_game_dir(), g_a[i].name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || (uint64_t)st.st_size != g_a[i].size) { if (fd >= 0) close(fd); continue; }
        for (uint32_t b = 0; b < g_a[i].nblocks && done < budget; b++) {
            if (!(s[b >> 3] & (1u << (b & 7)))) continue;
            if (g_job >= 0 && g_job != k) { close(fd); goto out; }      /* a newer mission wants its list */
            while (now_ns() - g_last_game_read < 30000000ull) usleep(10000);   /* the game is reading: wait */
            if ((blocks & 63) == 0 && mem_available_mb() < 160) { close(fd); goto out; }
            ssize_t r = pread(fd, buf, BLOCK, (off_t)b * BLOCK);
            if (r > 0) done += (uint64_t)r, blocks++;
        }
        close(fd);
    }
out:
    fprintf(stderr, "[readahead] %s: %llu blocks (%.0f MB) read ahead in %.1f s\n", key, (unsigned long long)blocks,
            done / 1048576.0, (now_ns() - t0) / 1e9);
}

static void *worker(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    for (;;) {
        pthread_mutex_lock(&g_mu);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 20;
        while (g_job < 0 && pthread_cond_timedwait(&g_cv, &g_mu, &ts) == 0) {}
        int k = g_job; g_job = -1;
        pthread_mutex_unlock(&g_mu);
        if (k >= 0) read_list(k);
        if (g_dirty) save_lists();                      /* keep the lists current */
    }
    return NULL;
}

static void at_exit(void) { if (g_dirty) save_lists(); }

/* a mission is loading: record into its list, read its blocks ahead, trim the heap */
static void mission_opened(const char *mission) {
    char key[48]; size_t n = 0;
    for (const char *c = mission; *c && *c != '\\' && *c != '/' && n < sizeof key - 1; c++) key[n++] = (char)tolower((unsigned char)*c);
    key[n] = 0;
    if (!n) return;
    pthread_mutex_lock(&g_mu);
    int k = list_index(key);
    if (k >= 0) { g_cur = k; g_job = k; pthread_cond_signal(&g_cv); }
    pthread_mutex_unlock(&g_mu);
    hle_heap_trim();
    fprintf(stderr, "[readahead] mission %s: its blocks queued, heap trimmed\n", key);
}

/* rw_data's dtaOpen (0x110018A0, rebased): (file_name, force_dta) */
void recomp_rw_data_110018A0_orig(void);
void fam_native_dta_open(void) {
    const char *name = (const char *)ADDR(MEM32(g_esp + 4));
    char nm[160]; snprintf(nm, sizeof nm, "%s", name ? name : "");
    recomp_rw_data_110018A0_orig();
    const char *m = strcasestr(nm, "missions");                /* ...missions\<name>\scene2.bin */
    if (!m || (m[8] != '\\' && m[8] != '/')) return;
    const char *base = strrchr(nm, '\\'), *b2 = strrchr(nm, '/');
    if (b2 > base) base = b2;
    if (!base || strcasecmp(base + 1, "scene2.bin")) return;
    if (g_on > 0) mission_opened(m + 9);
    else hle_heap_trim();
}

void fam_readahead_start(void) {
    g_on = fam_config_int("readahead", 1);
    if (!g_on) { g_on = 0; return; }
    load_lists();
    g_cur = list_index("*");
    g_job = g_cur;
    atexit(at_exit);
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
