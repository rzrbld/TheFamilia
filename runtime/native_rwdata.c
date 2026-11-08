/*
 * native_rwdata.c - native dtaRead for rw_data.dll (0x110025C0 here; the DLL
 * is rebased +0x01000000), the .dta archive reader every model, texture and
 * sound load goes through. Its time is the block decompression (an LZ coder,
 * inlined three times in the original) and the per-block decryption, byte
 * loops that cost up to a quarter of a second per streaming frame translated.
 *
 * Structure and semantics follow the original function (reference: the
 * mafia-re decompilation of rw_data.dll, used as a map only). Archive state is
 * the guest's own: the handle table v_file_h (0x1100EC54 begin / 0x1100EC58
 * end, 0x58-byte entries), the critical section C_m_p (0x1100EC6C), and the
 * Win32 calls go through the DLL's import table (EnterCriticalSection,
 * LeaveCriticalSection, SetFilePointer, GetLastError, ReadFile). The WAV/DPCM
 * blocks call the original WavDecompression (0x11004250). The read buffer for
 * a compressed block sits in the guest stack frame, as in the original.
 *
 * Checking (FAMILIA_NATIVE_CHECK=1, or FAMILIA_RWDATA_CHECK=N for N calls):
 * live differential test on the game's real reads. The original runs first;
 * its outputs are kept, the handle entry, the block cache, the WAV header, the
 * caller's buffer and the file position are put back, the native runs, and
 * every output is compared. A mismatch prints the first differing bytes and
 * switches the native off (the original's results stand).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

int fam_native_rwdata_on = 1;
static int g_live_checks;                         /* remaining calls to check live */

#define V_BEGIN   0x1100EC54u
#define V_END     0x1100EC58u
#define CS_MP     0x1100EC6Cu
#define IAT_LEAVE 0x1100B000u
#define IAT_ENTER 0x1100B004u
#define IAT_READ  0x1100B008u
#define IAT_GLE   0x1100B010u
#define IAT_SEEK  0x1100B014u
#define VA_WAVDEC 0x11004250u
#define ENT_SIZE  0x58u

/* S_FILEHANDLE fields (offsets checked against the asm) */
#define F_FILE    0x00
#define F_FLAGS   0x04
#define F_SIZE    0x10
#define F_DATAOFF 0x14
#define F_BSTART  0x18
#define F_FP      0x1C
#define F_BFP     0x20
#define F_BCOUNT  0x24
#define F_CURBLK  0x28
#define F_CBS     0x2C
#define F_PCBS    0x30
#define F_WAVHDR  0x34
#define F_BSIZE   0x38
#define F_KEY     0x40
#define F_INMEM   0x48
#define F_ENCR    0x49
#define F_CONTENT 0x50

#define BLOCK      0x8000u
#define SHIFT      15
#define HDR        4u
#define WAVHDR_SZ  0x2Cu

static recomp_func_t resolve(uint32_t va) {
    recomp_func_t f = recomp_lookup(va);
    if (!f) f = recomp_lookup_import(va);
    return f;
}
/* a stdcall through a function pointer (an import slot or a code address) */
static uint32_t call_std(uint32_t va, int n, const uint32_t *args) {
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    resolve(va)();
    return g_eax;
}
#define IMP(slot) MEM32(slot)

static inline uint32_t ent(uint32_t h) { return MEM32(V_BEGIN) + h * ENT_SIZE; }

static void lock(void)   { uint32_t a[1] = { CS_MP }; call_std(IMP(IAT_ENTER), 1, a); }
static void unlock(void) { uint32_t a[1] = { CS_MP }; call_std(IMP(IAT_LEAVE), 1, a); }

/* SetFilePointer(file, pos, NULL, FILE_BEGIN) failing: -1 and an error */
static int seek_fails(uint32_t file, uint32_t pos) {
    uint32_t a[4] = { file, pos, 0, 0 };
    if (call_std(IMP(IAT_SEEK), 4, a) != 0xFFFFFFFFu) return 0;
    return call_std(IMP(IAT_GLE), 0, NULL) != 0;
}
/* ReadFile(file, buf, n, &read, NULL) == TRUE && read == n */
static int read_ok(uint32_t file, uint32_t buf, uint32_t n, uint32_t pread) {
    uint32_t a[5] = { file, buf, n, pread, 0 };
    if (!call_std(IMP(IAT_READ), 5, a)) return 0;
    return MEM32(pread) == n;
}

