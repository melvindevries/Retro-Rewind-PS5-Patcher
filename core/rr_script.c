/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "rr_script.h"
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXWORDS 512
#define MAXVARS 64

typedef struct { char *name; char *val; } var_t;
typedef struct { char *text; int glob; } word_t;

typedef struct {
    rr_ctx *c;
    rr_vtree *t;
    const rr_script_roots *roots;
    var_t vars[MAXVARS];
    int nvars;
    char **lines; size_t nlines;
    unsigned long cps; /* statistics */
} sh_t;

static const char *var_get(const sh_t *s, const char *name, size_t nl)
{
    for (int i = 0; i < s->nvars; i++) if (strlen(s->vars[i].name) == nl && !strncmp(s->vars[i].name, name, nl)) return s->vars[i].val;
    return NULL;
}

static int var_set(sh_t *s, const char *name, const char *val)
{
    for (int i = 0; i < s->nvars; i++) if (!strcmp(s->vars[i].name, name)) { free(s->vars[i].val); s->vars[i].val = rr_xstrdup(val); return 0; }
    if (s->nvars >= MAXVARS) return rr_fail(s->c, RR_E_FORMAT, "script uses too many variables");
    s->vars[s->nvars].name = rr_xstrdup(name); s->vars[s->nvars].val = rr_xstrdup(val); s->nvars++;
    return 0;
}

/* ---------------------------------------------------------------- tokenizer */

static void words_free(word_t *w, int n) { for (int i = 0; i < n; i++) free(w[i].text); }

static int buf_add(char **b, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (*len + n + 1 > *cap) { *cap = (*len + n + 1) * 2; *b = rr_xrealloc(*b, *cap); }
    memcpy(*b + *len, s, n); *len += n; (*b)[*len] = 0;
    return 0;
}

static int expand_var(sh_t *s, const char **pp, char **b, size_t *len, size_t *cap)
{
    const char *p = *pp; /* points after '$' */
    const char *name; size_t nl;
    if (*p == '{') {
        name = p + 1;
        const char *e = strchr(name, '}');
        if (!e) return rr_fail(s->c, RR_E_FORMAT, "unterminated ${...} in script");
        nl = (size_t)(e - name);
        for (size_t i = 0; i < nl; i++) if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) return rr_fail(s->c, RR_E_UNSUPPORTED, "unsupported parameter expansion in script");
        p = e + 1;
    } else if (isalpha((unsigned char)*p) || *p == '_') {
        name = p; nl = 0;
        while (isalnum((unsigned char)p[nl]) || p[nl] == '_') nl++;
        p += nl;
    } else return rr_fail(s->c, RR_E_UNSUPPORTED, "unsupported use of '$' in script");
    if (!nl) return rr_fail(s->c, RR_E_FORMAT, "empty variable name in script");
    const char *v = var_get(s, name, nl);
    if (!v) return rr_fail(s->c, RR_E_FORMAT, "script uses undefined variable '%.*s'", (int)nl, name);
    buf_add(b, len, cap, v, strlen(v));
    *pp = p;
    return 0;
}

