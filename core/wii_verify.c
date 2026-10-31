/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent verifier for finished WBFS images. It re-derives every hash from the
 * decrypted data and shares no code with the builder except the crypto primitives. */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "wii_build.h"
#include <stdlib.h>
#include <string.h>

static int signed_ok(const uint8_t *b, size_t size)
{
    for (int i = 4; i < 0x104; i++) if (b[i]) return 0;
    uint8_t h[20];
    rr_sha1(b + 0x140, size - 0x140, h);
    return h[0] == 0;
}

typedef struct { uint64_t off; uint32_t size; } fext;
static int fext_cmp(const void *a, const void *b)
{
    const fext *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

int rr_verify_wbfs(rr_ctx *c, const char *path, const char *expect_id, const uint8_t *expect_plain, uint64_t *files_out)
{
    wbfs_img w; wii_part p; wii_fst f;
    int r = wbfs_img_open(c, &w, path);
    if (r) return r;
    memset(&p, 0, sizeof p); memset(&f, 0, sizeof f);
    uint8_t *raw = NULL, *dec = NULL, *hdr = NULL; fext *fe = NULL;

    /* --- WBFS container ------------------------------------------------- */
    {
        uint8_t h[16];
        if (rr_pread_full(w.fd, h, 16, 0)) { r = rr_fail(c, RR_E_IO, "cannot read WBFS header"); goto out; }
        if ((uint64_t)rr_be32(h + 4) * w.hd_sec_sz != w.file_size) { r = rr_fail(c, RR_E_CORRUPT, "WBFS size field does not match the file size"); goto out; }
        uint32_t nblocks = (uint32_t)(w.file_size >> w.wbfs_sec_sz_s);
        uint8_t *seen = rr_xmalloc(nblocks + 1); memset(seen, 0, nblocks + 1);
        int bad = 0;
        for (uint32_t i = 0; i < w.n_wlba; i++) {
            uint32_t b = w.wlba[i];
            if (!b) continue;
            if (b >= nblocks || seen[b]) bad = 1; else seen[b] = 1;
        }
        free(seen);
        if (bad) { r = rr_fail(c, RR_E_CORRUPT, "WBFS block table is inconsistent"); goto out; }
        uint8_t disc0[0x100];
        if (rr_pread_full(w.fd, disc0, 0x100, w.hd_sec_sz) || wbfs_img_read(c, &w, 0, hdr = rr_xmalloc(0x100), 0x100) || memcmp(disc0, hdr, 0x100)) { r = rr_fail(c, RR_E_CORRUPT, "WBFS disc header copy does not match the disc"); goto out; }
        free(hdr); hdr = NULL;
    }
    /* --- disc header ------------------------------------------------------ */
    uint8_t dh[0x60];
    if ((r = wbfs_img_read(c, &w, 0, dh, sizeof dh))) goto out;
    if (rr_be32(dh + 0x18) != 0x5D1C9EA3u) { r = rr_fail(c, RR_E_CORRUPT, "not a Wii disc (magic missing)"); goto out; }
    if (expect_id && memcmp(dh, expect_id, 6)) { r = rr_fail(c, RR_E_CORRUPT, "disc ID is %.6s, expected %s", (char *)dh, expect_id); goto out; }
    /* --- partition --------------------------------------------------------- */
    if ((r = wii_find_data_partition(c, &w, &p))) goto out;
    if (!signed_ok(p.ticket, WII_TICKET_SIZE)) { r = rr_fail(c, RR_E_CORRUPT, "ticket is not validly fake-signed"); goto out; }
    if (!signed_ok(p.tmd, p.tmd_size)) { r = rr_fail(c, RR_E_CORRUPT, "TMD is not validly fake-signed"); goto out; }
    if (memcmp(p.ticket + 0x1dc, p.tmd + 0x18c, 8)) { r = rr_fail(c, RR_E_CORRUPT, "ticket and TMD title IDs differ"); goto out; }
    p.verify_h0 = 0;
    uint32_t blocks = (uint32_t)(p.data_size / WII_SECTOR), groups = (blocks - 1) / 64 + 1;
    raw = rr_xmalloc(64ull * WII_SECTOR);
    dec = rr_xmalloc(64ull * WII_DATA_BYTES);
    hdr = rr_xmalloc(64ull * WII_HASH_BYTES);
    uint8_t *exp = rr_xmalloc(64ull * WII_HASH_BYTES);
    rr_sha1_ctx plain; rr_sha1_init(&plain);
    uint8_t *h3 = rr_xmalloc(WII_H3_SIZE); memset(h3, 0, WII_H3_SIZE);
    for (uint32_t g = 0; g < groups && !r; g++) {
        if (rr_cancelled(c)) { r = rr_fail(c, RR_E_CANCEL, "cancelled"); break; }
        uint32_t nc = blocks - g * 64; if (nc > 64) nc = 64;
        if ((r = wbfs_img_read(c, &w, p.part_off + p.data_off + (uint64_t)g * 64 * WII_SECTOR, raw, (size_t)nc * WII_SECTOR))) break;
        memset(dec, 0, 64ull * WII_DATA_BYTES); memset(hdr, 0, 64ull * WII_HASH_BYTES);
        for (uint32_t s = 0; s < nc; s++) {
            const uint8_t *rs = raw + (size_t)s * WII_SECTOR;
            uint8_t iv0[16] = {0};
            size_t z = 0; while (z < WII_SECTOR && !rs[z]) z++;
            if (z == WII_SECTOR) { r = rr_fail(c, RR_E_CORRUPT, "cluster %u is missing from the image", g * 64 + s); break; }
            rr_aes128_cbc_decrypt(&p.aes, iv0, rs, hdr + (size_t)s * WII_HASH_BYTES, WII_HASH_BYTES);
            rr_aes128_cbc_decrypt(&p.aes, rs + 0x3d0, rs + WII_HASH_BYTES, dec + (size_t)s * WII_DATA_BYTES, WII_DATA_BYTES);
        }
        if (r) break;
        rr_sha1_update(&plain, dec, (size_t)nc * WII_DATA_BYTES);
        /* recompute the whole tree for a full 64-cluster group (absent clusters count as zero data) */
        memset(exp, 0, 64ull * WII_HASH_BYTES);
        for (int s = 0; s < 64; s++)
            for (int j = 0; j < 31; j++) rr_sha1(dec + (size_t)s * WII_DATA_BYTES + (size_t)j * 0x400, 0x400, exp + (size_t)s * WII_HASH_BYTES + j * 20);
        uint8_t h1t[8][160], h2t[160];
        for (int sg = 0; sg < 8; sg++) {
            for (int j = 0; j < 8; j++) rr_sha1(exp + (size_t)(sg * 8 + j) * WII_HASH_BYTES, 620, h1t[sg] + j * 20);
            rr_sha1(h1t[sg], 160, h2t + sg * 20);
        }
        for (int s = 0; s < 64; s++) {
            memcpy(exp + (size_t)s * WII_HASH_BYTES + 0x280, h1t[s / 8], 160);
            memcpy(exp + (size_t)s * WII_HASH_BYTES + 0x340, h2t, 160);
        }
        for (uint32_t s = 0; s < nc; s++)
            if (memcmp(exp + (size_t)s * WII_HASH_BYTES, hdr + (size_t)s * WII_HASH_BYTES, WII_HASH_BYTES)) { r = rr_fail(c, RR_E_CORRUPT, "hash mismatch in cluster %u of the image", g * 64 + s); break; }
        if (r) break;
        rr_sha1(h2t, 160, h3 + (size_t)g * 20);
        rr_progress(c, "Verifying disc image", (uint64_t)g * 64 + nc, blocks);
    }
    if (!r) {
        uint8_t ph[20]; rr_sha1_final(&plain, ph);
        if (memcmp(h3, p.h3, WII_H3_SIZE)) r = rr_fail(c, RR_E_CORRUPT, "H3 table does not match the data");
        else if (expect_plain && memcmp(ph, expect_plain, 20)) r = rr_fail(c, RR_E_CORRUPT, "disc content differs from what was built");
    }
    free(exp); free(h3);
    if (r) goto out;
    /* --- file system ----------------------------------------------------- */
    p.verify_h0 = 1;
    if ((r = wii_read_fst(c, &p, &f))) goto out;
    {
        uint64_t cap = wii_part_data_capacity(&p), n = 0;
        fe = rr_xmalloc((f.n ? f.n : 1) * sizeof *fe);
        size_t m = 0;
        for (size_t i = 0; i < f.n; i++) if (!f.e[i].is_dir && f.e[i].size) { fe[m].off = f.e[i].offset; fe[m].size = f.e[i].size; m++; }
        qsort(fe, m, sizeof *fe, fext_cmp);
        for (size_t i = 0; i < m; i++) {
            if (fe[i].off < 0x2440) { r = rr_fail(c, RR_E_CORRUPT, "file overlaps the disc header area"); break; }
            if (fe[i].off + fe[i].size > cap) { r = rr_fail(c, RR_E_CORRUPT, "file extends beyond the partition"); break; }
            if (i && fe[i].off < fe[i - 1].off + fe[i - 1].size && !(fe[i].off == fe[i - 1].off && fe[i].size == fe[i - 1].size)) { r = rr_fail(c, RR_E_CORRUPT, "two files overlap on the disc"); break; }
        }
        if (!r && files_out) *files_out = n;
    }
out:
    free(raw); free(dec); free(hdr); free(fe);
    wii_fst_free(&f);
    wii_part_free(&p);
    wbfs_img_close(&w);
    return r;
}
