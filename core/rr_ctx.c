/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "rr_ctx.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/statvfs.h>
#define RR_HAVE_STATVFS 1
#endif

void rr_ctx_init(rr_ctx *c) { memset(c, 0, sizeof *c); }

void rr_logf(rr_ctx *c, int level, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (c && c->log) c->log(c->log_user, level, msg);
}

int rr_fail(rr_ctx *c, int code, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (c) {
        memcpy(c->err, msg, sizeof c->err);
        c->err[sizeof c->err - 1] = 0;
        rr_logf(c, RR_LOG_ERROR, "%s", msg);
    }
    return code;
}

void rr_progress(rr_ctx *c, const char *phase, uint64_t done, uint64_t total)
{
    if (c && c->progress) c->progress(c->progress_user, phase, done, total);
}

int rr_cancelled(const rr_ctx *c) { return c && c->cancel && *c->cancel; }

const char *rr_strerror(int code)
{
    switch (code) {
    case RR_OK: return "ok";
    case RR_E_IO: return "I/O error";
    case RR_E_FORMAT: return "invalid format";
    case RR_E_CANCEL: return "cancelled";
    case RR_E_CORRUPT: return "integrity check failed";
    case RR_E_USAGE: return "bad request";
    case RR_E_NET: return "network error";
    case RR_E_STATE: return "inconsistent state";
    case RR_E_UNSAFE: return "unsafe path";
    case RR_E_NOSPACE: return "not enough free space";
    case RR_E_UNSUPPORTED: return "unsupported";
    }
    return "error";
}

void *rr_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("out of memory\n", stderr); abort(); }
    return p;
}
void *rr_xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { fputs("out of memory\n", stderr); abort(); }
    return p;
}
char *rr_xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = rr_xmalloc(n);
    memcpy(d, s, n);
    return d;
}
char *rr_path_join(const char *a, const char *b)
{
    size_t la = strlen(a);
    while (*b == '/') b++;
    char *r = rr_xmalloc(la + strlen(b) + 2);
    memcpy(r, a, la);
    if (la && a[la - 1] != '/') r[la++] = '/';
    strcpy(r + la, b);
    return r;
}

int rr_pread_full(int fd, void *buf, size_t n, uint64_t off)
{
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return RR_E_IO;
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return RR_E_IO; }
        if (r == 0) return RR_E_IO; /* short file */
        p += r; n -= (size_t)r;
    }
    return 0;
}

int rr_write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return RR_E_IO; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

int rr_pwrite_full(int fd, const void *buf, size_t n, uint64_t off)
{
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return RR_E_IO;
    return rr_write_full(fd, buf, n);
}

int rr_fd_size(int fd, uint64_t *size)
{
    off_t cur = lseek(fd, 0, SEEK_CUR), end = lseek(fd, 0, SEEK_END);
    if (end < 0) return -1;
    if (cur >= 0) lseek(fd, cur, SEEK_SET);
    *size = (uint64_t)end;
    return 0;
}

int rr_stat(const char *path, int follow, rr_fileinfo *fi)
{
    struct stat st;
    memset(fi, 0, sizeof *fi);
    int r = follow ? stat(path, &st) : lstat(path, &st);
    if (r != 0 && !follow && errno != ENOENT && errno != ENOTDIR) r = stat(path, &st);
    if (r == 0) {
        fi->st_size = (uint64_t)st.st_size; fi->mtime = (int64_t)st.st_mtime;
        fi->kind = S_ISREG(st.st_mode) ? 1 : S_ISDIR(st.st_mode) ? 2 : S_ISLNK(st.st_mode) ? 3 : 4;
        if (fi->kind == 4) { /* unusual mode bits on this mount: opening it as a directory is the real test */
            int dfd = open(path, O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) { close(dfd); fi->kind = 2; }
        }
        return 0;
    }
    int e = errno;
    if (e == ENOENT || e == ENOTDIR) { errno = e; return -1; }
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { close(fd); fi->kind = 2; return 0; }
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char b;
        ssize_t n = read(fd, &b, 1);
        if (n < 0 && errno == EISDIR) fi->kind = 2;
        else { fi->kind = 1; uint64_t sz = 0; if (rr_fd_size(fd, &sz) == 0) fi->st_size = sz; }
        close(fd);
        return 0;
    }
    errno = e;
    return -1;
}

