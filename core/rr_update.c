/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "rr_update.h"
#include "rr_zip.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ versions */

static int ver_ok(const char *v)
{
    size_t n = strlen(v);
    if (!n || n > 31 || !isdigit((unsigned char)v[0]) || !isdigit((unsigned char)v[n - 1])) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!(isdigit((unsigned char)v[i]) || v[i] == '.')) return 0;
        if (v[i] == '.' && v[i + 1] == '.') return 0;
    }
    return 1;
}

int rr_version_cmp(const char *a, const char *b, int *ok)
{
    if (!ver_ok(a) || !ver_ok(b)) { if (ok) *ok = 0; return 0; }
    if (ok) *ok = 1;
    for (;;) {
        unsigned long x = 0, y = 0;
        while (isdigit((unsigned char)*a)) x = x * 10 + (unsigned long)(*a++ - '0');
        while (isdigit((unsigned char)*b)) y = y * 10 + (unsigned long)(*b++ - '0');
        if (x != y) return x < y ? -1 : 1;
        if (!*a && !*b) return 0;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
}

int rr_read_installed_version(rr_ctx *c, const char *pack_dir, char out[32])
{
    char path[PATH_MAX], *txt; size_t len;
    snprintf(path, sizeof path, "%s/RetroRewind6/version.txt", pack_dir);
    if (rr_path_kind(path) != 1) return rr_fail(c, RR_E_STATE, "Retro Rewind is not installed here (%s is missing)", path);
    if (rr_read_text_file(path, &txt, &len, 256)) return rr_fail(c, RR_E_IO, "cannot read %s", path);
    char *s = txt; while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    int ok = ver_ok(s);
    if (ok) snprintf(out, 32, "%s", s);
    free(txt);
    if (!ok) return rr_fail(c, RR_E_STATE, "the installed version file is not a version number");
    return 0;
}

/* ------------------------------------------------------------------ manifest */

static int host_allowed(const char *url)
{
    if (strncmp(url, "https://", 8)) return 0;
    const char *h = url + 8;
    size_t hl = strcspn(h, "/:?#");
    if (!hl || hl > 100 || strchr(url, '@') ) return 0;
    char host[128]; memcpy(host, h, hl); host[hl] = 0;
    for (size_t i = 0; i < hl; i++) host[i] = (char)tolower((unsigned char)host[i]);
    if (!strcmp(host, "rwfc.net")) return 1;
    size_t n = hl;
    return n > 9 && !strcmp(host + n - 9, ".rwfc.net");
}

int rr_manifest_parse(rr_ctx *c, const char *text, rr_manifest *m)
{
    memset(m, 0, sizeof *m);
    size_t cap = 0, lineno = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        lineno++;
        char *line = rr_xmalloc(l + 1); memcpy(line, p, l); line[l] = 0;
        p += l + (e ? 1 : 0);
        char *s = line; while (isspace((unsigned char)*s)) s++;
        char *t = s + strlen(s); while (t > s && isspace((unsigned char)t[-1])) *--t = 0;
        if (!*s) { free(line); continue; }
        char *f[4] = {0}; int nf = 0;
        char *q = s;
        while (*q && nf < 4) {
            f[nf++] = q;
            if (nf == 4) break;
            while (*q && !isspace((unsigned char)*q)) q++;
            if (!*q) break;
            *q++ = 0;
            while (isspace((unsigned char)*q)) q++;
        }
        if (nf < 3 || strlen(f[0]) > 31 || strlen(f[1]) > 511 || strlen(f[2]) > 159 || (nf > 3 && strlen(f[3]) > 63)) {
            free(line); rr_manifest_free(m);
            return rr_fail(c, RR_E_FORMAT, "version list line %zu is malformed", lineno);
        }
        if (!ver_ok(f[0]) || !host_allowed(f[1]) || f[2][0] != '/') {
            int hostbad = ver_ok(f[0]) && !host_allowed(f[1]);
            char tmp[64]; snprintf(tmp, sizeof tmp, "%.60s", f[0]);
            free(line); rr_manifest_free(m);
            return rr_fail(c, hostbad ? RR_E_UNSAFE : RR_E_FORMAT, hostbad ? "version list line %zu points outside the official update servers" : "version list line %zu is invalid", lineno);
        }
        if (m->n == cap) { cap = cap ? cap * 2 : 64; m->e = rr_xrealloc(m->e, cap * sizeof *m->e); if (cap > 4096) { free(line); rr_manifest_free(m); return rr_fail(c, RR_E_FORMAT, "version list is too long"); } }
        rr_manifest_entry *en = &m->e[m->n];
        memset(en, 0, sizeof *en);
        snprintf(en->version, sizeof en->version, "%s", f[0]);
        snprintf(en->url, sizeof en->url, "%s", f[1]);
        snprintf(en->path, sizeof en->path, "%s", f[2]);
        if (nf > 3) snprintf(en->desc, sizeof en->desc, "%s", f[3]);
        if (m->n) {
            int ok; int cmp = rr_version_cmp(m->e[m->n - 1].version, en->version, &ok);
            if (cmp > 0) { free(line); rr_manifest_free(m); return rr_fail(c, RR_E_FORMAT, "version list is not in ascending order (line %zu)", lineno); }
        }
        m->n++;
        free(line);
    }
    if (!m->n) return rr_fail(c, RR_E_FORMAT, "the version list is empty");
    return 0;
}

void rr_manifest_free(rr_manifest *m) { free(m->e); m->e = NULL; m->n = 0; }
void rr_update_state_free(rr_update_state *s) { rr_manifest_free(&s->m); }

/* ------------------------------------------------------------------ helpers */

typedef struct { char pack[PATH_MAX], stage[PATH_MAX], dl[PATH_MAX], backup[PATH_MAX], journal[PATH_MAX], rr[PATH_MAX]; char list[PATH_MAX]; } paths_t;

