/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#ifndef MINIZ_NO_STDIO
#define MINIZ_NO_STDIO
#endif
#ifndef MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#endif
#ifndef MINIZ_NO_TIME
#define MINIZ_NO_TIME
#endif
#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "rr_zip.h"
#include "miniz.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int name_ok_impl(const char *name, int require_root)
{
    static const char root[] = "RetroRewind6";
    size_t n = strlen(name);
    if (n == 0 || n > 700) return 0;
    if (name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f) return 0;
    if (name[n - 1] == '/') n--; /* directory entries end in one slash */
    if (require_root && (n < sizeof root - 1 || strncmp(name, root, sizeof root - 1) || (n > sizeof root - 1 && name[sizeof root - 1] != '/'))) return 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && name[j] != '/') j++;
        size_t l = j - i;
        if (l == 0 || l > 255) return 0;
        if ((l == 1 && name[i] == '.') || (l == 2 && name[i] == '.' && name[i + 1] == '.')) return 0;
        i = j + 1;
    }
    return 1;
}

int rr_zip_name_ok(const char *name) { return name_ok_impl(name, 1); }

/* full-archive layout: how member names map into <stage>/RetroRewind6/ */
typedef struct { int mode; char strip[256]; } fullmap_t; /* mode 0 = strict (update archives), 1 = rooted/wrapped, 2 = bare */
/* returns the staged name for `name` (pointer into buf), or NULL when the member is not part of the pack */
static const char *fullmap(const fullmap_t *m, const char *name, char *buf, size_t cap)
{
    static const char rr[] = "RetroRewind6";
    if (!strncmp(name, "__MACOSX", 8) || strstr(name, ".DS_Store")) return NULL;
    if (m->mode == 0) return name;
    if (m->mode == 1) {
        size_t sl = strlen(m->strip);
        if (strncmp(name, m->strip, sl)) return NULL;
        name += sl;
        size_t l = strlen(name);
        if (l >= sizeof rr - 1 && !strncmp(name, rr, sizeof rr - 1) && (l == sizeof rr - 1 || name[sizeof rr - 1] == '/')) return name;
        return NULL;
    }
    if (snprintf(buf, cap, "%s/%s", rr, name) >= (int)cap) return NULL;
    return buf;
}

