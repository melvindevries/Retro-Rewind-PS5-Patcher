/* Porpoise Retro Rewind - shared context, error codes and small IO helpers.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RR_CTX_H
#define RR_CTX_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RR_OK = 0,
    RR_E_IO = -1,       /* read/write/open failure */
    RR_E_FORMAT = -2,   /* input is not what it claims to be */
    RR_E_CANCEL = -3,   /* user cancelled */
    RR_E_CORRUPT = -4,  /* checksum / integrity failure */
    RR_E_USAGE = -5,    /* bad arguments or state */
    RR_E_NET = -6,      /* network or HTTP failure */
    RR_E_STATE = -7,    /* inconsistent on-disk state, refused to continue */
    RR_E_UNSAFE = -8,   /* unsafe path / archive entry */
    RR_E_NOSPACE = -9,  /* not enough free space */
    RR_E_UNSUPPORTED = -10
};

enum { RR_LOG_DEBUG = 0, RR_LOG_INFO = 1, RR_LOG_WARN = 2, RR_LOG_ERROR = 3 };

typedef struct rr_ctx {
    void (*log)(void *user, int level, const char *msg);
    void *log_user;
    void (*progress)(void *user, const char *phase, uint64_t done, uint64_t total);
    void *progress_user;
    volatile int *cancel; /* optional: nonzero requests cancellation */
    char err[512];        /* last error message (set by rr_fail) */
} rr_ctx;

void rr_ctx_init(rr_ctx *c);
void rr_logf(rr_ctx *c, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int rr_fail(rr_ctx *c, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void rr_progress(rr_ctx *c, const char *phase, uint64_t done, uint64_t total);
int rr_cancelled(const rr_ctx *c);
const char *rr_strerror(int code);

/* ---- IO helpers (lseek/read/write only; no pread/pwrite dependency) ---- */
int rr_pread_full(int fd, void *buf, size_t n, uint64_t off);   /* 0 / -errno-ish RR_E_IO */
int rr_pwrite_full(int fd, const void *buf, size_t n, uint64_t off);
int rr_write_full(int fd, const void *buf, size_t n);
int rr_mkdir_p(const char *path);                                 /* 0 on success */
/* stat that keeps working where the plain call fails for some entries (e.g. EOVERFLOW / sandbox quirks): falls back to
 * open()+lseek(). kind: 1 file, 2 directory, 3 symlink (only when follow == 0 and stat works), 4 other.
 * Returns 0, or -1 with errno set (ENOENT/ENOTDIR = does not exist). mtime is 0 when only the fallback worked. */
typedef struct { uint64_t st_size; int64_t mtime; int kind; } rr_fileinfo;
int rr_stat(const char *path, int follow, rr_fileinfo *fi);
int rr_fd_size(int fd, uint64_t *size);                           /* size via lseek, 0 on success */
int rr_path_kind(const char *path);                               /* 0 none, 1 file, 2 dir, -1 other/error */
int rr_file_size(const char *path, uint64_t *size);
int rr_read_text_file(const char *path, char **out, size_t *len, size_t max); /* NUL-terminated, malloc */
int rr_write_text_file_atomic(const char *path, const char *data, size_t len);
int rr_rename_over(const char *from, const char *to);
int rr_remove_tree(const char *path);
int rr_copy_file_path(rr_ctx *c, const char *from, const char *to);
uint64_t rr_free_space(const char *path);                         /* bytes, or 0 if unknown */
/* Directory listing. On the PS5 build (RR_USE_GETDENTS) this reads the directory with the kernel's getdents instead of
 * the libc opendir/readdir. Returns NULL on failure with errno set; rr_readdir returns the next name or NULL. */
typedef struct rr_dir rr_dir;
rr_dir *rr_opendir(const char *path);
const char *rr_readdir(rr_dir *d);
void rr_closedir(rr_dir *d);
const char *rr_dir_diag(void); /* how the last rr_opendir listed the folder (for error messages) */
void rr_dir_debug(const char *path, char *out, size_t cap); /* raw first bytes of the listing, for diagnostics */
void *rr_xmalloc(size_t n);
void *rr_xrealloc(void *p, size_t n);
char *rr_xstrdup(const char *s);
char *rr_path_join(const char *a, const char *b);

static inline uint32_t rr_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint16_t rr_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint64_t rr_be64(const uint8_t *p) { return (uint64_t)rr_be32(p) << 32 | rr_be32(p + 4); }
static inline void rr_put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void rr_put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

#ifdef __cplusplus
}
#endif
#endif