/* the archive's LZ blocks: a 16-bit flag word per 16 items; a clear flag is a
 * literal byte, a set one a back reference (12-bit offset, 4-bit count + 3) or,
 * with offset 0, a run of one byte (16-bit count + 16) */
static uint32_t decompress(const uint8_t *src, uint8_t *dst, uint32_t len) {
    if (!src[0]) { memcpy(dst, src + 1, len - 1); return len - 1; }
    uint32_t out = 0, pos = 3;
    uint16_t flags = (uint16_t)((src[1] << 8) + src[2]);
    uint8_t bits = 16;
    while (pos < len) {
        if (!bits) { flags = (uint16_t)((src[pos] << 8) + src[pos + 1]); bits = 16; pos += 2; }
        if (!(flags & 0x8000)) {
            dst[out++] = src[pos++];
        } else {
            uint32_t offset = ((uint32_t)src[pos] << 4) + (src[pos + 1] >> 4);
            if (!offset) {
                uint16_t count = (uint16_t)((src[pos + 1] << 8) + src[pos + 2] + 15);
                uint8_t v = src[pos + 3];
                memset(dst + out, v, (size_t)count + 1);
                pos += 4; out += (uint32_t)count + 1;
            } else {
                uint32_t count = (uint32_t)(src[pos + 1] & 0xF) + 2 + 1;
                uint8_t *d = dst + out; const uint8_t *s = d - offset;
                for (uint32_t i = 0; i < count; i++) d[i] = s[i];      /* overlapping: byte by byte */
                pos += 2; out += count;
            }
        }
        flags = (uint16_t)(flags << 1);
        bits--;
    }
    return out;
}

/* DeCrypt (0x11004690): ~(~x ^ key) == x ^ key, 8 bytes at a time from the
 * end, the rest with the key's first bytes */
static void decrypt(uint8_t *buf, uint32_t len, uint64_t key) {
    uint32_t chunks = len >> 3, rest = len - chunks * 8;
    for (uint32_t i = chunks; i--; ) { uint64_t v; memcpy(&v, buf + 8 * i, 8); v ^= key; memcpy(buf + 8 * i, &v, 8); }
    uint8_t *q = buf + chunks * 8; const uint8_t *k = (const uint8_t *)&key;
    for (uint32_t i = rest; i--; ) q[i] ^= k[i];
}

/* one block in file_buffer (guest fb, cbs bytes) decoded to dst (guest);
 * returns its decoded size */
static uint32_t decode_block(uint32_t h, uint32_t fb, uint32_t cbs, uint32_t dst, uint32_t cur_block, int encrypted) {
    uint8_t *p = (uint8_t *)ADDR(fb);
    if (encrypted) { uint64_t key; memcpy(&key, (void *)ADDR(ent(h) + F_KEY), 8); decrypt(p, cbs, key); }
    if (p[0] == 1) return decompress(p, (uint8_t *)ADDR(dst), cbs);
    if (p[0] >= 8) {
        if (!cur_block && !encrypted) { uint64_t key; memcpy(&key, (void *)ADDR(ent(h) + F_KEY), 8); decrypt(p + 1, WAVHDR_SZ, key); }
        uint32_t a[4] = { fb, dst, cbs, MEM32(ent(h) + F_WAVHDR) };
        return call_std(VA_WAVDEC, 4, a);
    }
    memcpy((void *)ADDR(dst), p + 1, cbs - 1);
    return cbs - 1;
}

