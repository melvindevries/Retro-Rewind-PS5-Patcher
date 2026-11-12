/* Porpoise Retro Rewind - sceHttp transport for the update engine.
 * Sequence follows the proven reference (pool -> ssl -> http -> template -> connection -> request).
 * Redirects are followed manually so that every hop can be required to be https.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "http_sce.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceSslInit(size_t pool_size);
int sceSslTerm(int context);
int sceHttpInit(int net_pool, int ssl_context, size_t pool_size);
int sceHttpTerm(int context);
int sceHttpCreateTemplate(int context, const char *user_agent, int version, int auto_proxy);
int sceHttpDeleteTemplate(int template_id);
int sceHttpCreateConnectionWithURL(int template_id, const char *url, int keep_alive);
int sceHttpDeleteConnection(int connection);
int sceHttpCreateRequestWithURL(int connection, int method, const char *url, uint64_t content_length);
int sceHttpDeleteRequest(int request);
int sceHttpSetAutoRedirect(int id, int enabled);
int sceHttpSetResolveTimeOut(int id, uint32_t usec);
int sceHttpSetConnectTimeOut(int id, uint32_t usec);
int sceHttpSetSendTimeOut(int id, uint32_t usec);
int sceHttpSetRecvTimeOut(int id, uint32_t usec);
int sceHttpsEnableOption(int id, uint32_t flags);
int sceHttpSendRequest(int request, const void *data, size_t size);
int sceHttpGetStatusCode(int request, int *status);
int sceHttpReadData(int request, void *data, size_t size);
int sceHttpGetAllResponseHeaders(int request, char **header, size_t *size);
int sceHttpGetResponseContentLength(int request, int *result, uint64_t *length);

enum {
    NET_POOL = 1024 * 1024, SSL_POOL = 304 * 1024, HTTP_POOL = 4 * 1024 * 1024,
    HTTP_1_1 = 2, METHOD_GET = 0, TIMEOUT_US = 30000000,
    /* server verify | name check | not-after | not-before | known CA | SNI */
    VERIFY_FLAGS = 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x80,
    MAX_REDIRECTS = 5, CHUNK = 256 * 1024
};

static int g_pool = -1, g_ssl = -1, g_http = -1;

int sce_http_start(void)
{
    int r;
    if (g_http >= 0) return 0;
    r = g_pool = sceNetPoolCreate("porpoise_rr", NET_POOL, 0);
    if (r >= 0) r = g_ssl = sceSslInit(SSL_POOL);
    if (r >= 0) r = g_http = sceHttpInit(g_pool, g_ssl, HTTP_POOL);
    if (r < 0) { sce_http_stop(); return r; }
    return 0;
}

void sce_http_stop(void)
{
    if (g_http >= 0) sceHttpTerm(g_http);
    if (g_ssl >= 0) sceSslTerm(g_ssl);
    if (g_pool >= 0) sceNetPoolDestroy(g_pool);
    g_http = g_ssl = g_pool = -1;
}

/* Finds "Location:" in a raw header block and copies the value. */
static int header_location(const char *h, size_t n, char *out, size_t cap)
{
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && h[e] != '\n') e++;
        if (e - i > 9 && strncasecmp(h + i, "location:", 9) == 0) {
            size_t s = i + 9, t = e;
            while (s < t && (h[s] == ' ' || h[s] == '\t')) s++;
            while (t > s && (h[t - 1] == '\r' || h[t - 1] == ' ')) t--;
            if (t - s == 0 || t - s >= cap) return -1;
            memcpy(out, h + s, t - s);
            out[t - s] = 0;
            return 0;
        }
        i = e + 1;
    }
    return -1;
}

static int resolve_redirect(const char *cur, const char *loc, char *out, size_t cap)
{
    if (strncmp(loc, "https://", 8) == 0) {
        if (strlen(loc) >= cap) return -1;
        strcpy(out, loc);
        return 0;
    }
    if (loc[0] == '/' && loc[1] != '/' && strncmp(cur, "https://", 8) == 0) {
        const char *hostend = strchr(cur + 8, '/');
        size_t hl = hostend ? (size_t)(hostend - cur) : strlen(cur);
        if (hl + strlen(loc) >= cap) return -1;
        memcpy(out, cur, hl);
        strcpy(out + hl, loc);
        return 0;
    }
    return -1; /* http:// and anything else is refused */
}