static void mkpaths(paths_t *p, const char *pack_dir)
{
    snprintf(p->pack, sizeof p->pack, "%s", pack_dir);
    snprintf(p->stage, sizeof p->stage, "%s/.rr-stage", pack_dir);
    snprintf(p->dl, sizeof p->dl, "%s/.rr-dl", pack_dir);
    snprintf(p->backup, sizeof p->backup, "%s/.rr-backup", pack_dir);
    snprintf(p->journal, sizeof p->journal, "%s/.rr-journal", pack_dir);
    snprintf(p->rr, sizeof p->rr, "%s/RetroRewind6", pack_dir);
    snprintf(p->list, sizeof p->list, "%s/.rr-stage.list", pack_dir);
}

/* ------------------------------------------------------------------ journal / rollback */

typedef struct { char kind; char *rel; } jent;

static int journal_load(const char *path, jent **out, size_t *n)
{
    char *txt; size_t len;
    *out = NULL; *n = 0;
    if (rr_read_text_file(path, &txt, &len, 64u << 20)) return RR_E_IO;
    size_t cap = 0;
    char *p = txt;
    int first = 1;
    while (*p) {
        char *e = strchr(p, '\n'); if (e) *e = 0;
        if (first) { if (strcmp(p, "RRJ1")) { free(txt); return RR_E_FORMAT; } first = 0; }
        else if ((p[0] == 'M' || p[0] == 'A') && p[1] == ' ' && p[2]) {
            if (*n == cap) { cap = cap ? cap * 2 : 256; *out = rr_xrealloc(*out, cap * sizeof **out); }
            (*out)[*n].kind = p[0]; (*out)[*n].rel = rr_xstrdup(p + 2); (*n)++;
        }
        if (!e) break;
        p = e + 1;
    }
    free(txt);
    return 0;
}

static void journal_free(jent *j, size_t n) { for (size_t i = 0; i < n; i++) free(j[i].rel); free(j); }

static int rel_safe(const char *r)
{
    if (!*r || r[0] == '/') return 0;
    const char *s = r;
    for (;;) {
        const char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (!l || (l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return 0;
        if (!e) return 1;
        s = e + 1;
    }
}

static void rollback(const paths_t *p, jent *j, size_t n)
{
    for (size_t i = n; i-- > 0;) {
        if (!rel_safe(j[i].rel)) continue;
        char dest[PATH_MAX], bk[PATH_MAX];
        snprintf(dest, sizeof dest, "%s/%s", p->rr, j[i].rel);
        snprintf(bk, sizeof bk, "%s/%s", p->backup, j[i].rel);
        rr_fileinfo st;
        if (rr_stat(bk, 0, &st) == 0) {
            rename(bk, dest);
            for (char *sl = strrchr(bk, '/'); sl && (size_t)(sl - bk) > strlen(p->backup); sl = strrchr(bk, '/')) { *sl = 0; if (rmdir(bk) != 0) break; }
        }
        else if (j[i].kind == 'A') unlink(dest);
    }
}

int rr_update_recover(rr_ctx *c, const char *pack_dir)
{
    paths_t p; mkpaths(&p, pack_dir);
    if (rr_path_kind(p.journal) == 1) {
        jent *j; size_t n;
        int r = journal_load(p.journal, &j, &n);
        if (r) return rr_fail(c, RR_E_STATE, "an interrupted update left an unreadable journal; the pack may be inconsistent (%s)", p.journal);
        rr_logf(c, RR_LOG_WARN, "Rolling back an interrupted update (%zu files)", n);
        rollback(&p, j, n);
        journal_free(j, n);
        unlink(p.journal);
    }
    rr_remove_tree(p.stage); rr_remove_tree(p.dl); rr_remove_tree(p.backup);
    rmdir(p.backup); rmdir(p.stage); rmdir(p.dl);
    unlink(p.list);
    return 0;
}

/* ------------------------------------------------------------------ staging walk + merge */

typedef struct { char **v; size_t n, cap; } strvec;
static void sv_add(strvec *s, const char *x) { if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->v = rr_xrealloc(s->v, s->cap * sizeof *s->v); } s->v[s->n++] = rr_xstrdup(x); }
static void sv_free(strvec *s) { for (size_t i = 0; i < s->n; i++) free(s->v[i]); free(s->v); memset(s, 0, sizeof *s); }
static int sv_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int walk(rr_ctx *c, const char *root, const char *rel, strvec *files, strvec *dirs)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", root, rel);
    rr_dir *d = rr_opendir(dir);
    if (!d) return rr_fail(c, RR_E_IO, "cannot read %s", dir);
    int r = 0;
    const char *ename;
    while (!r && (ename = rr_readdir(d))) {
        if (!strcmp(ename, ".") || !strcmp(ename, "..")) continue;
        char relc[PATH_MAX], full[PATH_MAX];
        if (snprintf(relc, sizeof relc, "%s/%s", rel, ename) >= (int)sizeof relc || snprintf(full, sizeof full, "%s/%s", root, relc) >= (int)sizeof full) { r = rr_fail(c, RR_E_UNSAFE, "path too long"); break; }
        rr_fileinfo st;
        if (rr_stat(full, 0, &st)) { r = rr_fail(c, RR_E_IO, "cannot stat %s: %s", full, strerror(errno)); break; }
        if (st.kind == 2) { sv_add(dirs, relc); r = walk(c, root, relc, files, dirs); }
        else if (st.kind == 1) sv_add(files, relc);
        else r = rr_fail(c, RR_E_UNSAFE, "unexpected special file in update: %s", relc);
    }
    rr_closedir(d);
    return r;
}