static int tokenize(sh_t *s, const char *line, word_t *w, int *nw)
{
    *nw = 0;
    const char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') return 0;
        if (*nw >= MAXWORDS) return rr_fail(s->c, RR_E_FORMAT, "script line has too many words");
        char *b = NULL; size_t len = 0, cap = 0; int glob = 0;
        buf_add(&b, &len, &cap, "", 0);
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') {
                const char *e = strchr(p + 1, '\'');
                if (!e) { free(b); return rr_fail(s->c, RR_E_FORMAT, "unterminated quote in script"); }
                buf_add(&b, &len, &cap, p + 1, (size_t)(e - p - 1));
                p = e + 1;
            } else if (*p == '"') {
                p++;
                while (*p != '"') {
                    if (!*p) { free(b); return rr_fail(s->c, RR_E_FORMAT, "unterminated quote in script"); }
                    if (*p == '\\' || *p == '`') { free(b); return rr_fail(s->c, RR_E_UNSUPPORTED, "unsupported escape in script"); }
                    if (*p == '$') { p++; if (expand_var(s, &p, &b, &len, &cap)) { free(b); return RR_E_FORMAT; } }
                    else { buf_add(&b, &len, &cap, p, 1); p++; }
                }
                p++;
            } else if (*p == '$') {
                p++;
                if (expand_var(s, &p, &b, &len, &cap)) { free(b); return RR_E_FORMAT; }
            } else if (*p == '\\' || *p == '`' || *p == '(' || *p == ')' || *p == '|' || *p == '&' || *p == '<' || *p == '>' || *p == ';') {
                free(b);
                return rr_fail(s->c, RR_E_UNSUPPORTED, "unsupported shell syntax '%c' in script", *p);
            } else {
                if (*p == '*' || *p == '?' || *p == '[') glob = 1;
                buf_add(&b, &len, &cap, p, 1); p++;
            }
        }
        w[*nw].text = b; w[*nw].glob = glob; (*nw)++;
    }
}

/* ---------------------------------------------------------------- paths */

typedef enum { LOC_BAD, LOC_FILES, LOC_SYS, LOC_HOST } loc_kind;
typedef struct { loc_kind kind; char rel[1024]; /* LOC_FILES: path below files/; LOC_SYS: file name; */ char host[PATH_MAX]; int trailing_slash; } loc_t;

static int normalize(const char *in, char *out, size_t cap, int *trailing)
{
    size_t n = strlen(in);
    *trailing = n && in[n - 1] == '/';
    size_t o = 0;
    const char *p = in;
    if (*p == '/') return -1; /* absolute paths are never used by the scripts */
    while (*p) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == 0 || (l == 1 && p[0] == '.')) { /* skip */ }
        else if (l == 2 && p[0] == '.' && p[1] == '.') return -1;
        else {
            if (o + l + 2 > cap) return -1;
            if (o) out[o++] = '/';
            memcpy(out + o, p, l); o += l;
        }
        if (!e) break;
        p = e + 1;
    }
    out[o] = 0;
    return 0;
}

static int resolve(sh_t *s, const char *raw, loc_t *l)
{
    char norm[1024];
    memset(l, 0, sizeof *l);
    if (normalize(raw, norm, sizeof norm, &l->trailing_slash)) return rr_fail(s->c, RR_E_UNSAFE, "script path '%s' is not allowed", raw);
    if (!strncmp(norm, "workdir.tmp", 11) && (norm[11] == 0 || norm[11] == '/')) {
        const char *rest = norm[11] ? norm + 12 : norm + 11;
        if (!strcmp(rest, "files")) { l->kind = LOC_FILES; l->rel[0] = 0; return 0; }
        if (!strncmp(rest, "files/", 6)) { l->kind = LOC_FILES; snprintf(l->rel, sizeof l->rel, "%s", rest + 6); return 0; }
        if (!strcmp(rest, "sys")) { l->kind = LOC_SYS; l->rel[0] = 0; return 0; }
        if (!strncmp(rest, "sys/", 4) && !strchr(rest + 4, '/')) { l->kind = LOC_SYS; snprintf(l->rel, sizeof l->rel, "%s", rest + 4); return 0; }
        return rr_fail(s->c, RR_E_UNSUPPORTED, "script touches unsupported work path '%s'", raw);
    }
    l->kind = LOC_HOST;
    if (!strncmp(norm, "RetroRewind6", 12) && (norm[12] == 0 || norm[12] == '/'))
        snprintf(l->host, sizeof l->host, "%s/%s", s->roots->pack_dir, norm);
    else
        snprintf(l->host, sizeof l->host, "%s/%s", s->roots->builder_dir, norm);
    return 0;
}

/* The scripts were written for Windows/Wiimm's tools, where file names are case-insensitive; the console's file system is
 * not. If path does not exist exactly as typed, every component is looked up ignoring case ("Code.pul" finds "code.pul").
 * out gets the path as it really exists, or the typed path when nothing matches. */
