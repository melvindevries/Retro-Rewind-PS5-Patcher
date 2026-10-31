/* Porpoise Retro Rewind - official update discovery and safe merge of Update-only ZIPs.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RR_UPDATE_H
#define RR_UPDATE_H
#include "rr_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RR_VERSION_URL "https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt"

/* Transport supplied by the platform (sceHttp on PS5, a mock in tests).
 * dest_path != NULL: stream the body into that file (created/truncated), *mem_len = bytes written.
 * dest_path == NULL: return the body in *mem (malloc, NUL-terminated); more than mem_max bytes is an error.
 * *expected_len receives Content-Length when the server sent one (else 0). Redirects are followed by the transport
 * (https only). Returns 0 when an HTTP response arrived (any status), negative on transport failure. */
typedef struct rr_http {
    int (*get)(void *user, rr_ctx *c, const char *url, const char *dest_path, uint8_t **mem, size_t *mem_len, size_t mem_max, uint64_t *expected_len, int *http_status);
    void *user;
} rr_http;

typedef struct { char version[32]; char url[512]; char path[160]; char desc[64]; } rr_manifest_entry;
typedef struct { rr_manifest_entry *e; size_t n; } rr_manifest;

int rr_manifest_parse(rr_ctx *c, const char *text, rr_manifest *m);
void rr_manifest_free(rr_manifest *m);
/* version strings are dotted decimals ("6.13.4"); returns <0,0,>0; *ok=0 when either is malformed */
int rr_version_cmp(const char *a, const char *b, int *ok);
int rr_read_installed_version(rr_ctx *c, const char *pack_dir, char out[32]);

typedef struct {
    char installed[32];
    char latest[32];
    rr_manifest m;
    size_t first_pending; /* index into m.e of the first entry newer than installed */
    size_t n_pending;     /* number of zips still to apply */
    size_t n_versions;    /* number of distinct versions to step through */
    int up_to_date;
} rr_update_state;

void rr_update_state_free(rr_update_state *s);
int rr_update_recover(rr_ctx *c, const char *pack_dir);   /* rolls back an interrupted merge, removes leftovers */
int rr_update_check(rr_ctx *c, const rr_http *h, const char *pack_dir, rr_update_state *s);
/* latest_only != 0: plan only the archives of the newest release (skips releases in between; no "too old" check). */
int rr_update_check_ex(rr_ctx *c, const rr_http *h, const char *pack_dir, rr_update_state *s, int latest_only);
/* Applies every pending version in order. On failure the pack is left exactly at the last fully applied version. */
int rr_update_apply(rr_ctx *c, const rr_http *h, const char *pack_dir, rr_update_state *s);

/* exposed for tests */
int rr_merge_stage(rr_ctx *c, const char *pack_dir, const char *stage_root, const char *version);
int rr_merge_stage_ex(rr_ctx *c, const char *pack_dir, const char *stage_root, const char *version, size_t expected_files, const char *list_path);

#ifdef __cplusplus
}
#endif
#endif