static int is_dir_path(const char *p)
{
    rr_fileinfo fi;
    return rr_stat(p, 1, &fi) == 0 && fi.kind == 2;
}

/* Creates every missing component. errno is preserved from the failing mkdir so callers can report it. */
int rr_mkdir_p(const char *path)
{
    char *t = rr_xstrdup(path);
    size_t n = strlen(t);
    int saved = 0, r = 0;
    while (n > 1 && t[n - 1] == '/') t[--n] = 0;
    for (char *p = t + 1; ; p++) {
        if (*p != '/' && *p != 0) continue;
        char sv = *p;
        *p = 0;
        if (!is_dir_path(t) && mkdir(t, 0777) != 0 && !is_dir_path(t)) { saved = errno; r = RR_E_IO; break; }
        *p = sv;
        if (!sv) break;
    }
    free(t);
    if (r) errno = saved;
    return r;
}

int rr_path_kind(const char *path)
{
    rr_fileinfo fi;
    if (rr_stat(path, 1, &fi) != 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    return fi.kind == 1 ? 1 : fi.kind == 2 ? 2 : -1;
}

int rr_file_size(const char *path, uint64_t *size)
{
    rr_fileinfo fi;
    if (rr_stat(path, 1, &fi) != 0 || fi.kind != 1) return RR_E_IO;
    *size = fi.st_size;
    return 0;
}

int rr_read_text_file(const char *path, char **out, size_t *len, size_t max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return RR_E_IO;
    uint64_t sz;
    if (rr_fd_size(fd, &sz) != 0 || sz > max) { close(fd); return RR_E_IO; }
    char *b = rr_xmalloc((size_t)sz + 1);
    int r = rr_pread_full(fd, b, (size_t)sz, 0);
    close(fd);
    if (r) { free(b); return r; }
    b[sz] = 0;
    *out = b;
    if (len) *len = (size_t)sz;
    return 0;
}

int rr_rename_over(const char *from, const char *to)
{
    return rename(from, to) == 0 ? 0 : RR_E_IO;
}

int rr_write_text_file_atomic(const char *path, const char *data, size_t len)
{
    size_t pl = strlen(path);
    char *tmp = rr_xmalloc(pl + 8);
    memcpy(tmp, path, pl);
    strcpy(tmp + pl, ".tmp");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { free(tmp); return RR_E_IO; }
    int r = rr_write_full(fd, data, len);
    if (!r && fsync(fd) != 0) r = RR_E_IO;
    if (close(fd) != 0 && !r) r = RR_E_IO;
    if (!r) r = rr_rename_over(tmp, path);
    if (r) unlink(tmp);
    free(tmp);
    return r;
}

int rr_remove_tree(const char *path)
{
    rr_fileinfo st;
    if (rr_stat(path, 0, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return 0;
        return (unlink(path) == 0 || rmdir(path) == 0) ? 0 : RR_E_IO; /* state unknown: just try to remove it */
    }
    if (st.kind == 2) {
        rr_dir *d = rr_opendir(path);
        if (!d) return RR_E_IO;
        const char *nm;
        int r = 0;
        /* collect first: removing while iterating a getdents buffer is not safe on every filesystem */
        char **names = NULL; size_t cnt = 0, cap = 0;
        while ((nm = rr_readdir(d))) {
            if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            if (cnt == cap) { cap = cap ? cap * 2 : 32; names = rr_xrealloc(names, cap * sizeof *names); }
            names[cnt++] = rr_xstrdup(nm);
        }
        rr_closedir(d);
        for (size_t i = 0; i < cnt; i++) {
            char *c = rr_path_join(path, names[i]);
            if (rr_remove_tree(c)) r = RR_E_IO;
            free(c); free(names[i]);
        }
        free(names);
        if (rmdir(path) != 0) r = RR_E_IO;
        return r;
    }
    return unlink(path) == 0 ? 0 : RR_E_IO;
}

int rr_copy_file_path(rr_ctx *c, const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0) return rr_fail(c, RR_E_IO, "cannot open %s", from);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { close(in); return rr_fail(c, RR_E_IO, "cannot create %s", to); }
    uint8_t *buf = rr_xmalloc(1 << 16);
    int r = 0;
    for (;;) {
        ssize_t n = read(in, buf, 1 << 16);
        if (n < 0) { if (errno == EINTR) continue; r = RR_E_IO; break; }
        if (n == 0) break;
        if (rr_write_full(out, buf, (size_t)n)) { r = RR_E_IO; break; }
    }
    free(buf);
    close(in);
    if (!r && fsync(out) != 0) r = RR_E_IO;
    if (close(out) != 0 && !r) r = RR_E_IO;
    if (r) { unlink(to); return rr_fail(c, RR_E_IO, "copy %s -> %s failed", from, to); }
    return 0;
}

uint64_t rr_free_space(const char *path)
{
#ifdef RR_HAVE_STATVFS
    struct statvfs sv;
    if (statvfs(path, &sv) == 0) return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
#endif
    (void)path;
    return 0;
}

#ifdef RR_USE_GETDENTS
#include <fcntl.h>
#include <dirent.h>
int getdents(int fd, char *buf, int nbytes);
#ifndef RR_GDE
#define RR_GDE getdirentries /* host tests rename it to avoid the glibc prototype */
#endif
int RR_GDE(int fd, char *buf, int nbytes, long *basep);
/* Three ways of listing a directory are tried in turn when it is opened; the first that yields a readable record wins.
 * What each one did is kept in a diagnostic string so a failure can be explained instead of guessed at. */
struct rr_dir { int mode; int fd; DIR *dir; int len, pos, bufsz; char buf[262144]; char name[512]; };
static char g_diag[320];
const char *rr_dir_diag(void) { return g_diag; }

/* Parses one record of either known dirent generation. Returns reclen (>0) and the name offset, or 0. */
static unsigned parse_rec(const unsigned char *e, int left, unsigned *off, unsigned *namlen, int *deleted)
{
    unsigned ra = left >= 8 ? (unsigned)(e[4] | e[5] << 8) : 0, na = left >= 8 ? e[7] : 0;
    unsigned rb = left >= 24 ? (unsigned)(e[16] | e[17] << 8) : 0, nb = left >= 24 ? (unsigned)(e[20] | e[21] << 8) : 0;
    if (ra >= 8 + na + 1 && ra <= (unsigned)left && (ra & 3) == 0 && na > 0 && e[8 + na] == 0 && strlen((const char *)e + 8) == na) {
        *off = 8; *namlen = na; *deleted = (e[0] | e[1] | e[2] | e[3]) == 0; return ra;
    }
    if (rb >= 24 + nb + 1 && rb <= (unsigned)left && (rb & 7) == 0 && nb > 0 && e[24 + nb] == 0 && strlen((const char *)e + 24) == nb) {
        *off = 24; *namlen = nb; *deleted = 0; return rb;
    }
    return 0;
}

static int fill(rr_dir *d, int first)
{
    long base = 0;
    int n = d->mode == 0 ? getdents(d->fd, d->buf, d->bufsz) : RR_GDE(d->fd, d->buf, d->bufsz, &base);
    if (first) snprintf(g_diag + strlen(g_diag), sizeof g_diag - strlen(g_diag), "%s%s:n=%d%s%d", strlen(g_diag) ? " " : "", d->mode == 0 ? "getdents" : "getdirentries", n, n < 0 ? ",errno=" : "", n < 0 ? errno : 0);
    if (n > d->bufsz) n = d->bufsz; /* never trust a count larger than the buffer we gave */
    d->len = n; d->pos = 0;
    return n;
}

rr_dir *rr_opendir(const char *path)
{
    g_diag[0] = 0;
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        int e1 = errno;
        fd = open(path, O_RDONLY); /* some mounts reject the O_DIRECTORY flag */
        snprintf(g_diag, sizeof g_diag, "open(O_DIRECTORY):errno=%d%s", e1, fd >= 0 ? " plain-open:ok" : "");
        if (fd < 0) { int e2 = errno; snprintf(g_diag + strlen(g_diag), sizeof g_diag - strlen(g_diag), " plain-open:errno=%d", e2); errno = e1; return NULL; }
    }
    rr_dir *d = rr_xmalloc(sizeof *d);
    memset(d, 0, offsetof(rr_dir, buf));
    d->fd = fd;
    /* 1st choice: the system's own opendir/readdir (works for other homebrew on the console). */
#ifndef RR_TEST_NO_LIBC_FIRST
    {
        DIR *sys = fdopendir(dup(fd));
        if (!sys) sys = opendir(path);
        if (sys) {
            errno = 0;
            struct dirent *first = readdir(sys);
            if (first || errno == 0) {
                closedir(sys);
                DIR *again = opendir(path);
                if (again) {
                    d->mode = 2; d->dir = again;
                    snprintf(g_diag + strlen(g_diag), sizeof g_diag - strlen(g_diag), "libc:ok");
                    return d;
                }
            } else {
                snprintf(g_diag + strlen(g_diag), sizeof g_diag - strlen(g_diag), "libc:readdir errno=%d ", errno);
                closedir(sys);
            }
        } else snprintf(g_diag + strlen(g_diag), sizeof g_diag - strlen(g_diag), "libc:opendir errno=%d ", errno);
    }
#endif
    /* The console's getdents answers EINVAL to small buffers (seen with 8-32 KiB); 64 KiB and up work. */
    static const int sizes[] = {65536, 131072, 262144, 32768};
    for (int mode = 0; mode < 2; mode++) {
        for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
            d->mode = mode; d->bufsz = sizes[k];
            lseek(fd, 0, SEEK_SET);
            int n = fill(d, 1);
            if (n > 0) {
                unsigned off, nl; int del;
                if (parse_rec((const unsigned char *)d->buf, n, &off, &nl, &del)) return d; /* buffer already holds the first records */
                snprintf(g_diag + strlen(g_diag), sizeof g_diag - strlen(g_diag), "(unparsable)");
            }
            if (n == 0) break; /* a real, empty answer: no point in other sizes */
        }
    }
    /* last resort: the system libc */
    d->mode = 2;
    d->dir = opendir(path);
    snprintf(g_diag + strlen(g_diag), sizeof g_diag - strlen(g_diag), " libc:%s", d->dir ? "opened" : "failed");
    if (!d->dir) { d->mode = 3; } /* the folder opens but cannot be listed: behave as an empty listing, with the diagnostics above */
    return d;
}

