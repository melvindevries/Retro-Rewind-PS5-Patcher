/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "wii_disc.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ----------------------------------------------------------------- keys */

void wii_common_key(int index, uint8_t out[16])
{
    /* Same derivation as the supplied Wiimm's ISO Tools (lib-std.c): the key is never stored
     * in clear; it is mask ^ SHA1(base)[0..16] ^ SHA1(template)[0..16]. */
    static const uint8_t mask[2][16] = {
        {0x25, 0x00, 0x94, 0x12, 0x2a, 0x3e, 0x4e, 0x1f, 0x03, 0x2d, 0xc5, 0xc9, 0x30, 0x2a, 0x58, 0xab},
        {0xad, 0x5c, 0x95, 0x84, 0x80, 0xda, 0x93, 0xd5, 0x58, 0x06, 0xfe, 0x77, 0xf9, 0xe7, 0x69, 0x22}};
    static const char base[16] = {'W', 'i', 'i', 'm', 'm', 's', ' ', 'W', 'B', 'F', 'S', ' ', 'T', 'o', 'o', 'l'};
    static const char templ[16] = {'c', 'o', 'm', 'm', 'o', 'n', ' ', 'k', 'e', 'y', ' ', 'u', 'n', 's', 'e', 't'};
    uint8_t h1[20], h2[20];
    rr_sha1(base, 16, h1);
    rr_sha1(templ, 16, h2);
    if (index < 0 || index > 1) index = 0;
    for (int i = 0; i < 16; i++) out[i] = mask[index][i] ^ h1[i] ^ h2[i];
}

#define TIK_KEY 0x1bf
#define TIK_TITLE_ID 0x1dc
#define TIK_CKEY_IDX 0x1f1

void wii_decrypt_title_key(const uint8_t *ticket, uint8_t out[16])
{
    uint8_t ck[16], iv[16] = {0};
    wii_common_key(ticket[TIK_CKEY_IDX] <= 1 ? ticket[TIK_CKEY_IDX] : 0, ck);
    memcpy(iv, ticket + TIK_TITLE_ID, 8);
    rr_aes128 k;
    rr_aes128_set_key(&k, ck);
    rr_aes128_cbc_decrypt(&k, iv, ticket + TIK_KEY, out, 16);
}

void wii_encrypt_title_key(uint8_t *ticket, const uint8_t key[16])
{
    uint8_t ck[16], iv[16] = {0};
    wii_common_key(ticket[TIK_CKEY_IDX] <= 1 ? ticket[TIK_CKEY_IDX] : 0, ck);
    memcpy(iv, ticket + TIK_TITLE_ID, 8);
    rr_aes128 k;
    rr_aes128_set_key(&k, ck);
    rr_aes128_cbc_encrypt(&k, iv, key, ticket + TIK_KEY, 16);
}

static void fake_sign(uint8_t *buf, size_t size, size_t fake_off, size_t fake_len)
{
    memset(buf + 4, 0, 0x100);        /* signature */
    memset(buf + 0x104, 0, 0x3c);     /* signature padding */
    memset(buf + fake_off, 0, fake_len);
    uint8_t h[20];
    for (uint32_t v = 0;; v++) {
        buf[fake_off] = (uint8_t)v; buf[fake_off + 1] = (uint8_t)(v >> 8);
        buf[fake_off + 2] = (uint8_t)(v >> 16); buf[fake_off + 3] = (uint8_t)(v >> 24);
        rr_sha1(buf + 0x140, size - 0x140, h);
        if (!h[0]) return;
        if (v == 0xFFFFFFFFu) return; /* cannot happen in practice */
    }
}

void wii_fake_sign_ticket(uint8_t *t) { fake_sign(t, WII_TICKET_SIZE, 0x24c, 0x58); }
void wii_fake_sign_tmd(uint8_t *tmd, uint32_t size) { fake_sign(tmd, size, 0x19a, 0x3e); }

/* ----------------------------------------------------------------- WBFS */