static void fix_case(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    if (rr_path_kind(path) != 0) return;
    char cur[PATH_MAX];
    size_t cl = 0;
    const char *p = path;
    if (*p == '/') { cur[0] = 0; p++; cl = 0; }
    while (*p) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char comp[256];
        if (l == 0) { p = e ? e + 1 : p + l; continue; }
        if (l >= sizeof comp) return;
        memcpy(comp, p, l); comp[l] = 0;
        char cand[PATH_MAX];
        snprintf(cand, sizeof cand, "%s/%s", cur, comp);
        if (cl == 0 && path[0] != '/') snprintf(cand, sizeof cand, "%s", comp);
        if (rr_path_kind(cand) == 0) {
            rr_dir *d = rr_opendir(cl == 0 && path[0] != '/' ? "." : (cur[0] ? cur : "/"));
            int found = 0;
            if (d) {
                const char *en;
                while ((en = rr_readdir(d))) if (!strcasecmp(en, comp)) { snprintf(cand, sizeof cand, "%s/%s", cur, en); if (cl == 0 && path[0] != '/') snprintf(cand, sizeof cand, "%s", en); found = 1; break; }
                rr_closedir(d);
            }
            if (!found) return;
        }
        snprintf(cur, sizeof cur, "%s", cand);
        cl = strlen(cur);
        p = e ? e + 1 : p + l;
    }
    snprintf(out, cap, "%s", cur);
}


/* Does not use realpath(): that call is refused to titles on the console. The script paths were already normalised (no "..",
 * no absolute parts), so containment is a prefix test on the root, and symbolic links are refused component by component. */
static int under_root(const char *path, const char *root, size_t *rootlen)
{
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (strncmp(path, root, n) || (path[n] != '/' && path[n] != 0)) return 0;
    *rootlen = n;
    return 1;
}

static int host_ok(sh_t *s, const char *path)
{
    char fixed[PATH_MAX];
    fix_case(path, fixed, sizeof fixed);
    rr_fileinfo fi;
    if (rr_stat(fixed, 0, &fi)) return rr_fail(s->c, RR_E_IO, "script source '%s' does not exist (%s)", path, strerror(errno));
    size_t rl = 0;
    if (!under_root(fixed, s->roots->pack_dir, &rl) && !under_root(fixed, s->roots->builder_dir, &rl))
        return rr_fail(s->c, RR_E_UNSAFE, "script source '%s' resolves outside the allowed folders", path);
    if (strstr(fixed, "/../") || (strlen(fixed) >= 3 && !strcmp(fixed + strlen(fixed) - 3, "/..")))
        return rr_fail(s->c, RR_E_UNSAFE, "script source '%s' resolves outside the allowed folders", path);
    /* no symbolic link anywhere below the root */
    char cur[PATH_MAX];
    snprintf(cur, sizeof cur, "%s", fixed);
    for (char *e = cur + rl; *e; ) {
        char *nx = strchr(e + 1, '/');
        if (nx) *nx = 0;
        rr_fileinfo c;
        if (rr_stat(cur, 0, &c) == 0 && c.kind == 3) return rr_fail(s->c, RR_E_UNSAFE, "script source '%s' goes through a symbolic link", path);
        if (!nx) break;
        *nx = '/';
        e = nx;
    }
    return 0;
}

/* ---------------------------------------------------------------- copy */