static int parents_ok(rr_ctx *c, const char *rr_root, const char *rel)
{
    char path[PATH_MAX];
    size_t n = strlen(rel);
    for (size_t i = 0; i <= n; i++) {
        if (rel[i] != '/' && rel[i] != 0) continue;
        snprintf(path, sizeof path, "%s/%.*s", rr_root, (int)i, rel);
        rr_fileinfo st;
        if (rr_stat(path, 0, &st)) { if (errno == ENOENT || errno == ENOTDIR) return 0; return rr_fail(c, RR_E_IO, "cannot stat %s: %s", path, strerror(errno)); }
        int isfinal = rel[i] == 0;
        if (st.kind == 3) return rr_fail(c, RR_E_UNSAFE, "refusing to update through symbolic link %s", path);
        if (!isfinal && st.kind != 2) return rr_fail(c, RR_E_STATE, "%s is a file but the update needs a folder there", path);
    }
    return 0;
}

int rr_merge_stage(rr_ctx *c, const char *pack_dir, const char *stage_root, const char *version)
{
    return rr_merge_stage_ex(c, pack_dir, stage_root, version, 0, NULL);
}

/* Reads the extraction list written by rr_zip_extract_ex: the archive's own file names, so no folder listing is needed. */
static int load_list(rr_ctx *c, const char *list_path, strvec *files, strvec *dirs)
{
    char *txt; size_t len;
    if (rr_read_text_file(list_path, &txt, &len, 64u << 20)) return rr_fail(c, RR_E_IO, "cannot read the extraction list");
    int r = 0;
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] != 'F' && line[0] != 'D') continue;
        const char *nm = line + 2;
        if (strncmp(nm, "RetroRewind6/", 13)) { if (!strcmp(nm, "RetroRewind6") || !strcmp(nm, "RetroRewind6/")) continue; r = rr_fail(c, RR_E_UNSAFE, "unexpected path in extraction list"); break; }
        char rel[PATH_MAX];
        snprintf(rel, sizeof rel, "%s", nm + 13);
        size_t l = strlen(rel);
        while (l && rel[l - 1] == '/') rel[--l] = 0;
        if (!l) continue;
        sv_add(line[0] == 'F' ? files : dirs, rel);
    }
    free(txt);
    if (!r && files->n) {
        qsort(files->v, files->n, sizeof *files->v, sv_cmp);
        size_t w = 1; /* unique */
        for (size_t i = 1; i < files->n; i++) { if (strcmp(files->v[i], files->v[w - 1])) files->v[w++] = files->v[i]; else free(files->v[i]); }
        files->n = w;
        int has_ver = 0; for (size_t i = 0; i < files->n; i++) if (!strcmp(files->v[i], "version.txt")) has_ver = 1;
        if (!has_ver) sv_add(files, "version.txt");
    }
    return r;
}

static int load_names(rr_ctx *c, const char *list_path, strvec *files, strvec *dirs)
{
    char *txt; size_t len;
    if (rr_read_text_file(list_path, &txt, &len, 64u << 20)) return rr_fail(c, RR_E_IO, "cannot read the extraction list");
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] != 'F' && line[0] != 'D') continue;
        char rel[PATH_MAX];
        snprintf(rel, sizeof rel, "%s", line + 2);
        size_t l = strlen(rel);
        while (l && rel[l - 1] == '/') rel[--l] = 0;
        if (!l) continue;
        sv_add(line[0] == 'F' ? files : dirs, rel);
    }
    free(txt);
    qsort(files->v, files->n, sizeof *files->v, sv_cmp);
    size_t w = files->n ? 1 : 0;
    for (size_t i = 1; i < files->n; i++) { if (strcmp(files->v[i], files->v[w - 1])) files->v[w++] = files->v[i]; else free(files->v[i]); }
    files->n = w;
    return 0;
}

/* Removes the named files under root and then the folders that held them (deepest first, only if empty).
 * Needs no folder listing, so it works even where listing a folder does not. */
static void rm_named(const char *root, char *const *names, const char *kinds, size_t n, char want)
{
    for (size_t i = 0; i < n; i++) {
        if (kinds && kinds[i] != want) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", root, names[i]);
        for (char *q = path; *q; ) { if (q[0] == '/' && q[1] == '/') memmove(q, q + 1, strlen(q)); else q++; } /* "dir//.d" from listed folder entries */
        unlink(path);
        for (char *sl = strrchr(path, '/'); sl && (size_t)(sl - path) > strlen(root); sl = strrchr(path, '/')) { *sl = 0; if (rmdir(path) != 0) break; }
    }
}