int wbfs_img_open(rr_ctx *c, wbfs_img *w, const char *path)
{
    memset(w, 0, sizeof *w);
    w->fd = open(path, O_RDONLY);
    if (w->fd < 0) return rr_fail(c, RR_E_IO, "cannot open %s", path);
    { uint64_t fsz = 0;
      if (rr_fd_size(w->fd, &fsz) != 0) { wbfs_img_close(w); return rr_fail(c, RR_E_IO, "%s is not a regular file", path); }
      w->file_size = fsz; }
    uint8_t head[512];
    if (w->file_size < 0x400 || rr_pread_full(w->fd, head, sizeof head, 0)) { wbfs_img_close(w); return rr_fail(c, RR_E_FORMAT, "%s is too small to be a WBFS file", path); }
    if (memcmp(head, "WBFS", 4)) { wbfs_img_close(w); return rr_fail(c, RR_E_FORMAT, "%s is not a WBFS file (bad magic)", path); }
    int hd_s = head[8], wb_s = head[9];
    if (hd_s < 9 || hd_s > 12 || wb_s < 15 || wb_s > 26) { wbfs_img_close(w); return rr_fail(c, RR_E_FORMAT, "unsupported WBFS geometry (%d/%d)", hd_s, wb_s); }
    w->hd_sec_sz = 1u << hd_s;
    w->wbfs_sec_sz_s = wb_s;
    w->wbfs_sec_sz = 1u << wb_s;
    int used = 0, slot = -1;
    for (int i = 0; i < (int)(w->hd_sec_sz - 12) && i < 500; i++) {
        uint8_t b;
        if (rr_pread_full(w->fd, &b, 1, 12 + (uint64_t)i)) break;
        if (b) { used++; if (slot < 0) slot = i; }
    }
    if (used != 1 || slot != 0) {
        wbfs_img_close(w);
        return rr_fail(c, used ? RR_E_UNSUPPORTED : RR_E_FORMAT, used ? "WBFS file holds %d discs; exactly one game image is required" : "WBFS file holds no disc", used);
    }
    w->n_wlba = (uint32_t)((143432u * 2u) >> (wb_s - 15));
    if (w->n_wlba > 65535) w->n_wlba = 65535;
    uint8_t *raw = rr_xmalloc((size_t)w->n_wlba * 2);
    if (rr_pread_full(w->fd, w->hdr, 0x100, w->hd_sec_sz) || rr_pread_full(w->fd, raw, (size_t)w->n_wlba * 2, w->hd_sec_sz + 0x100)) {
        free(raw); wbfs_img_close(w);
        return rr_fail(c, RR_E_FORMAT, "WBFS disc info is truncated");
    }
    w->wlba = rr_xmalloc((size_t)w->n_wlba * sizeof(uint16_t));
    for (uint32_t i = 0; i < w->n_wlba; i++) {
        w->wlba[i] = rr_be16(raw + 2 * i);
        if (w->wlba[i] && ((uint64_t)w->wlba[i] + 1) * w->wbfs_sec_sz > w->file_size) {
            free(raw); wbfs_img_close(w);
            return rr_fail(c, RR_E_CORRUPT, "WBFS file is truncated (block %u beyond end of file)", (unsigned)w->wlba[i]);
        }
    }
    free(raw);
    return 0;
}

void wbfs_img_close(wbfs_img *w)
{
    if (w->fd > 0 || w->wlba) {
        if (w->fd > 0) close(w->fd);
        free(w->wlba);
    }
    memset(w, 0, sizeof *w);
}

int wbfs_img_read(rr_ctx *c, const wbfs_img *w, uint64_t off, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        uint64_t blk = off >> w->wbfs_sec_sz_s;
        uint32_t in = (uint32_t)(off & (w->wbfs_sec_sz - 1));
        size_t take = w->wbfs_sec_sz - in;
        if (take > n) take = n;
        if (blk >= w->n_wlba || !w->wlba[blk]) memset(p, 0, take);
        else if (rr_pread_full(w->fd, p, take, (uint64_t)w->wlba[blk] * w->wbfs_sec_sz + in))
            return rr_fail(c, RR_E_IO, "read error in WBFS block %u", (unsigned)w->wlba[blk]);
        p += take; off += take; n -= take;
    }
    return 0;
}

/* ------------------------------------------------------------ partition */

