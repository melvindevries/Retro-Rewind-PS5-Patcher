/* Porpoise Retro Rewind - Wii disc / WBFS reader.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WII_DISC_H
#define WII_DISC_H
#include "rr_crypto.h"
#include "rr_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WII_SECTOR 0x8000u        /* one cluster on disc */
#define WII_HASH_BYTES 0x400u
#define WII_DATA_BYTES 0x7C00u
#define WII_GROUP_CLUSTERS 64u    /* 64 clusters = 2 MiB per H2 group */
#define WII_H3_SIZE 0x18000u
#define WII_TICKET_SIZE 0x2A4u
#define WII_PART_HDR_SIZE 0x2C0u
#define WII_DISC_HDR_SIZE 0x440u
#define WII_PART_OFFSET 0xF800000ull /* where the single data partition is placed */
#define WII_MAX_DISC_SIZE (143432ull * WII_SECTOR)

typedef struct {
    int fd;
    uint64_t file_size;
    uint32_t hd_sec_sz;
    int wbfs_sec_sz_s;
    uint32_t wbfs_sec_sz;
    uint16_t *wlba; /* BE-decoded block table of the single disc */
    uint32_t n_wlba;
    uint8_t hdr[0x100];
} wbfs_img;

int wbfs_img_open(rr_ctx *c, wbfs_img *w, const char *path);
void wbfs_img_close(wbfs_img *w);
/* read from the virtual disc address space; unmapped blocks read as zeros */
int wbfs_img_read(rr_ctx *c, const wbfs_img *w, uint64_t off, void *buf, size_t n);

typedef struct {
    uint64_t part_off;     /* disc offset of the partition */
    uint32_t type;
    uint8_t ticket[WII_TICKET_SIZE];
    uint32_t tmd_size, cert_size;
    uint8_t *tmd, *cert;
    uint8_t *h3;           /* WII_H3_SIZE bytes */
    uint64_t data_off;     /* relative to part_off */
    uint64_t data_size;    /* encrypted bytes */
    uint8_t title_key[16]; /* plain */
    rr_aes128 aes;
    /* decrypted cluster cache */
    int64_t cache_cluster;
    uint8_t cache[WII_DATA_BYTES];
    int verify_h0;
    const wbfs_img *img;
} wii_part;

int wii_find_data_partition(rr_ctx *c, const wbfs_img *w, wii_part *p);
void wii_part_free(wii_part *p);
/* read from the decrypted data address space (verifies H0 hashes when enabled) */
int wii_part_read(rr_ctx *c, wii_part *p, uint64_t dec_off, void *buf, size_t n);
uint64_t wii_part_data_capacity(const wii_part *p); /* decrypted bytes available */

/* Wii common key (derived exactly like Wiimm's ISO Tools does at start-up). */
void wii_common_key(int index, uint8_t out[16]);
void wii_decrypt_title_key(const uint8_t *ticket, uint8_t out[16]);
void wii_encrypt_title_key(uint8_t *ticket, const uint8_t key[16]);
void wii_fake_sign_ticket(uint8_t *ticket);
void wii_fake_sign_tmd(uint8_t *tmd, uint32_t tmd_size);

/* FST */
typedef struct {
    char *name;      /* full relative path with '/' separators */
    int is_dir;
    uint64_t offset; /* decrypted-space byte offset (already <<2) */
    uint32_t size;
} wii_fst_entry;

typedef struct {
    wii_fst_entry *e;
    size_t n;
    uint32_t dol_off, fst_off, fst_size; /* bytes, from boot.bin */
} wii_fst;

int wii_read_fst(rr_ctx *c, wii_part *p, wii_fst *f);
void wii_fst_free(wii_fst *f);

#ifdef __cplusplus
}
#endif
#endif