int rr_merge_stage_ex(rr_ctx *c, const char *pack_dir, const char *stage_root, const char *version, size_t expected_files, const char *list_path)
{
    paths_t p; mkpaths(&p, pack_dir);
    char srr[PATH_MAX];
    snprintf(srr, sizeof srr, "%s/RetroRewind6", stage_root);
    if (rr_path_kind(srr) != 2) return rr_fail(c, RR_E_FORMAT, "the update archive does not contain a RetroRewind6 folder");
    strvec files = {0}, dirs = {0};
    int r = 0;
    if (list_path && rr_path_kind(list_path) == 1) r = load_list(c, list_path, &files, &dirs);
    else {
        r = walk(c, srr, "", &files, &dirs);
        /* walk() produced "/x/y" style (leading slash); normalise */
        for (size_t i = 0; i < files.n && !r; i++) { char *t = rr_xstrdup(files.v[i] + 1); free(files.v[i]); files.v[i] = t; }
        for (size_t i = 0; i < dirs.n && !r; i++) { char *t = rr_xstrdup(dirs.v[i] + 1); free(dirs.v[i]); dirs.v[i] = t; }
    }
    if (r) goto out;
    if (!files.n) { r = rr_fail(c, RR_E_FORMAT, "the update archive contains no files"); goto out; }
    if (expected_files && files.n < expected_files) { r = rr_fail(c, RR_E_STATE, "only %zu of %zu extracted files are visible in the working folder; nothing was changed", files.n, expected_files); goto out; }
    qsort(files.v, files.n, sizeof *files.v, sv_cmp);
    /* version.txt is always written last so an interrupted run still reads as the old version */
    {
        size_t k = 0;
        for (size_t i = 0; i < files.n; i++) if (!strcmp(files.v[i], "version.txt")) { char *t = files.v[i]; memmove(files.v + i, files.v + i + 1, (files.n - i - 1) * sizeof *files.v); files.v[files.n - 1] = t; k = 1; break; }
        (void)k;
    }
    /* plan + validate before changing anything */
    char *kinds = rr_xmalloc(files.n);
    for (size_t i = 0; i < files.n; i++) {
        if (!rel_safe(files.v[i])) { free(kinds); r = rr_fail(c, RR_E_UNSAFE, "unsafe path in update"); goto out; }
        char dest[PATH_MAX]; rr_fileinfo st;
        snprintf(dest, sizeof dest, "%s/%s", p.rr, files.v[i]);
        int pr = parents_ok(c, p.rr, files.v[i]);
        if (pr < 0) { free(kinds); r = pr; goto out; }
        { char srcp[PATH_MAX]; snprintf(srcp, sizeof srcp, "%s/%s", srr, files.v[i]);
          if (rr_path_kind(srcp) != 1) { free(kinds); r = rr_fail(c, RR_E_STATE, "extracted file %s is missing from the working folder; nothing was changed", files.v[i]); goto out; } }
        if (rr_stat(dest, 0, &st) == 0) {
            if (st.kind != 1) { free(kinds); r = rr_fail(c, RR_E_STATE, "%s exists but is not a normal file", dest); goto out; }
            kinds[i] = 'M';
        } else kinds[i] = 'A';
    }
    /* journal first */
    {
        size_t cap = 16; for (size_t i = 0; i < files.n; i++) cap += strlen(files.v[i]) + 4;
        char *jt = rr_xmalloc(cap), *w = jt;
        w += sprintf(w, "RRJ1\n");
        for (size_t i = 0; i < files.n; i++) w += sprintf(w, "%c %s\n", kinds[i], files.v[i]);
        int jr = rr_write_text_file_atomic(p.journal, jt, (size_t)(w - jt));
        free(jt);
        if (jr) { free(kinds); r = rr_fail(c, RR_E_IO, "cannot write the update journal"); goto out; }
    }
    rr_remove_tree(p.backup);
    for (size_t i = 0; i < dirs.n; i++) { char d[PATH_MAX]; snprintf(d, sizeof d, "%s/%s", p.rr, dirs.v[i]); rr_mkdir_p(d); }
    for (size_t i = 0; i < files.n && !r; i++) {
        char dest[PATH_MAX], src[PATH_MAX], bk[PATH_MAX];
        snprintf(dest, sizeof dest, "%s/%s", p.rr, files.v[i]);
        snprintf(src, sizeof src, "%s/%s", srr, files.v[i]);
        snprintf(bk, sizeof bk, "%s/%s", p.backup, files.v[i]);
        char *sl = strrchr(dest, '/'); *sl = 0; int mr = rr_mkdir_p(dest); *sl = '/';
        if (mr) { r = rr_fail(c, RR_E_IO, "cannot create folder for %s", dest); break; }
        if (kinds[i] == 'M') {
            char *sb = strrchr(bk, '/'); *sb = 0; mr = rr_mkdir_p(bk); *sb = '/';
            if (mr || rename(dest, bk)) { r = rr_fail(c, RR_E_IO, "cannot back up %s: %s", dest, strerror(errno)); break; }
        }
        if (rename(src, dest)) { r = rr_fail(c, RR_E_IO, "cannot install %s: %s", dest, strerror(errno)); break; }
    }
    if (r) {
        free(kinds);
        jent *j; size_t jn;
        if (journal_load(p.journal, &j, &jn) == 0) { rollback(&p, j, jn); journal_free(j, jn); }
        unlink(p.journal);
        goto out;
    }
    unlink(p.journal); /* commit point */
    rr_remove_tree(p.backup);
    rm_named(p.backup, files.v, kinds, files.n, 'M'); /* also works when folders cannot be listed */
    rmdir(p.backup);
    free(kinds);
    rr_logf(c, RR_LOG_INFO, "Merged %zu files into %s (matching files replaced, new files added, others kept)", files.n, p.rr);
    (void)version;
out:
    sv_free(&files); sv_free(&dirs);
    return r;
}

/* ------------------------------------------------------------------ check + apply */

static int http_text(rr_ctx *c, const rr_http *h, const char *url, char **out)
{
    uint8_t *mem = NULL; size_t len = 0; int status = 0; uint64_t exp = 0;
    int r = h->get(h->user, c, url, NULL, &mem, &len, 65536, &exp, &status);
    if (r) { free(mem); return r; }
    if (status != 200) { free(mem); return rr_fail(c, RR_E_NET, "the update server answered HTTP %d", status); }
    if (exp && exp != len) { free(mem); return rr_fail(c, RR_E_NET, "the version list was cut off (%zu of %llu bytes)", len, (unsigned long long)exp); }
    *out = (char *)mem;
    return 0;
}

int rr_update_check_ex(rr_ctx *c, const rr_http *h, const char *pack_dir, rr_update_state *s, int latest_only)
{
    memset(s, 0, sizeof *s);
    int r = rr_update_recover(c, pack_dir);
    if (r) return r;
    if ((r = rr_read_installed_version(c, pack_dir, s->installed))) return r;
    char *txt;
    if ((r = http_text(c, h, RR_VERSION_URL, &txt))) return r;
    r = rr_manifest_parse(c, txt, &s->m);
    free(txt);
    if (r) return r;
    snprintf(s->latest, sizeof s->latest, "%s", s->m.e[s->m.n - 1].version);
    int ok;
    if (!latest_only && rr_version_cmp(s->installed, s->m.e[0].version, &ok) < 0)
        return rr_fail(c, RR_E_STATE, "installed version %s is older than the oldest listed update (%s); a full reinstall is needed", s->installed, s->m.e[0].version);
    s->first_pending = s->m.n;
    for (size_t i = 0; i < s->m.n; i++) if (rr_version_cmp(s->m.e[i].version, s->installed, &ok) > 0) { s->first_pending = i; break; }
    if (latest_only && s->first_pending < s->m.n) {
        /* only the archives of the newest release; older releases in between are not downloaded */
        size_t k = s->m.n - 1;
        while (k > s->first_pending && !strcmp(s->m.e[k - 1].version, s->latest)) k--;
        s->first_pending = k;
    }
    s->n_pending = s->m.n - s->first_pending;
    for (size_t i = s->first_pending; i < s->m.n; i++) if (i == s->first_pending || strcmp(s->m.e[i].version, s->m.e[i - 1].version)) s->n_versions++;
    s->up_to_date = s->n_pending == 0;
    return 0;
}

