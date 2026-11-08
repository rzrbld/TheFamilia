/*
 * familia-import - builds the_familia/data from the user's own Mafia files.
 *
 *   familia-import --source DIR --dest DIR --7z PATH_TO_7ZZS
 *
 * source/ may contain, in any letter case:
 *   - the three game discs: .cue + .bin (MODE1/2352 or 2048) or .iso
 *   - mafia_patch_1.2_eng.exe (the official 1.2 patch, a RAR self-extractor)
 *   - the unprotected 1.2 Game.exe, loose or inside a .rar/.zip/.7z
 * Nothing in source/ is modified. The patch and archives are unpacked with
 * PortMaster's 7zzs into dest/.import-tmp, which is removed afterwards.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* SHA-256 of the files the translated code was generated from. */
#define SHA_GAME_UNPROTECTED "f49809e22e0a927af670c19cc81ccd9bcab443abfc0e7737eceb8a7996dff11c"
#define SHA_LS3DF_12         "e211c85009440d6c032f2074d79184f074a9a046574648745c2594b4cef178a6"
#define GAME12_TIMESTAMP     1056983719u

static const char *g_src, *g_dst, *g_7z;
static char g_tmp[4096];

static void die(const char *fmt, const char *a) { fprintf(stderr, "[import] ERROR: "); fprintf(stderr, fmt, a); fprintf(stderr, "\n"); exit(1); }
static void info(const char *fmt, const char *a) { printf("[import] "); printf(fmt, a); printf("\n"); fflush(stdout); }

/* ------------------------------------------------------------- sha256 */
typedef struct { uint32_t h[8]; uint64_t n; uint8_t b[64]; size_t bl; } sha256;
static const uint32_t K[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
 0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
 0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
 0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha256 *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(sha256 *s) {
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv); s->n = 0; s->bl = 0;
}
static void sha_update(sha256 *s, const uint8_t *p, size_t n) {
    s->n += n;
    while (n) {
        size_t k = 64 - s->bl < n ? 64 - s->bl : n;
        memcpy(s->b + s->bl, p, k); s->bl += k; p += k; n -= k;
        if (s->bl == 64) { sha_block(s, s->b); s->bl = 0; }
    }
}
static void sha_hex(sha256 *s, char out[65]) {
    uint64_t bits = s->n * 8; uint8_t pad = 0x80, z = 0;
    sha_update(s, &pad, 1);
    while (s->bl != 56) sha_update(s, &z, 1);
    uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", s->h[i]);
}
static int file_sha(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    sha256 s; sha_init(&s); static uint8_t buf[1 << 16]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) sha_update(&s, buf, r);
    fclose(f); sha_hex(&s, out); return 0;
}

/* ---------------------------------------------------------- utilities */
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }
static void mkdirs(const char *path) {
    char p[4096]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"); if (!in) return -1;
    char tmp[4200]; snprintf(tmp, sizeof tmp, "%s.part", to);
    FILE *out = fopen(tmp, "wb"); if (!out) { fclose(in); return -1; }
    static uint8_t buf[1 << 20]; size_t r; int ok = 1;
    while ((r = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, r, out) != r) { ok = 0; break; }
    fclose(in); if (fclose(out) != 0) ok = 0;
    if (!ok || rename(tmp, to) != 0) { unlink(tmp); return -1; }
    return 0;
}
static int run(char *const argv[]) {
    pid_t p = fork();
    if (p == 0) { execv(argv[0], argv); _exit(127); }
    int st; if (p < 0 || waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
/* find a file below dir whose basename matches name (case-insensitive) */
static int find_file(const char *dir, const char *name, char *out, size_t n, int depth) {
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) found = find_file(p, name, out, n, depth + 1); }
        else if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s", p); found = 1; }
    }
    closedir(d); return found;
}