/* uint __stdcall dtaRead(int handle, void *buffer, uint len) */
static uint32_t dta_read(uint32_t handle, uint32_t buffer, uint32_t len) {
    /* the original's frame: 0x8058 bytes below its four saved registers */
    uint32_t esp_entry = g_esp;
    uint32_t frame = esp_entry - 0x10 - 0x8058;
    uint32_t p_cbs = frame + 0x14, p_read = frame + 0x20, fb = frame + 0x58;
    g_esp = frame;
    uint32_t r = 0xFFFFFFFFu;
    lock();
#define FAIL do { unlock(); goto out; } while (0)
#define FAIL_NOMEM do { MEM8(ent(handle) + F_INMEM) = 0; unlock(); goto out; } while (0)
    if (!buffer) FAIL;
    uint32_t b = MEM32(V_BEGIN);
    if (!b) FAIL;
    int32_t count = (int32_t)(MEM32(V_END) - b) / (int32_t)ENT_SIZE;
    if (handle >= (uint32_t)count) FAIL;
    uint32_t e = ent(handle);
    uint32_t file = MEM32(e + F_FILE);
    if (file == 0xFFFFFFFFu) FAIL;
    if (MEM32(e + F_FLAGS) & 2) FAIL;
    uint32_t result = 0;
    uint32_t size = MEM32(e + F_SIZE), data_offset = MEM32(e + F_DATAOFF);
    uint32_t bstart = MEM32(e + F_BSTART), fp = MEM32(e + F_FP), bfp = MEM32(e + F_BFP);
    uint32_t cur = MEM32(e + F_CURBLK), bcount = MEM32(e + F_BCOUNT);
    uint32_t cbs = MEM32(e + F_CBS), pcbs = MEM32(e + F_PCBS), bsize = MEM32(e + F_BSIZE);
    uint32_t content = MEM32(e + F_CONTENT);
    int encrypted = MEM8(e + F_ENCR);
    if (len > size - fp) len = size - fp;
#define READ_HDR(fail) do { if (!read_ok(file, p_cbs, 4, p_read)) fail; cbs = MEM32(p_cbs); } while (0)
    if (!(MEM32(e + F_FLAGS) & 1)) {
        uint32_t fp_block = fp >> SHIFT;
        if (fp_block > cur) {
            while (cur != fp_block) {
                bstart += cbs + HDR; cur++;
                if (seek_fails(file, bstart + data_offset)) FAIL;
                READ_HDR(FAIL);
                pcbs = cbs >> 16; cbs &= 0xFFFF;
            }
            bfp = fp - (cur << SHIFT);
            MEM8(ent(handle) + F_INMEM) = 0;
        } else if (fp_block < cur) {
            if (fp_block < cur >> 1) {
                if (seek_fails(file, data_offset)) FAIL;
                cur = 0; bstart = 0;
                READ_HDR(FAIL);
                pcbs = cbs >> 16; cbs &= 0xFFFF;
                while (cur != fp_block) {
                    bstart += cbs + HDR; cur++;
                    if (seek_fails(file, bstart + data_offset)) FAIL;
                    READ_HDR(FAIL);
                    pcbs = cbs >> 16; cbs &= 0xFFFF;
                }
            } else {
                while (cur != fp_block) {
                    bstart -= pcbs + HDR; cur--;
                    if (seek_fails(file, bstart + data_offset)) FAIL;
                    READ_HDR(FAIL);
                    pcbs = cbs >> 16; cbs &= 0xFFFF;
                }
            }
            bfp = fp - (cur << SHIFT);
            MEM8(ent(handle) + F_INMEM) = 0;
        }
        if (!MEM8(ent(handle) + F_INMEM)) {
            if (seek_fails(file, bstart + data_offset + HDR)) FAIL;
            if (!read_ok(file, fb, cbs, p_read)) FAIL;
            bsize = decode_block(handle, fb, cbs, content, cur, encrypted);
            MEM8(ent(handle) + F_INMEM) = 1;
        }
        uint32_t remaining = bsize - bfp;
        if (remaining >= len) {
            memcpy((void *)ADDR(buffer), (void *)ADDR(content + bfp), len);
            fp += len; bfp += len; result = len;
            if (bfp == bsize && cur + 1 < bcount) {
                bstart += cbs + HDR; cur++;
                if (seek_fails(file, bstart + data_offset)) FAIL;
                READ_HDR(FAIL);
                bfp = 0; pcbs = cbs >> 16; cbs &= 0xFFFF;
                MEM8(ent(handle) + F_INMEM) = 0;
            }
        } else {
            memcpy((void *)ADDR(buffer), (void *)ADDR(content + bfp), remaining);
            fp += remaining; bfp = 0; result = remaining; len -= remaining;
            bstart += cbs + HDR;
            if (seek_fails(file, bstart + data_offset)) FAIL;
            uint32_t whole = len / BLOCK, rest = len - whole * BLOCK;
            for (uint32_t i = 0; i < whole; i++) {
                READ_HDR(FAIL_NOMEM);
                cbs &= 0xFFFF;
                if (!read_ok(file, fb, cbs, p_read)) FAIL_NOMEM;
                cur++;
                bsize = decode_block(handle, fb, cbs, buffer + result, cur, encrypted);
                result += bsize; fp += bsize;
                bstart += cbs + HDR;
            }
            READ_HDR(FAIL_NOMEM);
            pcbs = cbs >> 16; cbs &= 0xFFFF; cur++;
            if (rest > 0) {
                if (!read_ok(file, fb, cbs, p_read)) FAIL_NOMEM;
                bsize = decode_block(handle, fb, cbs, content, cur, encrypted);
                memcpy((void *)ADDR(buffer + result), (void *)ADDR(content), rest);
                fp += rest; bfp = rest; result += rest;
            } else MEM8(ent(handle) + F_INMEM) = 0;
        }
    } else {
        if (!MEM8(ent(handle) + F_INMEM) || bstart > fp || bsize + bstart < fp) {
            bstart = fp / BLOCK * BLOCK;
            if (seek_fails(file, bstart)) FAIL;
            uint32_t to_read = size - bstart;
            bsize = to_read < BLOCK ? to_read : BLOCK;
            if (!read_ok(file, content, bsize, p_read)) FAIL;
            MEM8(ent(handle) + F_INMEM) = 1;
            bfp = fp - bstart;
        }
        uint32_t remaining = bsize - bfp;
        if (remaining >= len) {
            memcpy((void *)ADDR(buffer), (void *)ADDR(content + bfp), len);
            fp += len; bfp += len; result = len;
            if (bfp == bsize && fp < size) { bstart += bsize; MEM8(ent(handle) + F_INMEM) = 0; }
        } else {
            memcpy((void *)ADDR(buffer), (void *)ADDR(content + bfp), remaining);
            MEM8(ent(handle) + F_INMEM) = 0;
            fp += remaining; bfp = 0; result = remaining; len -= remaining;
            bstart += bsize;
            if (seek_fails(file, bstart)) FAIL;
            uint32_t to_read = size - bstart;
            bsize = to_read < BLOCK ? to_read : BLOCK;
            if (len >= bsize) {
                if (!read_ok(file, buffer + result, len, p_read)) FAIL;
                result += len; fp += len;
            } else if (len > 0) {
                if (!read_ok(file, content, bsize, p_read)) FAIL;
                memcpy((void *)ADDR(buffer + result), (void *)ADDR(content), len);
                result += len; fp += len; bfp = len;
                MEM8(ent(handle) + F_INMEM) = 1;
            }
        }
    }
    e = ent(handle);
    MEM32(e + F_BSTART) = bstart; MEM32(e + F_FP) = fp; MEM32(e + F_BFP) = bfp;
    MEM32(e + F_CURBLK) = cur; MEM32(e + F_CBS) = cbs; MEM32(e + F_PCBS) = pcbs;
    MEM32(e + F_BSIZE) = bsize;
    unlock();
    r = result;
out:
    g_esp = esp_entry;
    return r;
#undef FAIL
#undef FAIL_NOMEM
#undef READ_HDR
}