int rr_update_check(rr_ctx *c, const rr_http *h, const char *pack_dir, rr_update_state *s)
{
    return rr_update_check_ex(c, h, pack_dir, s, 0);
}

/* Gives an empty directory at path. A leftover from an earlier failed run must never leak into this update: if it cannot
 * be deleted it is moved aside instead. */
static int fresh_dir(rr_ctx *c, const char *path)
{
    rr_remove_tree(path);
    if (rr_path_kind(path) != 0) {
        int moved = 0;
        for (int n = 0; n < 100 && !moved; n++) {
            char aside[PATH_MAX];
            snprintf(aside, sizeof aside, "%s.old%d", path, n);
            if (rr_path_kind(aside) == 0 && rename(path, aside) == 0) moved = 1;
        }
        if (!moved) return rr_fail(c, RR_E_IO, "cannot clear the leftover folder %s: %s", path, strerror(errno));
        rr_logf(c, RR_LOG_WARN, "moved a leftover folder aside: %s", path);
    }
    return rr_mkdir_p(path);
}

static int apply_version(rr_ctx *c, const rr_http *h, const paths_t *p, const rr_update_state *s, size_t a, size_t b)
{
    const char *ver = s->m.e[a].version;
    rr_logf(c, RR_LOG_INFO, "Updating to %s (%zu archive%s)", ver, b - a, b - a == 1 ? "" : "s");
    unlink(p->list);
    if (fresh_dir(c, p->stage) || fresh_dir(c, p->dl)) return rr_fail(c, RR_E_IO, "cannot create working folders in %s: %s (the app has no write permission there; see the Access row)", p->pack, strerror(errno));
    int r = 0;
    size_t extracted = 0;
    for (size_t i = a; i < b && !r; i++) {
        char zp[PATH_MAX], part[PATH_MAX];
        snprintf(zp, sizeof zp, "%s/%zu.zip", p->dl, i - a);
        snprintf(part, sizeof part, "%s.part", zp);
        uint8_t *mem = NULL; size_t len = 0; int status = 0; uint64_t exp = 0;
        rr_logf(c, RR_LOG_INFO, "Downloading %s", s->m.e[i].url);
        r = h->get(h->user, c, s->m.e[i].url, part, &mem, &len, 0, &exp, &status);
        free(mem);
        if (r) break;
        if (status != 200) { r = rr_fail(c, RR_E_NET, "download of %s failed: HTTP %d", s->m.e[i].path, status); break; }
        uint64_t sz = 0;
        if (rr_file_size(part, &sz) || sz != len || (exp && exp != sz) || sz < 22) { r = rr_fail(c, RR_E_CORRUPT, "download of %s is incomplete (%llu of %llu bytes)", s->m.e[i].path, (unsigned long long)sz, (unsigned long long)exp); break; }
        if (rename(part, zp)) { r = rr_fail(c, RR_E_IO, "cannot finish download"); break; }
        uint64_t freeb = rr_free_space(p->pack);
        uint64_t maxt = freeb > (256ull << 20) ? freeb - (192ull << 20) : (64ull << 20);
        if (!freeb) maxt = 12ull << 30;
        uint64_t total = 0; size_t nfiles = 0;
        if ((r = rr_zip_extract_ex(c, zp, p->stage, maxt, &total, &nfiles, p->list))) break;
        extracted += nfiles;
        rr_logf(c, RR_LOG_INFO, "Archive extracted and verified: %zu files, %llu KiB", nfiles, (unsigned long long)(total >> 10));
        unlink(zp);
    }
    if (!r) {
        /* the official archives do not always carry a matching version.txt; the manifest entry is the authority */
        char vt[PATH_MAX], line[64];
        snprintf(vt, sizeof vt, "%s/RetroRewind6/version.txt", p->stage);
        int n = snprintf(line, sizeof line, "%s\n", ver);
        char *old; size_t ol;
        if (rr_path_kind(vt) == 1 && rr_read_text_file(vt, &old, &ol, 256) == 0) {
            char *t = old; while (isspace((unsigned char)*t)) t++;
            char *e = t + strlen(t); while (e > t && isspace((unsigned char)e[-1])) *--e = 0;
            if (strcmp(t, ver)) rr_logf(c, RR_LOG_WARN, "archive says version %s, list says %s; using the list", t, ver);
            free(old);
        }
        char sd[PATH_MAX]; snprintf(sd, sizeof sd, "%s/RetroRewind6", p->stage);
        if (rr_path_kind(sd) != 2) r = rr_fail(c, RR_E_FORMAT, "the update archive for %s has no RetroRewind6 folder", ver);
        else if (rr_write_text_file_atomic(vt, line, (size_t)n)) r = rr_fail(c, RR_E_IO, "cannot write version file");
    }
    if (!r) {
        if (rr_cancelled(c)) r = rr_fail(c, RR_E_CANCEL, "cancelled");
        else r = rr_merge_stage_ex(c, p->pack, p->stage, ver, extracted, p->list);
    }
    {   /* clean up by name as well, for systems where a folder cannot be listed */
        char *txt; size_t len;
        if (rr_path_kind(p->list) == 1 && rr_read_text_file(p->list, &txt, &len, 64u << 20) == 0) {
            for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
                if (line[0] != 'F' && line[0] != 'D') continue;
                char nmbuf[PATH_MAX];
                snprintf(nmbuf, sizeof nmbuf, "%s%s", line + 2, line[0] == 'D' ? "/.d" : "");
                char *nm[1] = { nmbuf };
                rm_named(p->stage, nm, NULL, 1, 0);
            }
            free(txt);
        }
        { char *vn[1] = { (char *)"RetroRewind6/version.txt" }; rm_named(p->stage, vn, NULL, 1, 0); }
        for (size_t i = a; i < b; i++) {
            char zp[PATH_MAX];
            snprintf(zp, sizeof zp, "%s/%zu.zip", p->dl, i - a); unlink(zp);
            snprintf(zp, sizeof zp, "%s/%zu.zip.part", p->dl, i - a); unlink(zp);
        }
        rmdir(p->dl);
        rmdir(p->stage);
        unlink(p->list);
    }
    rr_remove_tree(p->stage); rr_remove_tree(p->dl);
    return r;
}

