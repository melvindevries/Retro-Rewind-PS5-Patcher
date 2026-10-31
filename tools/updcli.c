/* Host test CLI for the update engine with a file-backed mock transport. SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../core/rr_update.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { const char *srv; const char *fault; int gets; } mock_t;

static int mock_get(void *user, rr_ctx *c, const char *url, const char *dest, uint8_t **mem, size_t *mem_len, size_t mem_max, uint64_t *expected, int *status)
{
    mock_t *m = user;
    m->gets++;
    const char *name = strrchr(url, '/'); name = name ? name + 1 : url;
    char path[4096]; snprintf(path, sizeof path, "%s/%s", m->srv, name);
    *expected = 0;
    if (m->fault && !strncmp(m->fault, "status:", 7)) {
        int code = atoi(m->fault + 7); const char *f = strchr(m->fault + 7, ':');
        if (f && !strcmp(f + 1, name)) { *status = code; *mem_len = 0; if (!dest) *mem = calloc(1, 1); return 0; }
    }
    if (m->fault && !strncmp(m->fault, "neterr:", 7) && !strcmp(m->fault + 7, name)) return rr_fail(c, RR_E_NET, "simulated network failure");
    FILE *f = fopen(path, "rb");
    if (!f) { *status = 404; *mem_len = 0; if (!dest) *mem = calloc(1, 1); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return rr_fail(c, RR_E_IO, "mock read"); }
    fclose(f);
    buf[sz] = 0;
    *expected = (uint64_t)sz;
    size_t n = (size_t)sz;
    if (m->fault && !strncmp(m->fault, "truncate:", 9) && !strcmp(m->fault + 9, name)) n = n / 2;   /* server says full length, body is short */
    if (m->fault && !strncmp(m->fault, "flip:", 5) && !strcmp(m->fault + 5, name)) buf[n / 3] ^= 0x55;
    *status = 200;
    if (dest) {
        int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || rr_write_full(fd, buf, n)) { free(buf); if (fd >= 0) close(fd); return rr_fail(c, RR_E_IO, "mock write"); }
        close(fd); free(buf); *mem_len = n;
    } else {
        if (n > mem_max) { free(buf); return rr_fail(c, RR_E_NET, "response too large"); }
        *mem = buf; *mem_len = n;
    }
    return 0;
}

static void logcb(void *u, int l, const char *msg) { (void)u; if (l >= 1) fprintf(stderr, "[%s] %s\n", l == 1 ? "info" : l == 2 ? "warn" : "ERROR", msg); }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: updcli check|apply|applylatest|recover PACK SRV [fault]\n"); return 2; }
    rr_ctx c; rr_ctx_init(&c); c.log = logcb;
    mock_t m = {argv[3], argc > 4 ? argv[4] : NULL, 0};
    rr_http h = {mock_get, &m};
    if (!strcmp(argv[1], "recover")) { int r = rr_update_recover(&c, argv[2]); printf(r ? "FAILED: %s\n" : "RECOVERED\n", c.err); return r != 0; }
    if (!strcmp(argv[1], "builder")) { /* argv[5] = archive name on the mock server */
        char url[600]; snprintf(url, sizeof url, "https://github.com/melvindevries/Retro-Rewind-PS5-Patcher/raw/main/builder/%s", argc > 5 ? argv[5] : "RetroRewindBuilder.zip");
        int r = rr_install_builder(&c, &h, argv[2], url);
        if (r) { printf("FAILED: %s\n", c.err); return 1; }
        printf("BUILDER-OK present=%d gets=%d\n", rr_builder_present(argv[2]), m.gets); return 0;
    }
    if (!strcmp(argv[1], "full")) { /* full RR_FULL_URL-style install; argv[5] = archive file name on the mock server */
        char url[600]; snprintf(url, sizeof url, "https://update.rwfc.net/RetroRewind/zip/%s", argc > 5 ? argv[5] : "RetroRewind.zip");
        int r = rr_install_full(&c, &h, argv[2], url);
        if (r) { printf("FAILED: %s\n", c.err); return 1; }
        char v[32]; rr_read_installed_version(&c, argv[2], v);
        printf("INSTALLED now=%s gets=%d\n", v, m.gets); return 0;
    }
    rr_update_state s;
    int r = rr_update_check_ex(&c, &h, argv[2], &s, !strcmp(argv[1], "applylatest"));
    if (r) { printf("FAILED: %s\n", c.err); return 1; }
    printf("installed=%s latest=%s pending=%zu versions=%zu\n", s.installed, s.latest, s.n_pending, s.n_versions);
    if (!strcmp(argv[1], "apply") || !strcmp(argv[1], "applylatest")) {
        r = rr_update_apply(&c, &h, argv[2], &s);
        if (r) { printf("FAILED: %s (now at %s)\n", c.err, s.installed); rr_update_state_free(&s); return 1; }
        printf("APPLIED now=%s gets=%d\n", s.installed, m.gets);
    }
    rr_update_state_free(&s);
    return 0;
}