/* ---------------------------------------------------------- live check */
void recomp_rw_data_110025C0_orig(void);

static uint32_t file_pos(uint32_t file) {          /* SetFilePointer(file, 0, NULL, FILE_CURRENT) */
    uint32_t a[4] = { file, 0, 0, 1 };
    uint32_t esp0 = g_esp; g_esp -= 0x100;
    uint32_t r = call_std(IMP(IAT_SEEK), 4, a);
    g_esp = esp0;
    return r;
}
static void file_seek(uint32_t file, uint32_t pos) {
    uint32_t a[4] = { file, pos, 0, 0 };
    uint32_t esp0 = g_esp; g_esp -= 0x100;
    call_std(IMP(IAT_SEEK), 4, a);
    g_esp = esp0;
}

static void live_check(uint32_t handle, uint32_t buffer, uint32_t len) {
    uint32_t b = MEM32(V_BEGIN), count = b ? (uint32_t)((int32_t)(MEM32(V_END) - b) / (int32_t)ENT_SIZE) : 0;
    if (!buffer || handle >= count || len > (8u << 20)) { recomp_rw_data_110025C0_orig(); return; }
    uint32_t e = ent(handle), file = MEM32(e + F_FILE);
    if (file == 0xFFFFFFFFu) { recomp_rw_data_110025C0_orig(); return; }
    uint32_t content = MEM32(e + F_CONTENT), wav = MEM32(e + F_WAVHDR);
    uint32_t blen = len;
    uint8_t ent0[ENT_SIZE], wav0[WAVHDR_SZ], *cont0 = malloc(BLOCK), *buf0 = malloc(blen + 1);
    memcpy(ent0, (void *)ADDR(e), ENT_SIZE);
    if (content) memcpy(cont0, (void *)ADDR(content), BLOCK);
    if (wav) memcpy(wav0, (void *)ADDR(wav), WAVHDR_SZ);
    memcpy(buf0, (void *)ADDR(buffer), blen);
    uint32_t pos0 = file_pos(file);
    /* the original, from the caller's frame */
    uint32_t esp0 = g_esp;
    recomp_rw_data_110025C0_orig();
    uint32_t r1 = g_eax, esp1 = g_esp;
    uint8_t ent1[ENT_SIZE], wav1[WAVHDR_SZ], *cont1 = malloc(BLOCK), *buf1 = malloc(blen + 1);
    memcpy(ent1, (void *)ADDR(ent(handle)), ENT_SIZE);
    if (content) memcpy(cont1, (void *)ADDR(content), BLOCK);
    if (wav) memcpy(wav1, (void *)ADDR(wav), WAVHDR_SZ);
    memcpy(buf1, (void *)ADDR(buffer), blen);
    uint32_t pos1 = file_pos(file);
    /* back to the start, then the native */
    memcpy((void *)ADDR(ent(handle)), ent0, ENT_SIZE);
    if (content) memcpy((void *)ADDR(content), cont0, BLOCK);
    if (wav) memcpy((void *)ADDR(wav), wav0, WAVHDR_SZ);
    memcpy((void *)ADDR(buffer), buf0, blen);
    file_seek(file, pos0);
    g_esp = esp0;
    uint32_t r2 = dta_read(handle, buffer, len);
    uint32_t pos2 = file_pos(file);
    const char *what = NULL; uint32_t at = 0; const uint8_t *x = NULL, *y = NULL;
    if (r1 != r2) what = "result";
    else if (pos1 != pos2) what = "file position";
    else if (memcmp(ent1, (void *)ADDR(ent(handle)), ENT_SIZE)) { what = "handle entry"; x = ent1; y = (const uint8_t *)ADDR(ent(handle)); at = ENT_SIZE; }
    else if (memcmp(buf1, (void *)ADDR(buffer), blen)) { what = "buffer"; x = buf1; y = (const uint8_t *)ADDR(buffer); at = blen; }
    else if (content && memcmp(cont1, (void *)ADDR(content), BLOCK)) { what = "block cache"; x = cont1; y = (const uint8_t *)ADDR(content); at = BLOCK; }
    else if (wav && memcmp(wav1, (void *)ADDR(wav), WAVHDR_SZ)) { what = "wav header"; x = wav1; y = (const uint8_t *)ADDR(wav); at = WAVHDR_SZ; }
    if (what) {
        fprintf(stderr, "[native] dtaRead MISMATCH (%s): handle %u len %u result %08X/%08X pos %08X/%08X\n",
                what, handle, len, r1, r2, pos1, pos2);
        for (uint32_t i = 0, n = 0; x && i < at && n < 3; i++)
            if (x[i] != y[i]) { fprintf(stderr, "[native]   byte %u: %02X / %02X\n", i, x[i], y[i]); n++; }
        fam_native_rwdata_on = 0; g_live_checks = 0;
        /* the original's results stand */
        memcpy((void *)ADDR(ent(handle)), ent1, ENT_SIZE);
        if (content) memcpy((void *)ADDR(content), cont1, BLOCK);
        if (wav) memcpy((void *)ADDR(wav), wav1, WAVHDR_SZ);
        memcpy((void *)ADDR(buffer), buf1, blen);
        file_seek(file, pos1);
        fprintf(stderr, "[native] dtaRead native disabled\n");
    } else if (--g_live_checks == 0) {
        fprintf(stderr, "[native] dtaRead: live check passed (all checked reads matched the translated original)\n");
    }
    g_eax = r1;
    g_esp = esp1;
    free(cont0); free(buf0); free(cont1); free(buf1);
}

void fam_native_dta_read(void) {
    uint32_t handle = MEM32(g_esp + 4), buffer = MEM32(g_esp + 8), len = MEM32(g_esp + 12);
    if (!fam_native_rwdata_on) { recomp_rw_data_110025C0_orig(); return; }
    static int armed;
    if (!armed) { armed = 1; const char *e = getenv("FAMILIA_RWDATA_CHECK"); if (e && !g_live_checks) g_live_checks = atoi(e); }
    if (g_live_checks > 0) { live_check(handle, buffer, len); return; }
    g_eax = dta_read(handle, buffer, len);
    g_esp += 4 + 12;
}

/* Called from fam_native_init with FAMILIA_NATIVE_CHECK=1: arms the live
 * check for the first reads (FAMILIA_RWDATA_CHECK=N overrides the count). */
int fam_native_rwdata_check(void) {
    const char *e = getenv("FAMILIA_RWDATA_CHECK");
    g_live_checks = e ? atoi(e) : 2000;
    if (g_live_checks > 0)
        fprintf(stderr, "[native] dtaRead: checking the first %d archive reads live against the translated original\n", g_live_checks);
    return 0;
}