/* logs how many files each top-level folder of the pack contains, so a pack that lacks a folder can be diagnosed from the log */
static void log_top_folders(rr_ctx *c, const char *list_path)
{
    char *txt; size_t len;
    if (rr_read_text_file(list_path, &txt, &len, 256u << 20)) return;
    char names[24][48]; size_t cnt[24]; unsigned n = 0;
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] != 'F' || strncmp(line + 2, "RetroRewind6/", 13)) continue;
        const char *rest = line + 15;
        const char *sl = strchr(rest, '/');
        char top[48]; size_t l = sl ? (size_t)(sl - rest) : 0;
        if (!sl) snprintf(top, sizeof top, "(files in root)"); else { if (l > 47) l = 47; memcpy(top, rest, l); top[l] = 0; }
        unsigned k = 0; while (k < n && strcmp(names[k], top)) k++;
        if (k == n) { if (n == 24) continue; snprintf(names[n], 48, "%s", top); cnt[n++] = 0; }
        cnt[k]++;
    }
    free(txt);
    char buf[700]; size_t w = 0; buf[0] = 0;
    for (unsigned k = 0; k < n && w < sizeof buf - 80; k++) w += (size_t)snprintf(buf + w, sizeof buf - w, "%s%s=%zu", k ? ", " : "", names[k], cnt[k]);
    rr_logf(c, RR_LOG_INFO, "Pack folders in the archive (files): %s", buf);
}

int rr_install_full(rr_ctx *c, const rr_http *h, const char *pack_dir, const char *url)
{
    paths_t p; mkpaths(&p, pack_dir);
    if (!host_allowed(url)) return rr_fail(c, RR_E_UNSAFE, "refusing to download from %s (only https on rwfc.net is allowed)", url);
    int r = rr_mkdir_p(pack_dir);
    if (r) return rr_fail(c, RR_E_IO, "cannot create %s: %s (the app has no write permission there; see the Access row)", pack_dir, strerror(errno));
    if ((r = rr_update_recover(c, pack_dir))) return r;
    unlink(p.list);
    if (fresh_dir(c, p.stage) || fresh_dir(c, p.dl)) return rr_fail(c, RR_E_IO, "cannot create working folders in %s: %s (the app has no write permission there; see the Access row)", p.pack, strerror(errno));
    char zp[PATH_MAX], part[PATH_MAX];
    snprintf(zp, sizeof zp, "%s/full.zip", p.dl);
    snprintf(part, sizeof part, "%s.part", zp);
    size_t extracted = 0;
    char ver[32] = "";
    do {
        uint8_t *mem = NULL; size_t len = 0; int status = 0; uint64_t exp = 0;
        rr_logf(c, RR_LOG_INFO, "Downloading the full Retro Rewind pack: %s", url);
        r = h->get(h->user, c, url, part, &mem, &len, 0, &exp, &status);
        free(mem);
        if (r) break;
        if (status != 200) { r = rr_fail(c, RR_E_NET, "download of the full pack failed: HTTP %d", status); break; }
        uint64_t sz = 0;
        if (rr_file_size(part, &sz) || sz != len || (exp && exp != sz) || sz < 22) { r = rr_fail(c, RR_E_CORRUPT, "download of the full pack is incomplete (%llu of %llu bytes)", (unsigned long long)sz, (unsigned long long)exp); break; }
        if (rename(part, zp)) { r = rr_fail(c, RR_E_IO, "cannot finish download"); break; }
        rr_logf(c, RR_LOG_INFO, "Downloaded %llu MiB", (unsigned long long)(sz >> 20));
        uint64_t freeb = rr_free_space(p.pack);
        uint64_t maxt = freeb > (sz + (256ull << 20)) ? freeb - sz - (192ull << 20) : (64ull << 20);
        if (!freeb) maxt = 12ull << 30;
        uint64_t total = 0; size_t nfiles = 0;
        if ((r = rr_zip_extract_full(c, zp, p.stage, maxt, &total, &nfiles, p.list))) break;
        extracted = nfiles;
        rr_logf(c, RR_LOG_INFO, "Archive extracted and verified: %zu files, %llu MiB", nfiles, (unsigned long long)(total >> 20));
        log_top_folders(c, p.list);
        unlink(zp);
        if (!nfiles) { r = rr_fail(c, RR_E_FORMAT, "the full pack archive contains no files"); break; }
        char sd[PATH_MAX]; snprintf(sd, sizeof sd, "%s/RetroRewind6", p.stage);
        if (rr_path_kind(sd) != 2) { r = rr_fail(c, RR_E_FORMAT, "the full pack archive has no RetroRewind6 content"); break; }
        /* version: the archive's own version.txt if it is valid, otherwise the newest version the server lists */
        char vt[PATH_MAX], *old; size_t ol;
        snprintf(vt, sizeof vt, "%s/version.txt", sd);
        if (rr_path_kind(vt) == 1 && rr_read_text_file(vt, &old, &ol, 256) == 0) {
            char *t = old; while (isspace((unsigned char)*t)) t++;
            char *e = t + strlen(t); while (e > t && isspace((unsigned char)e[-1])) *--e = 0;
            if (ver_ok(t)) snprintf(ver, sizeof ver, "%s", t);
            free(old);
        }
        if (!ver[0]) {
            char *txt; rr_manifest m;
            if (!(r = http_text(c, h, RR_VERSION_URL, &txt))) {
                r = rr_manifest_parse(c, txt, &m); free(txt);
                if (!r) { snprintf(ver, sizeof ver, "%s", m.e[m.n - 1].version); rr_manifest_free(&m); rr_logf(c, RR_LOG_WARN, "the archive has no version.txt; recording %s from the server list", ver); }
            }
            if (r) break;
            char line[64]; int n = snprintf(line, sizeof line, "%s\n", ver);
            if (rr_write_text_file_atomic(vt, line, (size_t)n)) { r = rr_fail(c, RR_E_IO, "cannot write version file"); break; }
        }
    } while (0);
    if (!r) {
        if (rr_cancelled(c)) r = rr_fail(c, RR_E_CANCEL, "cancelled");
        else if (rr_mkdir_p(p.rr)) r = rr_fail(c, RR_E_IO, "cannot create %s: %s", p.rr, strerror(errno));
        else {
            r = rr_merge_stage_ex(c, p.pack, p.stage, ver, extracted, p.list);
        }
    }
    {   /* clean up by name as well, for systems where a folder cannot be listed */
        char *txt; size_t len;
        if (rr_path_kind(p.list) == 1 && rr_read_text_file(p.list, &txt, &len, 256u << 20) == 0) {
            for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
                if (line[0] != 'F' && line[0] != 'D') continue;
                char nmbuf[PATH_MAX];
                snprintf(nmbuf, sizeof nmbuf, "%s%s", line + 2, line[0] == 'D' ? "/.d" : "");
                char *nm[1] = { nmbuf };
                rm_named(p.stage, nm, NULL, 1, 0);
            }
            free(txt);
        }
        { char *vn[1] = { (char *)"RetroRewind6/version.txt" }; rm_named(p.stage, vn, NULL, 1, 0); }
        unlink(zp); unlink(part);
        rmdir(p.dl); rmdir(p.stage); unlink(p.list);
    }
    rr_remove_tree(p.stage); rr_remove_tree(p.dl);
    if (!r) {
        char v[32];
        if (rr_read_installed_version(c, pack_dir, v) || strcmp(v, ver)) r = rr_fail(c, RR_E_STATE, "version file did not read back after the install");
        else rr_logf(c, RR_LOG_INFO, "Retro Rewind %s installed", v);
    }
    return r;
}