static int join_rel(char *out, size_t cap, const char *dir, const char *name)
{
    int n = dir[0] ? snprintf(out, cap, "%s/%s", dir, name) : snprintf(out, cap, "%s", name);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list_dir(sh_t *s, const char *path, char ***out, size_t *n)
{
    rr_dir *d = rr_opendir(path);
    if (!d) return rr_fail(s->c, RR_E_IO, "cannot read folder %s", path);
    char **v = NULL; size_t cnt = 0, cap = 0;
    const char *ename;
    while ((ename = rr_readdir(d))) {
        if (ename[0] == '.') continue; /* sh globs and our recursion both skip dot files */
        if (cnt == cap) { cap = cap ? cap * 2 : 64; v = rr_xrealloc(v, cap * sizeof *v); }
        v[cnt++] = rr_xstrdup(ename);
    }
    rr_closedir(d);
    qsort(v, cnt, sizeof *v, cmp_str);
    *out = v; *n = cnt;
    return 0;
}

static void free_list(char **v, size_t n) { for (size_t i = 0; i < n; i++) free(v[i]); free(v); }

static int copy_into_files(sh_t *s, const char *src, const char *destrel, int recursive);

static int copy_dir(sh_t *s, const char *src, const char *destrel)
{
    int r = rr_vt_mkdir_p(s->c, s->t, destrel);
    if (r) return r;
    char **names; size_t n;
    if ((r = list_dir(s, src, &names, &n))) return r;
    for (size_t i = 0; i < n && !r; i++) {
        char sp[PATH_MAX], dp[1024];
        if (snprintf(sp, sizeof sp, "%s/%s", src, names[i]) >= (int)sizeof sp || join_rel(dp, sizeof dp, destrel, names[i])) { r = rr_fail(s->c, RR_E_FORMAT, "path too long"); break; }
        r = copy_into_files(s, sp, dp, 1);
    }
    free_list(names, n);
    return r;
}

static int copy_into_files(sh_t *s, const char *src_typed, const char *destrel, int recursive)
{
    char src[PATH_MAX];
    fix_case(src_typed, src, sizeof src);
    rr_fileinfo st;
    if (rr_stat(src, 0, &st)) return rr_fail(s->c, RR_E_IO, "cp: cannot stat '%s': %s", src, strerror(errno));
    if (st.kind == 3) return rr_fail(s->c, RR_E_UNSAFE, "cp: refusing to follow symbolic link '%s'", src);
    if (st.kind == 2) {
        if (!recursive) return rr_fail(s->c, RR_E_FORMAT, "cp: -r not specified; omitting directory '%s'", src);
        return copy_dir(s, src, destrel);
    }
    if (st.kind != 1) return rr_fail(s->c, RR_E_UNSAFE, "cp: '%s' is not a regular file", src);
    int r = rr_vt_put_host(s->c, s->t, destrel, src, (uint64_t)st.st_size);
    if (!r) s->cps++;
    return r;
}

static const char *base_name(const char *p) { const char *sl = strrchr(p, '/'); return sl ? sl + 1 : p; }

static int do_cp(sh_t *s, word_t *w, int nw)
{
    int recursive = 0, i = 1;
    for (; i < nw && w[i].text[0] == '-' && w[i].text[1]; i++) {
        for (const char *o = w[i].text + 1; *o; o++) {
            if (*o == 'r' || *o == 'R') recursive = 1;
            else if (*o != 'f') return rr_fail(s->c, RR_E_UNSUPPORTED, "cp: unsupported option -%c", *o);
        }
    }
    if (nw - i < 2) return rr_fail(s->c, RR_E_FORMAT, "cp: missing operands");
    /* destination */
    loc_t dl;
    int r = resolve(s, w[nw - 1].text, &dl);
    if (r) return r;
    if (dl.kind == LOC_HOST || dl.kind == LOC_BAD) return rr_fail(s->c, RR_E_UNSAFE, "cp: destination '%s' is not inside the work area", w[nw - 1].text);
    /* sources (glob expansion) */
    char **srcs = NULL; size_t ns = 0, cs = 0;
    for (int k = i; k < nw - 1; k++) {
        loc_t sl;
        if ((r = resolve(s, w[k].text, &sl))) goto out;
        if (sl.kind != LOC_HOST) { r = rr_fail(s->c, RR_E_UNSUPPORTED, "cp: source '%s' must come from the builder or pack folder", w[k].text); goto out; }
        if (w[k].glob) {
            char *sl2 = strrchr(sl.host, '/');
            if (!sl2 || strchr(sl2 + 1, '*') == NULL || strpbrk(sl.host, "?[") || strchr(sl.host, '*') != strrchr(sl.host, '*') || strcmp(sl2 + 1, "*")) { r = rr_fail(s->c, RR_E_UNSUPPORTED, "cp: only a trailing '/*' glob is supported ('%s')", w[k].text); goto out; }
            *sl2 = 0;
            if ((r = host_ok(s, sl.host))) goto out;
            { char fx[PATH_MAX]; fix_case(sl.host, fx, sizeof fx); snprintf(sl.host, sizeof sl.host, "%s", fx); }
            char **names; size_t nn;
            if ((r = list_dir(s, sl.host, &names, &nn))) goto out;
            if (!nn) { free_list(names, nn); r = rr_fail(s->c, RR_E_FORMAT, "cp: no files match '%s'", w[k].text); goto out; }
            for (size_t q = 0; q < nn; q++) {
                char tmp[PATH_MAX];
                if (snprintf(tmp, sizeof tmp, "%s/%s", sl.host, names[q]) >= (int)sizeof tmp) { free_list(names, nn); r = rr_fail(s->c, RR_E_FORMAT, "path too long"); goto out; }
                if (ns == cs) { cs = cs ? cs * 2 : 16; srcs = rr_xrealloc(srcs, cs * sizeof *srcs); }
                srcs[ns++] = rr_xstrdup(tmp);
            }
            free_list(names, nn);
        } else {
            if ((r = host_ok(s, sl.host))) goto out;
            if (ns == cs) { cs = cs ? cs * 2 : 16; srcs = rr_xrealloc(srcs, cs * sizeof *srcs); }
            srcs[ns++] = rr_xstrdup(sl.host);
        }
    }
    /* sys/ destination */
    if (dl.kind == LOC_SYS) {
        for (size_t k = 0; k < ns; k++) {
            rr_fileinfo st; char real[PATH_MAX];
            fix_case(srcs[k], real, sizeof real);
            if (rr_stat(real, 0, &st) || st.kind != 1) { r = rr_fail(s->c, RR_E_FORMAT, "cp: '%s' is not a regular file", srcs[k]); goto out; }
            const char *name = dl.rel[0] ? dl.rel : base_name(srcs[k]);
            if (dl.rel[0] && ns > 1) { r = rr_fail(s->c, RR_E_FORMAT, "cp: target is not a directory"); goto out; }
            if ((r = rr_vt_put_sys_host(s->c, s->t, name, real, (uint64_t)st.st_size))) goto out;
            s->cps++;
        }
        r = 0; goto out;
    }
    /* files/ destination */
    int di = dl.rel[0] ? rr_vt_find(s->t, dl.rel) : -2; /* -2 = the files root, which always exists */
    int dest_is_dir = (di == -2) || (di >= 0 && s->t->n[di].is_dir);
    if (dl.trailing_slash && !dest_is_dir) { r = rr_fail(s->c, RR_E_FORMAT, "cp: cannot create '%s': not a directory on the disc", w[nw - 1].text); goto out; }
    if (dest_is_dir) {
        for (size_t k = 0; k < ns; k++) {
            char dp[1024];
            if (join_rel(dp, sizeof dp, dl.rel, base_name(srcs[k]))) { r = rr_fail(s->c, RR_E_FORMAT, "path too long"); goto out; }
            if ((r = copy_into_files(s, srcs[k], dp, recursive))) goto out;
        }
    } else {
        if (ns != 1) { r = rr_fail(s->c, RR_E_FORMAT, "cp: target '%s' is not a directory", w[nw - 1].text); goto out; }
        if ((r = copy_into_files(s, srcs[0], dl.rel, recursive))) goto out;
    }
    r = 0;
out:
    for (size_t k = 0; k < ns; k++) free(srcs[k]);
    free(srcs);
    return r;
}

static int do_mkdir(sh_t *s, word_t *w, int nw)
{
    int i = 1, p = 0;
    for (; i < nw && w[i].text[0] == '-'; i++) { if (!strcmp(w[i].text, "-p")) p = 1; else return rr_fail(s->c, RR_E_UNSUPPORTED, "mkdir: unsupported option %s", w[i].text); }
    if (!p) return rr_fail(s->c, RR_E_UNSUPPORTED, "mkdir without -p is not supported");
    for (; i < nw; i++) {
        loc_t l;
        int r = resolve(s, w[i].text, &l);
        if (r) return r;
        if (l.kind != LOC_FILES) return rr_fail(s->c, RR_E_UNSUPPORTED, "mkdir: '%s' is outside the disc file area", w[i].text);
        if (l.rel[0] && (r = rr_vt_mkdir_p(s->c, s->t, l.rel))) return r;
    }
    return 0;
}

static int do_dolpatch(sh_t *s, word_t *w, int nw)
{
    if (nw < 4) return rr_fail(s->c, RR_E_FORMAT, "wit dolpatch: missing arguments");
    loc_t l;
    int r = resolve(s, w[2].text, &l);
    if (r) return r;
    if (l.kind != LOC_SYS || strcmp(l.rel, "main.dol")) return rr_fail(s->c, RR_E_UNSUPPORTED, "wit dolpatch: only sys/main.dol can be patched");
    for (int i = 3; i < nw; i++) {
        char *eq = strchr(w[i].text, '=');
        if (!eq) return rr_fail(s->c, RR_E_FORMAT, "wit dolpatch: bad patch '%s'", w[i].text);
        *eq = 0;
        char *end;
        unsigned long addr = strtoul(w[i].text, &end, 16);
        if (*end || !*w[i].text || addr > 0xFFFFFFFFul) return rr_fail(s->c, RR_E_FORMAT, "wit dolpatch: bad address");
        size_t hl = strlen(eq + 1);
        if (!hl || (hl & 1) || hl > 64) return rr_fail(s->c, RR_E_FORMAT, "wit dolpatch: bad value");
        uint8_t bytes[32];
        for (size_t k = 0; k < hl / 2; k++) {
            char h[3] = {eq[1 + 2 * k], eq[2 + 2 * k], 0};
            if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1])) return rr_fail(s->c, RR_E_FORMAT, "wit dolpatch: bad value");
            bytes[k] = (uint8_t)strtoul(h, NULL, 16);
        }
        if ((r = rr_vt_patch_dol(s->c, s->t, (uint32_t)addr, bytes, hl / 2))) return r;
    }
    return 0;
}