/* ------------------------------------------------------- disc images */
typedef struct { FILE *f; int raw; } disc;
static int disc_read(disc *d, uint32_t lba, uint8_t *out, uint32_t nsect) {
    for (uint32_t i = 0; i < nsect; i++) {
        long off = (long)(lba + i) * (d->raw ? 2352 : 2048) + (d->raw ? 16 : 0);
        if (fseek(d->f, off, SEEK_SET) || fread(out + (size_t)i * 2048, 1, 2048, d->f) != 2048) return -1;
    }
    return 0;
}
static int g_joliet;
static void name_of(const uint8_t *rec, char *out, size_t n) {
    int nl = rec[32]; const uint8_t *p = rec + 33; size_t k = 0;
    if (g_joliet) for (int i = 0; i + 1 < nl && k + 1 < n; i += 2) { uint16_t c = (uint16_t)(p[i] << 8 | p[i + 1]); out[k++] = c < 128 ? (char)c : '_'; }
    else for (int i = 0; i < nl && k + 1 < n; i++) out[k++] = (char)p[i];
    out[k] = 0;
    char *semi = strchr(out, ';'); if (semi) *semi = 0;
}
static int g_files_out;
static int extract_tree(disc *d, uint32_t lba, uint32_t size, const char *rel, int in_game) {
    uint32_t ns = (size + 2047) / 2048;
    uint8_t *buf = malloc((size_t)ns * 2048);
    if (!buf || disc_read(d, lba, buf, ns)) { free(buf); return -1; }
    for (uint32_t pos = 0; pos < size; ) {
        uint8_t len = buf[pos];
        if (!len) { pos = (pos / 2048 + 1) * 2048; continue; }
        const uint8_t *r = buf + pos; pos += len;
        if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) continue;
        char name[256]; name_of(r, name, sizeof name);
        uint32_t elba = (uint32_t)r[2] | (uint32_t)r[3] << 8 | (uint32_t)r[4] << 16 | (uint32_t)r[5] << 24;
        uint32_t esz  = (uint32_t)r[10] | (uint32_t)r[11] << 8 | (uint32_t)r[12] << 16 | (uint32_t)r[13] << 24;
        char sub[4096]; snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", name);
        if (r[25] & 2) {
            int game = in_game || (!*rel && !strcasecmp(name, "MafiaGame"));
            if (extract_tree(d, elba, esz, game && !in_game ? "" : sub, game)) { free(buf); return -1; }
            continue;
        }
        if (!in_game) continue;
        if (ends_with(name, ".url") || !strcasecmp(name, "Setup.exe") || !strcasecmp(name, "MafiaSetup.exe")) continue;
        char dst[4096]; snprintf(dst, sizeof dst, "%s/%s", g_dst, sub);
        struct stat st;
        if (!stat(dst, &st) && (uint32_t)st.st_size == esz) continue;   /* resume */
        char *slash = strrchr(dst, '/'); *slash = 0; mkdirs(dst); *slash = '/';
        char tmp[4200]; snprintf(tmp, sizeof tmp, "%s.part", dst);
        FILE *o = fopen(tmp, "wb"); if (!o) { free(buf); return -1; }
        printf("[import]   %s (%u MB)\n", sub, esz >> 20); fflush(stdout);
        static uint8_t chunk[2048 * 256];
        for (uint32_t left = esz, l = elba; left; ) {
            uint32_t n = left > sizeof chunk ? (uint32_t)sizeof chunk : left, s = (n + 2047) / 2048;
            if (disc_read(d, l, chunk, s) || fwrite(chunk, 1, n, o) != n) { fclose(o); unlink(tmp); free(buf); return -1; }
            left -= n; l += s;
        }
        if (fclose(o) || rename(tmp, dst)) { free(buf); return -1; }
        g_files_out++;
    }
    free(buf);
    return 0;
}
static int import_disc(const char *image) {
    disc d = { fopen(image, "rb"), 0 };
    if (!d.f) return -1;
    uint8_t sync[12] = {0}, ref[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (fread(sync, 1, 12, d.f) == 12 && !memcmp(sync, ref, 12)) d.raw = 1;
    uint8_t vd[2048], root[34]; g_joliet = 0;
    if (disc_read(&d, 16, vd, 1) || memcmp(vd + 1, "CD001", 5)) { fclose(d.f); return -1; }
    memcpy(root, vd + 156, 34);
    for (uint32_t l = 17; l < 32; l++) {           /* prefer Joliet: long names */
        if (disc_read(&d, l, vd, 1) || vd[0] == 255) break;
        if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/') { memcpy(root, vd + 156, 34); g_joliet = 1; break; }
    }
    uint32_t lba = (uint32_t)root[2] | (uint32_t)root[3] << 8 | (uint32_t)root[4] << 16 | (uint32_t)root[5] << 24;
    uint32_t sz = (uint32_t)root[10] | (uint32_t)root[11] << 8 | (uint32_t)root[12] << 16 | (uint32_t)root[13] << 24;
    int r = extract_tree(&d, lba, sz, "", 0);
    fclose(d.f);
    return r;
}

/* ------------------------------------------------------------ steps */
static int pe_timestamp_unprotected(const char *path, uint32_t *ts) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    uint8_t h[4096]; size_t n = fread(h, 1, sizeof h, f); fclose(f);
    if (n < 0x200 || h[0] != 'M' || h[1] != 'Z') return -1;
    uint32_t pe = (uint32_t)h[0x3C] | (uint32_t)h[0x3D] << 8;
    if (pe + 0x200 > n || memcmp(h + pe, "PE\0\0", 4)) return -1;
    memcpy(ts, h + pe + 8, 4);
    uint16_t nsec = (uint16_t)(h[pe + 6] | h[pe + 7] << 8), opt = (uint16_t)(h[pe + 20] | h[pe + 21] << 8);
    const uint8_t *sec = h + pe + 24 + opt;
    for (int i = 0; i < nsec && sec + 40 <= h + n; i++, sec += 40)
        if (!memcmp(sec, "stxt", 4)) return 1;       /* SafeDisc wrapper sections */
    return 0;
}
static void unpack(const char *archive, const char *into) {
    mkdirs(into);
    char o[4200]; snprintf(o, sizeof o, "-o%s", into);
    char *argv[] = { (char *)g_7z, "x", "-y", "-bso0", "-bsp0", o, (char *)archive, NULL };
    if (run(argv) != 0) die("could not unpack %s with 7zzs", archive);
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--source")) g_src = argv[i + 1];
        else if (!strcmp(argv[i], "--dest")) g_dst = argv[i + 1];
        else if (!strcmp(argv[i], "--7z")) g_7z = argv[i + 1];
    }
    if (!g_src || !g_dst || !g_7z) { fprintf(stderr, "usage: familia-import --source DIR --dest DIR --7z 7ZZS\n"); return 2; }
    mkdirs(g_dst);
    snprintf(g_tmp, sizeof g_tmp, "%s/.import-tmp", g_dst);

    /* 1. discs */
    DIR *d = opendir(g_src); if (!d) die("no source folder %s", g_src);
    struct dirent *e; int discs = 0;
    char patch[4096] = "", exe_cand[16][4096]; int nexe = 0;
    while ((e = readdir(d))) {
        char p[4096]; snprintf(p, sizeof p, "%s/%s", g_src, e->d_name);
        if (ends_with(e->d_name, ".bin") || ends_with(e->d_name, ".iso") || ends_with(e->d_name, ".img")) {
            info("reading disc image %s", e->d_name);
            if (import_disc(p)) die("%s is not a readable ISO9660 disc image", e->d_name);
            discs++;
        } else if (ends_with(e->d_name, ".exe") && strcasestr(e->d_name, "patch")) snprintf(patch, sizeof patch, "%s", p);
        else if ((ends_with(e->d_name, ".exe") || ends_with(e->d_name, ".rar") || ends_with(e->d_name, ".zip") || ends_with(e->d_name, ".7z")) && nexe < 16)
            snprintf(exe_cand[nexe++], 4096, "%s", p);
    }
    closedir(d);
    if (!discs) die("no disc images (.bin/.iso) found in %s", g_src);
    char probe[4096];
    if (!find_file(g_dst, "a0.dta", probe, sizeof probe, 0) || !find_file(g_dst, "a1.dta", probe, sizeof probe, 0) ||
        !find_file(g_dst, "a4.dta", probe, sizeof probe, 0))
        die("the disc images do not contain all Mafia data (%s): all three CDs are needed", "a0/a1/a4.dta");

    /* 2. the 1.2 patch */
    if (!*patch) die("mafia_patch_1.2_eng.exe not found in %s", g_src);
    info("unpacking the 1.2 patch %s", patch);
    char pdir[4200]; snprintf(pdir, sizeof pdir, "%s/patch", g_tmp);
    unpack(patch, pdir);
    static const char *const from_patch[] = { "a8.dta", "binkw32.dll", "LS3DF.dll", "Logo3.bik", "bilboard0.dta", "isdata.dta" };
    for (size_t i = 0; i < sizeof from_patch / sizeof *from_patch; i++) {
        char f[4096], to[4200];
        if (!find_file(pdir, from_patch[i], f, sizeof f, 0)) die("the patch does not contain %s: is it the English 1.2 patch?", from_patch[i]);
        snprintf(to, sizeof to, "%s/%s", g_dst, from_patch[i]);
        if (copy_file(f, to)) die("could not write %s", to);
    }
    char sha[65], ls3df[4200]; snprintf(ls3df, sizeof ls3df, "%s/LS3DF.dll", g_dst);
    if (file_sha(ls3df, sha) || strcmp(sha, SHA_LS3DF_12)) die("LS3DF.dll from the patch is not the expected 1.2 build (sha256 %s)", sha);

    /* 3. the unprotected Game.exe */
    char game[4096] = "";
    for (int i = 0; i < nexe && !*game; i++) {
        char cand[4096] = "";
        if (ends_with(exe_cand[i], ".exe")) snprintf(cand, sizeof cand, "%s", exe_cand[i]);
        else {
            char adir[4200]; snprintf(adir, sizeof adir, "%s/exe%d", g_tmp, i);
            unpack(exe_cand[i], adir);
            if (!find_file(adir, "Game.exe", cand, sizeof cand, 0)) continue;
        }
        uint32_t ts = 0; int prot = pe_timestamp_unprotected(cand, &ts);
        if (file_sha(cand, sha)) continue;
        if (!strcmp(sha, SHA_GAME_UNPROTECTED)) { snprintf(game, sizeof game, "%s", cand); break; }
        if (prot == 0 && ts == GAME12_TIMESTAMP) {
            printf("[import] WARNING: %s is an unprotected 1.2 Game.exe with an unknown checksum (%s); it may differ from the tested one\n", cand, sha);
            snprintf(game, sizeof game, "%s", cand);
        } else if (prot == 1) printf("[import] %s is still SafeDisc protected; skipped\n", cand);
    }
    if (!*game) die("no unprotected Mafia 1.2 Game.exe found in %s (see README)", g_src);
    char gdst[4200]; snprintf(gdst, sizeof gdst, "%s/Game.exe", g_dst);
    if (copy_file(game, gdst)) die("could not write %s", gdst);

    char *rm[] = { "/bin/rm", "-rf", g_tmp, NULL }; run(rm);
    printf("[import] done: %d disc image(s), %d files written\n", discs, g_files_out);
    return 0;
}