const char *rr_readdir(rr_dir *d)
{
    if (d->mode == 3) return NULL;
    if (d->mode == 2) {
        struct dirent *e = readdir(d->dir);
        if (!e) return NULL;
        snprintf(d->name, sizeof d->name, "%s", e->d_name);
        return d->name;
    }
    for (;;) {
        if (d->pos >= d->len) {
            if (fill(d, 0) <= 0) return NULL;
        }
        const unsigned char *e = (const unsigned char *)d->buf + d->pos;
        unsigned off, namlen; int deleted;
        unsigned reclen = parse_rec(e, d->len - d->pos, &off, &namlen, &deleted);
        if (!reclen) return NULL; /* unknown layout: stop instead of guessing */
        d->pos += (int)reclen;
        if (deleted) continue;
        memcpy(d->name, e + off, namlen + 1);
        return d->name;
    }
}
void rr_closedir(rr_dir *d)
{
    if (!d) return;
    if (d->mode == 2 && d->dir) closedir(d->dir);
    if (d->fd >= 0) close(d->fd);
    free(d);
}
void rr_dir_debug(const char *path, char *out, size_t cap)
{
    rr_dir *d = rr_opendir(path);
    if (!d) { snprintf(out, cap, "cannot list: errno=%d", errno); return; }
    int n = 0; while (rr_readdir(d)) n++;
    rr_closedir(d);
    snprintf(out, cap, "listing strategy log: %s; entries=%d", g_diag, n);
}
#else
void rr_dir_debug(const char *path, char *out, size_t cap) { (void)path; snprintf(out, cap, "libc readdir"); }
const char *rr_dir_diag(void) { return "libc readdir"; }
struct rr_dir { DIR *d; };
rr_dir *rr_opendir(const char *path)
{
    DIR *x = opendir(path);
    if (!x) return NULL;
    rr_dir *d = rr_xmalloc(sizeof *d);
    d->d = x;
    return d;
}
#ifdef RR_TEST_NO_LISTING /* tests: simulate a system where folders cannot be listed */
const char *rr_readdir(rr_dir *d) { (void)d; return NULL; }
#else
const char *rr_readdir(rr_dir *d) { struct dirent *e = readdir(d->d); return e ? e->d_name : NULL; }
#endif
void rr_closedir(rr_dir *d) { if (d) { closedir(d->d); free(d); } }
#endif