typedef struct { int fd; } rd_t;
static size_t zread(void *opaque, mz_uint64 ofs, void *buf, size_t n)
{
    rd_t *r = opaque;
    size_t got = 0;
    while (got < n) {
        ssize_t k = pread(r->fd, (char *)buf + got, n - got, (off_t)(ofs + got));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    return got;
}

typedef struct { int fd; uint64_t written; uint32_t crc; rr_ctx *c; int err; } wr_t;
static size_t zwrite(void *opaque, mz_uint64 ofs, const void *buf, size_t n)
{
    wr_t *w = opaque;
    (void)ofs;
    if (w->err) return 0;
    if (rr_cancelled(w->c)) { w->err = RR_E_CANCEL; return 0; }
    if (rr_write_full(w->fd, buf, n)) { w->err = RR_E_IO; return 0; }
    w->crc = (uint32_t)mz_crc32(w->crc, buf, n);
    w->written += n;
    return n;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int rr_zip_extract(rr_ctx *c, const char *zip_path, const char *stage_root, uint64_t max_total, uint64_t *total_out, size_t *files_out)
{
    return rr_zip_extract_ex(c, zip_path, stage_root, max_total, total_out, files_out, NULL);
}

static void list_add(const char *list_path, char kind, const char *name)
{
    if (!list_path) return;
    int fd = open(list_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char line[4200];
    int n = snprintf(line, sizeof line, "%c %s\n", kind, name);
    if (n > 0 && (size_t)n < sizeof line) (void)!write(fd, line, (size_t)n);
    close(fd);
}

static int extract_impl(rr_ctx *c, const char *zip_path, const char *stage_root, uint64_t max_total, uint64_t *total_out, size_t *files_out, const char *list_path, int full);

int rr_zip_extract_ex(rr_ctx *c, const char *zip_path, const char *stage_root, uint64_t max_total, uint64_t *total_out, size_t *files_out, const char *list_path)
{
    return extract_impl(c, zip_path, stage_root, max_total, total_out, files_out, list_path, 0);
}

int rr_zip_extract_full(rr_ctx *c, const char *zip_path, const char *stage_root, uint64_t max_total, uint64_t *total_out, size_t *files_out, const char *list_path)
{
    return extract_impl(c, zip_path, stage_root, max_total, total_out, files_out, list_path, 1);
}

static int extract_impl(rr_ctx *c, const char *zip_path, const char *stage_root, uint64_t max_total, uint64_t *total_out, size_t *files_out, const char *list_path, int full)
{
    int r = 0;
    fullmap_t fm; memset(&fm, 0, sizeof fm);
    char **names = NULL;
    rd_t rd; rd.fd = open(zip_path, O_RDONLY);
    if (rd.fd < 0) return rr_fail(c, RR_E_IO, "cannot open %s", zip_path);
    struct stat st;
    if (fstat(rd.fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 22) { close(rd.fd); return rr_fail(c, RR_E_CORRUPT, "download is not a ZIP file (%lld bytes)", (long long)st.st_size); }
    mz_zip_archive za;
    memset(&za, 0, sizeof za);
    za.m_pRead = zread; za.m_pIO_opaque = &rd;
    if (!mz_zip_reader_init(&za, (mz_uint64)st.st_size, 0)) {
        close(rd.fd);
        return rr_fail(c, RR_E_CORRUPT, "ZIP is damaged or incomplete (%s)", mz_zip_get_error_string(mz_zip_get_last_error(&za)));
    }
    mz_uint n = mz_zip_reader_get_num_files(&za);
    if (n == 0 || n > 50000) { r = rr_fail(c, RR_E_CORRUPT, "ZIP has %u entries", (unsigned)n); goto out; }
    /* full archives: decide the layout first (rooted, wrapped in one folder, or bare) */
    if (full) {
        fm.mode = 2;
        for (mz_uint i = 0; i < n && fm.mode == 2; i++) {
            mz_zip_archive_file_stat fs;
            if (!mz_zip_reader_file_stat(&za, i, &fs)) break;
            const char *nm = fs.m_filename;
            if (!strncmp(nm, "RetroRewind6/", 13) || !strcmp(nm, "RetroRewind6")) { fm.mode = 1; fm.strip[0] = 0; break; }
            const char *sl = strchr(nm, '/');
            if (sl && sl > nm && (size_t)(sl - nm) < sizeof fm.strip - 2 && !strncmp(sl + 1, "RetroRewind6/", 13) ) {
                fm.mode = 1; memcpy(fm.strip, nm, (size_t)(sl - nm) + 1); fm.strip[sl - nm + 1] = 0;
            }
        }
        rr_logf(c, RR_LOG_INFO, "Full archive layout: %s", fm.mode == 2 ? "files at archive root (placed into RetroRewind6)" : fm.strip[0] ? "RetroRewind6 inside a wrapper folder" : "RetroRewind6 at archive root");
    }
    names = rr_xmalloc(n * sizeof *names);
    mz_uint nn = 0;
    uint64_t total = 0, files = 0;
    /* pass 1: validate everything before touching the disk */
    for (mz_uint i = 0; i < n && !r; i++) {
        mz_zip_archive_file_stat fs;
        if (!mz_zip_reader_file_stat(&za, i, &fs)) { r = rr_fail(c, RR_E_CORRUPT, "ZIP directory entry %u unreadable", (unsigned)i); break; }
        if (!name_ok_impl(fs.m_filename, !full)) { r = rr_fail(c, RR_E_UNSAFE, "unsafe or unexpected path in ZIP: '%.120s'", fs.m_filename); break; }
        char mb[1024];
        const char *mapped = full ? fullmap(&fm, fs.m_filename, mb, sizeof mb) : fs.m_filename;
        if (!mapped) continue; /* not part of the pack */
        if (fs.m_is_encrypted) { r = rr_fail(c, RR_E_UNSUPPORTED, "encrypted ZIP entry '%.120s'", fs.m_filename); break; }
        if (!fs.m_is_supported) { r = rr_fail(c, RR_E_UNSUPPORTED, "unsupported compression in '%.120s'", fs.m_filename); break; }
        if (((fs.m_version_made_by >> 8) == 3) && ((fs.m_external_attr >> 16) & 0170000) == 0120000) { r = rr_fail(c, RR_E_UNSAFE, "symbolic link in ZIP: '%.120s'", fs.m_filename); break; }
        if (!fs.m_is_directory) {
            size_t l = strlen(fs.m_filename);
            if (fs.m_filename[l - 1] == '/') { r = rr_fail(c, RR_E_CORRUPT, "bad entry '%.120s'", fs.m_filename); break; }
            total += fs.m_uncomp_size; files++;
            if (total > max_total) { r = rr_fail(c, RR_E_NOSPACE, "ZIP expands to more than %llu MiB", (unsigned long long)(max_total >> 20)); break; }
        }
        char *nm = rr_xstrdup(mapped);
        size_t l = strlen(nm); if (l && nm[l - 1] == '/') nm[l - 1] = 0;
        names[nn++] = nm;
    }
    if (!r) {
        qsort(names, nn, sizeof *names, cmpstr);
        for (mz_uint i = 1; i < nn; i++) if (!strcmp(names[i], names[i - 1])) { r = rr_fail(c, RR_E_UNSAFE, "duplicate entry in ZIP: '%.120s'", names[i]); break; }
    }
    for (mz_uint i = 0; i < nn; i++) free(names[i]);
    free(names);
    if (r) goto out;
    if (total > max_total) { r = rr_fail(c, RR_E_NOSPACE, "ZIP too large"); goto out; }
    /* pass 2: extract with CRC verification */
    uint64_t done = 0;
    for (mz_uint i = 0; i < n && !r; i++) {
        mz_zip_archive_file_stat fs;
        mz_zip_reader_file_stat(&za, i, &fs);
        char mb[1024];
        const char *mapped = full ? fullmap(&fm, fs.m_filename, mb, sizeof mb) : fs.m_filename;
        if (!mapped) continue;
        char path[4096];
        if (snprintf(path, sizeof path, "%s/%s", stage_root, mapped) >= (int)sizeof path) { r = rr_fail(c, RR_E_UNSAFE, "path too long"); break; }
        if (fs.m_is_directory) {
            size_t l = strlen(path); while (l && path[l - 1] == '/') path[--l] = 0;
            if (rr_mkdir_p(path)) r = rr_fail(c, RR_E_IO, "cannot create %s", path);
            else list_add(list_path, 'D', mapped);
            continue;
        }
        char *sl = strrchr(path, '/'); *sl = 0;
        if (rr_mkdir_p(path)) { r = rr_fail(c, RR_E_IO, "cannot create %s", path); break; }
        *sl = '/';
        wr_t w = {.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644), .written = 0, .crc = 0, .c = c, .err = 0};
        if (w.fd < 0) { r = rr_fail(c, RR_E_IO, "cannot create %s: %s", path, strerror(errno)); break; }
        list_add(list_path, 'F', mapped); /* listed before writing so a failed run can still be cleaned up by name */
        mz_bool ok = mz_zip_reader_extract_to_callback(&za, i, zwrite, &w, 0);
        int fr = fsync(w.fd);
        close(w.fd);
        if (w.err == RR_E_CANCEL) { r = rr_fail(c, RR_E_CANCEL, "cancelled"); break; }
        if (w.err) { r = rr_fail(c, RR_E_IO, "write error extracting %.120s", fs.m_filename); break; }
        if (!ok) { r = rr_fail(c, RR_E_CORRUPT, "ZIP entry '%.120s' is damaged (%s)", fs.m_filename, mz_zip_get_error_string(mz_zip_get_last_error(&za))); break; }
        if (w.written != fs.m_uncomp_size || w.crc != (uint32_t)fs.m_crc32) { r = rr_fail(c, RR_E_CORRUPT, "ZIP entry '%.120s' failed its CRC-32 check", fs.m_filename); break; }
        if (fr) { r = rr_fail(c, RR_E_IO, "fsync failed"); break; }
        done += fs.m_uncomp_size;
        rr_progress(c, "Extracting update", done, total);
    }
    if (!r) { if (total_out) *total_out = total; if (files_out) *files_out = (size_t)files; }
out:
    mz_zip_reader_end(&za);
    close(rd.fd);
    return r;
}
