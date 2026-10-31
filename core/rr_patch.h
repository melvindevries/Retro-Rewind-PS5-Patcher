/* Porpoise Retro Rewind - patch orchestration (detect base, run builder script, build, verify, install).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RR_PATCH_H
#define RR_PATCH_H
#include "rr_script.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *base_dir;     /* e.g. /data/porpoise/rewind/base */
    const char *pack_dir;     /* directory that CONTAINS RetroRewind6/ (e.g. /data/porpoise/rewind/pack) */
    const char *builder_dir;  /* e.g. /data/porpoise/rewind/builder */
    const char *out_dir;      /* e.g. /data/porpoise/games */
    char region;              /* 'E', 'P' or 'J' */
} rr_patch_cfg;

typedef struct {
    int base_found;           /* exactly one .wbfs present */
    char base_path[1024];
    char base_region;         /* 'E','P','J' or 0 if unreadable/unsupported */
    char base_id[8];
    char base_error[512];     /* why the base is unusable (empty when fine) */
    int out_exists;
    char out_path[1024];
    uint64_t out_size;
    int out_current;          /* output exists and was built from the current base/pack/builder */
} rr_patch_status;

typedef struct {
    int skipped_up_to_date;
    uint64_t out_size;
    uint64_t files;
    char out_path[1024];
} rr_patch_result;

const char *rr_region_dest_id(char region);   /* "RMCETO" ... or NULL */
const char *rr_region_src_id(char region);    /* "RMCE01" ... */
int rr_detect_base(rr_ctx *c, const char *base_dir, char *path_out, size_t cap, char *region_out, char id_out[8]);
int rr_patch_status_get(rr_ctx *c, const rr_patch_cfg *cfg, rr_patch_status *st);
int rr_patch_run(rr_ctx *c, const rr_patch_cfg *cfg, int force, rr_patch_result *res);
/* full verification of the installed output (GUI "verify" action) */
int rr_patch_verify_output(rr_ctx *c, const rr_patch_cfg *cfg);

#ifdef __cplusplus
}
#endif
#endif