/* ---------------------------------------------------------------- runner */

static int run_range(sh_t *s, size_t a, size_t b, int depth);

static int exec_line(sh_t *s, const char *line)
{
    word_t w[MAXWORDS]; int nw;
    int r = tokenize(s, line, w, &nw);
    if (r) { words_free(w, nw); return r; }
    if (!nw) return 0;
    const char *cmd = w[0].text;
    char *eq = strchr(w[0].text, '=');
    if (nw == 1 && eq && eq > cmd) {
        int ok = 1;
        for (const char *q = cmd; q < eq; q++) if (!(isalnum((unsigned char)*q) || *q == '_')) ok = 0;
        if (ok) { *eq = 0; r = var_set(s, cmd, eq + 1); words_free(w, nw); return r; }
    }
    if (!strcmp(cmd, "cp")) r = do_cp(s, w, nw);
    else if (!strcmp(cmd, "mkdir")) r = do_mkdir(s, w, nw);
    else if (!strcmp(cmd, "wit") && nw >= 2 && !strcmp(w[1].text, "dolpatch")) r = do_dolpatch(s, w, nw);
    else r = rr_fail(s->c, RR_E_UNSUPPORTED, "unsupported command in script: '%s'", cmd);
    words_free(w, nw);
    return r;
}

static int run_range(sh_t *s, size_t a, size_t b, int depth)
{
    if (depth > 4) return rr_fail(s->c, RR_E_FORMAT, "script loops nested too deeply");
    for (size_t i = a; i < b; i++) {
        if (rr_cancelled(s->c)) return rr_fail(s->c, RR_E_CANCEL, "cancelled");
        const char *ln = s->lines[i];
        while (*ln == ' ' || *ln == '\t') ln++;
        if (!*ln || *ln == '#') continue;
        if (!strncmp(ln, "for ", 4)) {
            word_t w[MAXWORDS]; int nw;
            char *hdr = rr_xstrdup(ln);
            size_t hl = strlen(hdr);
            while (hl && (hdr[hl - 1] == ' ' || hdr[hl - 1] == '\t')) hdr[--hl] = 0;
            size_t body = i + 1;
            if (hl >= 4 && !strcmp(hdr + hl - 4, "; do")) hdr[hl - 4] = 0;
            else if (body < b && !strcmp(s->lines[body], "do")) body++;
            else { free(hdr); return rr_fail(s->c, RR_E_FORMAT, "malformed for-loop in script"); }
            int r = tokenize(s, hdr, w, &nw);
            free(hdr);
            if (r) { words_free(w, nw); return r; }
            if (nw < 4 || strcmp(w[2].text, "in")) { words_free(w, nw); return rr_fail(s->c, RR_E_FORMAT, "malformed for-loop in script"); }
            int depth2 = 1; size_t j = body;
            for (; j < b; j++) {
                const char *l2 = s->lines[j];
                while (*l2 == ' ' || *l2 == '\t') l2++;
                if (!strncmp(l2, "for ", 4)) depth2++;
                else if (!strcmp(l2, "done") && --depth2 == 0) break;
            }
            if (j >= b) { words_free(w, nw); return rr_fail(s->c, RR_E_FORMAT, "for-loop without 'done'"); }
            char *name = rr_xstrdup(w[1].text);
            for (int k = 3; k < nw && !r; k++) { r = var_set(s, name, w[k].text); if (!r) r = run_range(s, body, j, depth + 1); }
            free(name); words_free(w, nw);
            if (r) return r;
            i = j;
            continue;
        }
        int r = exec_line(s, ln);
        if (r) return r;
    }
    return 0;
}

