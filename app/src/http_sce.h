/* Porpoise Retro Rewind - sceHttp transport for the update engine. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HTTP_SCE_H
#define HTTP_SCE_H
#include "rr_update.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One-time network stack setup (pool, ssl, http). Returns 0 or a negative sce error. */
int sce_http_start(void);
void sce_http_stop(void);
int sce_http_get(void *user, rr_ctx *c, const char *url, const char *dest_path, uint8_t **mem,
                 size_t *mem_len, size_t mem_max, uint64_t *expected_len, int *http_status);
#ifdef __cplusplus
}
#endif
#endif
