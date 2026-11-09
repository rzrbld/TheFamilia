/*
 * readahead.c - learned read-ahead for the game's archives.
 *
 * The game reads its .dta archives in small pieces (about 20 KB) at scattered
 * offsets, and on the first visit to a part of the city each piece costs an
 * SD card access (about 3 ms; the archives are hundreds of MB on FAT32), all
 * on the game thread. This module remembers which 64 KB blocks of each
 * archive the game has read (conf/readahead.txt), and on the next start a
 * low-priority thread reads those blocks into the kernel's page cache while
 * the menus and the loading screen run, so the game finds them in RAM.
 *
 * The thread steps aside whenever the game itself read within the last
 * 30 ms, stops when free memory runs low (the page cache is the kernel's
 * anyway: it drops what it needs), and reads at most FAMILIA_READAHEAD_MB
 * (default 256) per start. FAMILIA_READAHEAD=0 turns it off.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include "familia.h"

int fam_config_int(const char *key, int def);
const char *fam_game_dir(void);

#define BLOCK (64u * 1024u)
#define MAXA 32
typedef struct {
    char name[64];
    uint64_t size;
    uint32_t nblocks;
    uint8_t *seen;            /* a bit per block: read by the game, this session or before */
    dev_t dev; ino_t ino;
} archive;
static archive g_a[MAXA];
static int g_na;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t g_last_game_read;   /* ns, CLOCK_MONOTONIC */
static volatile int g_dirty;
static int g_on = -1;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static archive *archive_named(const char *name, uint64_t size) {
    for (int i = 0; i < g_na; i++) if (!strcasecmp(g_a[i].name, name)) return g_a[i].size == size ? &g_a[i] : NULL;
    if (g_na == MAXA || strlen(name) >= sizeof g_a[0].name) return NULL;
    archive *a = &g_a[g_na++];
    snprintf(a->name, sizeof a->name, "%s", name);
    a->size = size; a->nblocks = (uint32_t)((size + BLOCK - 1) / BLOCK);
    a->seen = calloc((a->nblocks + 7) / 8, 1);
    return a;
}

/* fd -> archive, checked by device and inode (a closed fd's number is reused) */
static struct { int fd; dev_t dev; ino_t ino; archive *a; } g_fd[64];
static archive *archive_of_fd(int fd) {
    struct stat st;
    if (fstat(fd, &st)) return NULL;
    unsigned h = (unsigned)fd & 63;
    if (g_fd[h].fd == fd && g_fd[h].dev == st.st_dev && g_fd[h].ino == st.st_ino) return g_fd[h].a;
    archive *a = NULL;
    char lp[64], fn[1024]; snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd);
    ssize_t k = readlink(lp, fn, sizeof fn - 1);
    if (k > 4) {
        fn[k] = 0;
        const char *b = strrchr(fn, '/'); b = b ? b + 1 : fn;
        size_t l = strlen(b);
        if (l > 4 && !strcasecmp(b + l - 4, ".dta")) a = archive_named(b, (uint64_t)st.st_size);
        if (a) a->dev = st.st_dev, a->ino = st.st_ino;
    }
    g_fd[h].fd = fd; g_fd[h].dev = st.st_dev; g_fd[h].ino = st.st_ino; g_fd[h].a = a;
    return a;
}

/* every ReadFile of the game (kernel32_ext.c) */
void hle_read_hook(int fd, uint64_t off, uint32_t len) {
    if (g_on <= 0) return;
    g_last_game_read = now_ns();
    pthread_mutex_lock(&g_mu);
    archive *a = archive_of_fd(fd);
    if (a) {
        for (uint64_t b = off / BLOCK; b <= (off + len - 1) / BLOCK && b < a->nblocks; b++)
            if (!(a->seen[b >> 3] & (1u << (b & 7)))) { a->seen[b >> 3] |= (uint8_t)(1u << (b & 7)); g_dirty = 1; }
    }
    pthread_mutex_unlock(&g_mu);
}

/* conf/readahead.txt: one line per archive, "name size hexbitmap" */
static void list_path(char *p, size_t n) { snprintf(p, n, "%s/readahead.txt", fam_user_dir()); }
static void load_list(void) {
    char p[1024]; list_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char name[64]; unsigned long long size;
    while (fscanf(f, "%63s %llu ", name, &size) == 2) {
        archive *a = archive_named(name, size);
        for (uint32_t i = 0; i < (a ? (a->nblocks + 7) / 8 : 0); i++) { unsigned v; if (fscanf(f, "%2x", &v) != 1) break; a->seen[i] = (uint8_t)v; }
        int c; while ((c = fgetc(f)) != EOF && c != '\n') {}
    }
    fclose(f);
}
static void save_list(void) {
    char p[1024], t[1100]; list_path(p, sizeof p); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w");
    if (!f) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_na; i++) {
        fprintf(f, "%s %llu ", g_a[i].name, (unsigned long long)g_a[i].size);
        for (uint32_t k = 0; k < (g_a[i].nblocks + 7) / 8; k++) fprintf(f, "%02x", g_a[i].seen[k]);
        fputc('\n', f);
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

static void *worker(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    uint64_t budget = (uint64_t)fam_config_int("readahead_mb", 256) << 20, done = 0, blocks = 0;
    uint64_t t0 = now_ns();
    static uint8_t buf[BLOCK];
    /* the blocks known from earlier sessions, archive by archive, in file order */
    int na; pthread_mutex_lock(&g_mu); na = g_na; pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < na && done < budget; i++) {
        char path[1100]; snprintf(path, sizeof path, "%s/%s", fam_game_dir(), g_a[i].name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || (uint64_t)st.st_size != g_a[i].size) { if (fd >= 0) close(fd); continue; }
        for (uint32_t b = 0; b < g_a[i].nblocks && done < budget; b++) {
            if (!(g_a[i].seen[b >> 3] & (1u << (b & 7)))) continue;
            while (now_ns() - g_last_game_read < 30000000ull) usleep(10000);   /* the game is reading: wait */
            if ((blocks & 63) == 0 && mem_available_mb() < 160) { b = g_a[i].nblocks; i = na; break; }
            ssize_t r = pread(fd, buf, BLOCK, (off_t)b * BLOCK);
            if (r > 0) done += (uint64_t)r, blocks++;
        }
        close(fd);
    }
    fprintf(stderr, "[readahead] %llu blocks (%.0f MB) read ahead in %.1f s\n", (unsigned long long)blocks,
            done / 1048576.0, (now_ns() - t0) / 1e9);
    for (;;) {                                         /* then keep the list current */
        sleep(20);
        if (g_dirty) save_list();
    }
    return NULL;
}

static void at_exit(void) { if (g_dirty) save_list(); }

void fam_readahead_start(void) {
    g_on = fam_config_int("readahead", 1);
    if (!g_on) return;
    load_list();
    atexit(at_exit);
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