static int one_request(rr_ctx *c, const char *url, const char *dest_path, uint8_t **mem, size_t *mem_len,
                       size_t mem_max, uint64_t *expected, int *status, char *loc, size_t loc_cap)
{
    int tmpl = -1, conn = -1, req = -1, r, fd = -1, rc = 0;
    uint8_t *buf = NULL, *acc = NULL;
    size_t acc_len = 0, acc_cap = 0;
    uint64_t total = 0;
    *status = 0; loc[0] = 0;

    r = tmpl = sceHttpCreateTemplate(g_http, "PorpoiseRetroRewind/1.0", HTTP_1_1, 0);
    if (r >= 0) r = sceHttpSetAutoRedirect(tmpl, 0);
    if (r >= 0) r = sceHttpSetResolveTimeOut(tmpl, TIMEOUT_US);
    if (r >= 0) r = sceHttpSetConnectTimeOut(tmpl, TIMEOUT_US);
    if (r >= 0) r = sceHttpSetSendTimeOut(tmpl, TIMEOUT_US);
    if (r >= 0) r = sceHttpSetRecvTimeOut(tmpl, TIMEOUT_US);
    if (r >= 0) r = sceHttpsEnableOption(tmpl, VERIFY_FLAGS);
    if (r >= 0) r = conn = sceHttpCreateConnectionWithURL(tmpl, url, 0);
    if (r >= 0) r = req = sceHttpCreateRequestWithURL(conn, METHOD_GET, url, 0);
    if (r >= 0) r = sceHttpSendRequest(req, NULL, 0);
    if (r >= 0) r = sceHttpGetStatusCode(req, status);
    if (r < 0) { rc = rr_fail(c, RR_E_NET, "HTTPS request failed (sce error 0x%08x)", (unsigned)r); goto done; }

    if (*status >= 300 && *status < 400) {
        char *h = NULL; size_t hn = 0;
        if (sceHttpGetAllResponseHeaders(req, &h, &hn) >= 0 && h) header_location(h, hn, loc, loc_cap);
        goto done;
    }
    if (*status != 200) goto done;

    { int known = 1; uint64_t len = 0;
      if (sceHttpGetResponseContentLength(req, &known, &len) >= 0 && known == 0) { *expected = len; total = len; } }

    if (dest_path) {
        fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { rc = rr_fail(c, RR_E_IO, "cannot create %s", dest_path); goto done; }
    }
    buf = (uint8_t *)malloc(CHUNK);
    if (!buf) { rc = rr_fail(c, RR_E_IO, "out of memory"); goto done; }
    { uint64_t got_total = 0;
      for (;;) {
        int got;
        if (rr_cancelled(c)) { rc = rr_fail(c, RR_E_CANCEL, "cancelled"); goto done; }
        got = sceHttpReadData(req, buf, CHUNK);
        if (got < 0) { rc = rr_fail(c, RR_E_NET, "download interrupted (sce error 0x%08x)", (unsigned)got); goto done; }
        if (got == 0) break;
        if (fd >= 0) {
            if (rr_write_full(fd, buf, (size_t)got) != 0) { rc = rr_fail(c, RR_E_IO, "write failed (disk full?)"); goto done; }
        } else {
            if (acc_len + (size_t)got > mem_max) { rc = rr_fail(c, RR_E_NET, "response larger than %zu bytes", mem_max); goto done; }
            if (acc_len + (size_t)got + 1 > acc_cap) {
                size_t nc = acc_cap ? acc_cap * 2 : 8192;
                while (nc < acc_len + (size_t)got + 1) nc *= 2;
                acc = (uint8_t *)realloc(acc, nc);
                if (!acc) { rc = rr_fail(c, RR_E_IO, "out of memory"); goto done; }
                acc_cap = nc;
            }
            memcpy(acc + acc_len, buf, (size_t)got);
        }
        acc_len += (size_t)got; got_total += (uint64_t)got;
        rr_progress(c, "Downloading", got_total, total);
      }
      *mem_len = (size_t)got_total;
      if (!dest_path) { if (!acc) acc = (uint8_t *)calloc(1, 1); else acc[acc_len] = 0; *mem = acc; acc = NULL; }
    }
done:
    if (fd >= 0) { fsync(fd); close(fd); }
    free(buf); free(acc);
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
    return rc;
}

int sce_http_get(void *user, rr_ctx *c, const char *url, const char *dest_path, uint8_t **mem,
                 size_t *mem_len, size_t mem_max, uint64_t *expected_len, int *http_status)
{
    char cur[1024], next[1024];
    int hop;
    (void)user;
    if (g_http < 0) return rr_fail(c, RR_E_NET, "network stack is not initialised");
    if (strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof cur) return rr_fail(c, RR_E_UNSAFE, "refusing non-https URL");
    strcpy(cur, url);
    *expected_len = 0; *mem_len = 0; if (mem) *mem = NULL;
    for (hop = 0; hop <= MAX_REDIRECTS; hop++) {
        int rc = one_request(c, cur, dest_path, mem, mem_len, mem_max, expected_len, http_status, next, sizeof next);
        if (rc) return rc;
        if (*http_status >= 300 && *http_status < 400) {
            char abs[1024];
            if (!next[0] || resolve_redirect(cur, next, abs, sizeof abs) != 0)
                return rr_fail(c, RR_E_NET, "unusable or non-https redirect (HTTP %d)", *http_status);
            strcpy(cur, abs);
            continue;
        }
        return 0;
    }
    return rr_fail(c, RR_E_NET, "too many redirects");
}