int rr_builder_present(const char *d)
{
    static const char *const f[] = {"copy-files.sh", "create-rr-usa.sh", "create-rr-pal.sh", "create-rr-jpn.sh", NULL};
    for (int i = 0; f[i]; i++) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", d, f[i]); if (rr_path_kind(p) != 1) return 0; }
    return 1;
}

int rr_builder_current(const char *d)
{
    char p[PATH_MAX], *txt = NULL; size_t len = 0;
    snprintf(p, sizeof p, "%s/.rr-builder-rev", d);
    if (rr_path_kind(p) != 1 || rr_read_text_file(p, &txt, &len, 256)) return 0;
    int ok = len >= strlen(RR_BUILDER_REV) && !strncmp(txt, RR_BUILDER_REV, strlen(RR_BUILDER_REV));
    free(txt);
    return ok;
}

static int builder_url_ok(const char *url)
{
    static const char *const ok[] = {"https://github.com/melvindevries/Retro-Rewind-PS5-Patcher/", "https://raw.githubusercontent.com/melvindevries/Retro-Rewind-PS5-Patcher/", NULL};
    if (strstr(url, "..") || strchr(url, '@')) return 0;
    for (int i = 0; ok[i]; i++) if (!strncmp(url, ok[i], strlen(ok[i]))) return 1;
    return 0;
}