int wii_find_data_partition(rr_ctx *c, const wbfs_img *w, wii_part *p)
{
    memset(p, 0, sizeof *p);
    p->img = w;
    p->cache_cluster = -1;
    uint8_t pt[0x20];
    int r = wbfs_img_read(c, w, 0x40000, pt, sizeof pt);
    if (r) return r;
    uint64_t found = 0; int have = 0; uint32_t ftype = 0;
    for (int t = 0; t < 4 && !have; t++) {
        uint32_t cnt = rr_be32(pt + t * 8), off4 = rr_be32(pt + t * 8 + 4);
        if (!cnt || cnt > 8 || !off4) continue;
        uint8_t ent[64];
        r = wbfs_img_read(c, w, (uint64_t)off4 << 2, ent, cnt * 8);
        if (r) return r;
        for (uint32_t i = 0; i < cnt; i++) {
            if (rr_be32(ent + i * 8 + 4) == 0) { found = (uint64_t)rr_be32(ent + i * 8) << 2; ftype = 0; have = 1; break; }
        }
    }
    if (!have) return rr_fail(c, RR_E_FORMAT, "no data partition found in the disc image");
    p->part_off = found; p->type = ftype;
    uint8_t ph[WII_PART_HDR_SIZE];
    if ((r = wbfs_img_read(c, w, found, ph, sizeof ph))) return r;
    memcpy(p->ticket, ph, WII_TICKET_SIZE);
    p->tmd_size = rr_be32(ph + 0x2a4);
    uint64_t tmd_off = (uint64_t)rr_be32(ph + 0x2a8) << 2;
    p->cert_size = rr_be32(ph + 0x2ac);
    uint64_t cert_off = (uint64_t)rr_be32(ph + 0x2b0) << 2, h3_off = (uint64_t)rr_be32(ph + 0x2b4) << 2;
    p->data_off = (uint64_t)rr_be32(ph + 0x2b8) << 2;
    p->data_size = (uint64_t)rr_be32(ph + 0x2bc) << 2;
    if (p->tmd_size < 0x1e4 + 0x24 || p->tmd_size > 0x4000 || p->cert_size > 0x4000 || p->data_size == 0 || (p->data_size % WII_SECTOR) ||
        p->data_off < 0x20000 || (p->data_off % WII_SECTOR) || h3_off + WII_H3_SIZE > p->data_off)
        return rr_fail(c, RR_E_FORMAT, "partition header is malformed");
    p->tmd = rr_xmalloc(p->tmd_size); p->cert = rr_xmalloc(p->cert_size ? p->cert_size : 1); p->h3 = rr_xmalloc(WII_H3_SIZE);
    if ((r = wbfs_img_read(c, w, found + tmd_off, p->tmd, p->tmd_size)) || (r = wbfs_img_read(c, w, found + cert_off, p->cert, p->cert_size)) ||
        (r = wbfs_img_read(c, w, found + h3_off, p->h3, WII_H3_SIZE))) { wii_part_free(p); return r; }
    uint8_t h[20];
    rr_sha1(p->h3, WII_H3_SIZE, h);
    if (memcmp(h, p->tmd + 0x1f4, 20)) { wii_part_free(p); return rr_fail(c, RR_E_CORRUPT, "partition H3 table does not match its TMD hash (damaged image)"); }
    wii_decrypt_title_key(p->ticket, p->title_key);
    rr_aes128_set_key(&p->aes, p->title_key);
    p->verify_h0 = 1;
    return 0;
}

void wii_part_free(wii_part *p)
{
    free(p->tmd); free(p->cert); free(p->h3);
    p->tmd = p->cert = p->h3 = NULL;
}

uint64_t wii_part_data_capacity(const wii_part *p) { return p->data_size / WII_SECTOR * WII_DATA_BYTES; }

static int load_cluster(rr_ctx *c, wii_part *p, int64_t cl)
{
    if (p->cache_cluster == cl) return 0;
    if ((uint64_t)cl * WII_SECTOR >= p->data_size) return rr_fail(c, RR_E_FORMAT, "read beyond the end of the partition (cluster %lld)", (long long)cl);
    uint8_t raw[WII_SECTOR], dec[WII_SECTOR], iv0[16] = {0};
    int r = wbfs_img_read(c, p->img, p->part_off + p->data_off + (uint64_t)cl * WII_SECTOR, raw, WII_SECTOR);
    if (r) return r;
    {   /* scrubbed (unmapped) sectors read as zeros: they carry no data and have no valid hashes */
        size_t z = 0;
        while (z < WII_SECTOR && !raw[z]) z++;
        if (z == WII_SECTOR) {
            memset(p->cache, 0, WII_DATA_BYTES);
            p->cache_cluster = cl;
            return 0;
        }
    }
    rr_aes128_cbc_decrypt(&p->aes, iv0, raw, dec, WII_HASH_BYTES);
    rr_aes128_cbc_decrypt(&p->aes, raw + 0x3d0, raw + WII_HASH_BYTES, dec + WII_HASH_BYTES, WII_DATA_BYTES);
    if (p->verify_h0) {
        for (int j = 0; j < 31; j++) {
            uint8_t h[20];
            rr_sha1(dec + WII_HASH_BYTES + (size_t)j * 0x400, 0x400, h);
            if (memcmp(h, dec + j * 20, 20)) {
                p->cache_cluster = -1;
                return rr_fail(c, RR_E_CORRUPT, "base image is damaged: hash mismatch in cluster %lld", (long long)cl);
            }
        }
    }
    memcpy(p->cache, dec + WII_HASH_BYTES, WII_DATA_BYTES);
    p->cache_cluster = cl;
    return 0;
}