int rr_script_run(rr_ctx *c, rr_vtree *t, const char *script, const rr_script_roots *roots, const char *const *vars)
{
    sh_t s;
    memset(&s, 0, sizeof s);
    s.c = c; s.t = t; s.roots = roots;
    for (; vars && *vars; vars++) {
        const char *eq = strchr(*vars, '=');
        if (!eq) return rr_fail(c, RR_E_USAGE, "bad preset variable");
        char *n = rr_xstrdup(*vars); n[eq - *vars] = 0;
        var_set(&s, n, eq + 1);
        free(n);
    }
    /* logical lines: join backslash-newline continuations, normalise CRLF */
    size_t cap = 256, nl = 0;
    s.lines = rr_xmalloc(cap * sizeof *s.lines);
    const char *p = script;
    char *cur = NULL; size_t cl = 0, cc = 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        size_t raw = l;
        if (l && p[l - 1] == '\r') l--;
        int cont = l && p[l - 1] == '\\';
        if (cont) l--;
        buf_add(&cur, &cl, &cc, p, l);
        if (!cont) {
            if (nl == cap) { cap *= 2; s.lines = rr_xrealloc(s.lines, cap * sizeof *s.lines); }
            s.lines[nl++] = cur; cur = NULL; cl = cc = 0;
        } else buf_add(&cur, &cl, &cc, " ", 1);
        p += raw + (e ? 1 : 0);
        if (!e) break;
    }
    if (cur) { if (nl == cap) { cap *= 2; s.lines = rr_xrealloc(s.lines, cap * sizeof *s.lines); } s.lines[nl++] = cur; }
    s.nlines = nl;
    int r = run_range(&s, 0, nl, 0);
    if (!r) rr_logf(c, RR_LOG_INFO, "copy script finished: %lu file copies", s.cps);
    for (size_t i = 0; i < nl; i++) free(s.lines[i]);
    free(s.lines);
    for (int i = 0; i < s.nvars; i++) { free(s.vars[i].name); free(s.vars[i].val); }
    return r;
}