int rr_install_builder(rr_ctx *c, const rr_http *h, const char *builder_dir, const char *url)
{
    if (!builder_url_ok(url)) return rr_fail(c, RR_E_UNSAFE, "refusing to download the builder from %s", url);
    int r = rr_mkdir_p(builder_dir);
    if (r) return rr_fail(c, RR_E_IO, "cannot create %s: %s (the app has no write permission there; see the Access row)", builder_dir, strerror(errno));
    char stage[PATH_MAX], dl[PATH_MAX], list[PATH_MAX], zp[PATH_MAX], part[PATH_MAX];
    snprintf(stage, sizeof stage, "%s/.rb-stage", builder_dir);
    snprintf(dl, sizeof dl, "%s/.rb-dl", builder_dir);
    snprintf(list, sizeof list, "%s/.rb-stage.list", builder_dir);
    snprintf(zp, sizeof zp, "%s/builder.zip", dl);
    snprintf(part, sizeof part, "%s.part", zp);
    unlink(list);
    if (fresh_dir(c, stage) || fresh_dir(c, dl)) return rr_fail(c, RR_E_IO, "cannot create working folders in %s: %s (the app has no write permission there; see the Access row)", builder_dir, strerror(errno));
    strvec files = {0}, dirs = {0}, created = {0};
    do {
        uint8_t *mem = NULL; size_t len = 0; int status = 0; uint64_t exp = 0;
        rr_logf(c, RR_LOG_INFO, "Downloading the Retro Rewind ISO builder files: %s", url);
        r = h->get(h->user, c, url, part, &mem, &len, 0, &exp, &status);
        free(mem);
        if (r) break;
        if (status != 200) { r = rr_fail(c, RR_E_NET, "download of the builder files failed: HTTP %d", status); break; }
        uint64_t sz = 0;
        if (rr_file_size(part, &sz) || sz != len || (exp && exp != sz) || sz < 22) { r = rr_fail(c, RR_E_CORRUPT, "download of the builder files is incomplete (%llu of %llu bytes)", (unsigned long long)sz, (unsigned long long)exp); break; }
        if (rename(part, zp)) { r = rr_fail(c, RR_E_IO, "cannot finish download"); break; }
        uint64_t freeb = rr_free_space(builder_dir);
        uint64_t maxt = freeb > (sz + (128ull << 20)) ? freeb - sz - (64ull << 20) : (64ull << 20);
        if (!freeb) maxt = 2ull << 30;
        uint64_t total = 0; size_t nfiles = 0;
        if ((r = rr_zip_extract_builder(c, zp, stage, maxt, &total, &nfiles, list))) break;
        unlink(zp);
        if ((r = load_names(c, list, &files, &dirs))) break;
        /* the archive must really be the builder */
        int need = 0;
        for (size_t i = 0; i < files.n; i++) if (!strcmp(files.v[i], "copy-files.sh") || !strcmp(files.v[i], "create-rr-usa.sh") || !strcmp(files.v[i], "create-rr-pal.sh") || !strcmp(files.v[i], "create-rr-jpn.sh")) need++;
        if (need != 4) { r = rr_fail(c, RR_E_FORMAT, "the builder archive does not contain copy-files.sh and the three create-rr scripts"); break; }
        rr_logf(c, RR_LOG_INFO, "Builder archive verified: %zu files kept, %llu KiB", files.n, (unsigned long long)(total >> 10));
        if (rr_cancelled(c)) { r = rr_fail(c, RR_E_CANCEL, "cancelled"); break; }
        /* place the files; anything that did not exist before is remembered so a failure can remove it again */
        for (size_t i = 0; i < dirs.n && !r; i++) { char d[PATH_MAX]; snprintf(d, sizeof d, "%s/%s", builder_dir, dirs.v[i]); if (rr_mkdir_p(d)) r = rr_fail(c, RR_E_IO, "cannot create %s: %s", d, strerror(errno)); }
        for (size_t i = 0; i < files.n && !r; i++) {
            char src[PATH_MAX], dest[PATH_MAX];
            snprintf(src, sizeof src, "%s/%s", stage, files.v[i]);
            snprintf(dest, sizeof dest, "%s/%s", builder_dir, files.v[i]);
            if (!rel_safe(files.v[i])) { r = rr_fail(c, RR_E_UNSAFE, "unsafe path in the builder archive"); break; }
            rr_fileinfo st;
            int existed = rr_stat(dest, 0, &st) == 0;
            if (existed && st.kind != 1) { r = rr_fail(c, RR_E_STATE, "%s exists but is not a normal file", dest); break; }
            char *sl = strrchr(dest, '/'); *sl = 0; int mr = rr_mkdir_p(dest); *sl = '/';
            if (mr) { r = rr_fail(c, RR_E_IO, "cannot create folder for %s", dest); break; }
            if (rename(src, dest)) { r = rr_fail(c, RR_E_IO, "cannot install %s: %s", dest, strerror(errno)); break; }
            if (!existed) sv_add(&created, files.v[i]);
        }
    } while (0);
    if (r) for (size_t i = 0; i < created.n; i++) { char d[PATH_MAX]; snprintf(d, sizeof d, "%s/%s", builder_dir, created.v[i]); unlink(d); }
    {   /* clean up by name as well, for systems where a folder cannot be listed */
        char *txt; size_t len;
        for (int pass = 0; pass < 2; pass++) /* second pass removes folders that only became empty during the first */
        if (rr_path_kind(list) == 1 && rr_read_text_file(list, &txt, &len, 64u << 20) == 0) {
            for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
                if (line[0] != 'F' && line[0] != 'D') continue;
                char nmbuf[PATH_MAX];
                snprintf(nmbuf, sizeof nmbuf, "%s%s", line + 2, line[0] == 'D' ? "/.d" : "");
                char *nm[1] = { nmbuf };
                rm_named(stage, nm, NULL, 1, 0);
            }
            free(txt);
        }
        unlink(zp); unlink(part);
        rmdir(dl); rmdir(stage); unlink(list);
    }
    rr_remove_tree(stage); rr_remove_tree(dl);
    sv_free(&files); sv_free(&dirs); sv_free(&created);
    if (!r) {
        if (!rr_builder_present(builder_dir)) r = rr_fail(c, RR_E_STATE, "the builder files are not all in place after the install");
        else {
            char mp[PATH_MAX];
            snprintf(mp, sizeof mp, "%s/.rr-builder-rev", builder_dir);
            if (rr_write_text_file_atomic(mp, RR_BUILDER_REV "\n", strlen(RR_BUILDER_REV) + 1)) r = rr_fail(c, RR_E_IO, "cannot write the builder revision marker");
            else rr_logf(c, RR_LOG_INFO, "ISO builder files installed in %s", builder_dir);
        }
    }
    return r;
}

int rr_update_apply(rr_ctx *c, const rr_http *h, const char *pack_dir, rr_update_state *s)
{
    paths_t p; mkpaths(&p, pack_dir);
    int r = rr_update_recover(c, pack_dir);
    if (r) return r;
    size_t i = s->first_pending;
    while (i < s->m.n && !r) {
        size_t j = i;
        while (j < s->m.n && !strcmp(s->m.e[j].version, s->m.e[i].version)) j++;
        r = apply_version(c, h, &p, s, i, j);
        if (!r) {
            char v[32];
            if (rr_read_installed_version(c, pack_dir, v) || strcmp(v, s->m.e[i].version)) { r = rr_fail(c, RR_E_STATE, "version file did not read back as %s after the update", s->m.e[i].version); break; }
            snprintf(s->installed, sizeof s->installed, "%s", v);
            s->first_pending = j; s->n_pending = s->m.n - j;
        }
        i = j;
    }
    if (!r) s->up_to_date = 1;
    return r;
}