int wii_part_read(rr_ctx *c, wii_part *p, uint64_t off, void *buf, size_t n)
{
    uint8_t *o = buf;
    if (off + n > wii_part_data_capacity(p) || off + n < off) return rr_fail(c, RR_E_FORMAT, "read beyond the end of the partition data");
    while (n) {
        int64_t cl = (int64_t)(off / WII_DATA_BYTES);
        uint32_t in = (uint32_t)(off % WII_DATA_BYTES);
        size_t take = WII_DATA_BYTES - in;
        if (take > n) take = n;
        int r = load_cluster(c, p, cl);
        if (r) return r;
        memcpy(o, p->cache + in, take);
        o += take; off += take; n -= take;
    }
    return 0;
}

/* ------------------------------------------------------------------ FST */

void wii_fst_free(wii_fst *f)
{
    for (size_t i = 0; i < f->n; i++) free(f->e[i].name);
    free(f->e);
    memset(f, 0, sizeof *f);
}

int wii_read_fst(rr_ctx *c, wii_part *p, wii_fst *f)
{
    memset(f, 0, sizeof *f);
    uint8_t boot[WII_DISC_HDR_SIZE];
    int r = wii_part_read(c, p, 0, boot, sizeof boot);
    if (r) return r;
    f->dol_off = rr_be32(boot + 0x420) << 2;
    f->fst_off = rr_be32(boot + 0x424) << 2;
    f->fst_size = rr_be32(boot + 0x428) << 2;
    uint64_t cap = wii_part_data_capacity(p);
    if (f->fst_size < 12 || f->fst_size > (16u << 20) || f->fst_off + (uint64_t)f->fst_size > cap) return rr_fail(c, RR_E_FORMAT, "FST location is invalid");
    uint8_t *fst = rr_xmalloc(f->fst_size);
    if ((r = wii_part_read(c, p, f->fst_off, fst, f->fst_size))) { free(fst); return r; }
    uint32_t n = rr_be32(fst + 8);
    if (n < 1 || (uint64_t)n * 12 > f->fst_size) { free(fst); return rr_fail(c, RR_E_FORMAT, "FST entry count is invalid"); }
    const uint8_t *names = fst + (size_t)n * 12;
    size_t names_len = f->fst_size - (size_t)n * 12;
    f->e = rr_xmalloc(sizeof(wii_fst_entry) * (n ? n : 1));
    /* iterative walk keeping a stack of (dir end index, path length) */
    struct { uint32_t end; size_t plen; } st[64];
    int sp = 0;
    char *path = rr_xmalloc(1024); size_t plen = 0; path[0] = 0;
    st[0].end = n; st[0].plen = 0;
    for (uint32_t i = 1; i < n; i++) {
        while (sp > 0 && i >= st[sp].end) { plen = st[sp].plen; path[plen] = 0; sp--; }
        const uint8_t *e = fst + (size_t)i * 12;
        uint32_t noff = rr_be32(e) & 0xffffff, isdir = e[0];
        uint32_t a = rr_be32(e + 4), b = rr_be32(e + 8);
        if (noff >= names_len) { r = rr_fail(c, RR_E_FORMAT, "FST name offset out of range"); goto bad; }
        size_t nl = strnlen((const char *)names + noff, names_len - noff);
        if (noff + nl >= names_len || nl == 0 || nl > 255 || plen + nl + 2 > 1024 || memchr(names + noff, '/', nl)) { r = rr_fail(c, RR_E_FORMAT, "FST name is invalid"); goto bad; }
        wii_fst_entry *o = &f->e[f->n++];
        size_t full = plen + (plen ? 1 : 0) + nl;
        o->name = rr_xmalloc(full + 1);
        memcpy(o->name, path, plen);
        if (plen) o->name[plen] = '/';
        memcpy(o->name + plen + (plen ? 1 : 0), names + noff, nl);
        o->name[full] = 0;
        o->is_dir = isdir == 1;
        if (isdir > 1) { r = rr_fail(c, RR_E_FORMAT, "FST entry type invalid"); goto bad; }
        if (o->is_dir) {
            if (b <= i || b > n || a >= (uint32_t)i + 1 + n || sp >= 63 || b > st[sp].end) { r = rr_fail(c, RR_E_FORMAT, "FST directory structure is invalid"); goto bad; }
            o->offset = 0; o->size = 0;
            sp++; st[sp].end = b; st[sp].plen = plen;
            memcpy(path, o->name, full + 1); plen = full;
        } else {
            o->offset = (uint64_t)a << 2; o->size = b;
            if (o->offset + b > cap) { r = rr_fail(c, RR_E_FORMAT, "FST file extent beyond partition: %s", o->name); goto bad; }
        }
    }
    free(path); free(fst);
    return 0;
bad:
    free(path); free(fst); wii_fst_free(f);
    return r;
}
