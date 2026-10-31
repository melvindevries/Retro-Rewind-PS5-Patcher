/* Porpoise Retro Rewind - virtual file tree + Wii disc / WBFS builder.
 * Reimplements what `wit copy <dir> --wbfs` does for the Retro Rewind builder, in portable C.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WII_BUILD_H
#define WII_BUILD_H
#include "wii_disc.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RB_BASE = 0, RB_HOST = 1, RB_MEM = 2 };

typedef struct rr_blob {
    int kind;
    uint64_t base_off; /* RB_BASE: offset in the decrypted partition data space */
    uint64_t size;
    char *path;        /* RB_HOST */
    uint8_t *mem;      /* RB_MEM */
    uint64_t dest_off; /* layout result */
    int placed;
} rr_blob;

typedef struct rr_node {
    char *path; /* relative to the disc's files/ root, '/' separated, no trailing slash */
    int is_dir;
    int blob;   /* index into blobs, -1 for directories */
} rr_node;

typedef struct rr_vtree {
    rr_node *n; size_t nn, capn;
    rr_blob *b; size_t nb, capb;
    int dol_blob, bi2_blob; /* sys/main.dol, sys/bi2.bin */
} rr_vtree;

void rr_vt_init(rr_vtree *t);
void rr_vt_free(rr_vtree *t);
int rr_vt_load_base(rr_ctx *c, rr_vtree *t, wii_part *p, const wii_fst *f);
int rr_vt_find(const rr_vtree *t, const char *path);
int rr_vt_mkdir_p(rr_ctx *c, rr_vtree *t, const char *path);
/* create or overwrite (in place, hard-link semantics) a file node with the content of a host file */
int rr_vt_put_host(rr_ctx *c, rr_vtree *t, const char *path, const char *host, uint64_t size);
/* sys/ files: only "main.dol" and "bi2.bin" are supported */
int rr_vt_put_sys_host(rr_ctx *c, rr_vtree *t, const char *name, const char *host, uint64_t size);
int rr_vt_patch_dol(rr_ctx *c, rr_vtree *t, uint32_t addr, const uint8_t *bytes, size_t n);
size_t rr_vt_count_files(const rr_vtree *t);

typedef struct {
    char disc_id[7];   /* 6 chars, whole disc header id (e.g. RMCETO) */
    char boot_id[5];   /* first 4 chars of the boot.bin id */
    char ticket_id[5];
    char tmd_id[5];
    char name[65];
} rr_build_cfg;

typedef struct {
    uint64_t data_bytes;        /* used decrypted bytes */
    uint32_t clusters;
    uint64_t wbfs_size;
    uint8_t plain_sha1[20];     /* SHA-1 over the whole plaintext cluster stream (for the verifier) */
    uint64_t file_count;
} rr_build_result;

/* Builds a complete WBFS image at out_path (caller supplies a temporary name). */
int wii_build_wbfs(rr_ctx *c, wii_part *base, rr_vtree *t, const rr_build_cfg *cfg, const char *out_path, rr_build_result *res);

/* Independent verification of a finished WBFS image. expect_id may be NULL.
 * If expect_plain_sha1 is not NULL the plaintext stream hash must match. */
int rr_verify_wbfs(rr_ctx *c, const char *path, const char *expect_disc_id, const uint8_t *expect_plain_sha1, uint64_t *files_out);

int nintendo_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
#endif
