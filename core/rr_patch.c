/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "rr_patch.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *rr_region_dest_id(char r) { return r == 'E' ? "RMCETO" : r == 'P' ? "RMCPTO" : r == 'J' ? "RMCJTO" : NULL; }
const char *rr_region_src_id(char r) { return r == 'E' ? "RMCE01" : r == 'P' ? "RMCP01" : r == 'J' ? "RMCJ01" : NULL; }
static const char *region_type(char r) { return r == 'E' ? "USA" : r == 'P' ? "EUR" : "JPN"; }
static const char *region_script(char r) { return r == 'E' ? "create-rr-usa.sh" : r == 'P' ? "create-rr-pal.sh" : "create-rr-jpn.sh"; }

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

/* 1 = file (or anything that is not a directory), 2 = directory, 0 = cannot tell. Does not trust only stat():
 * a directory can always be opened with O_DIRECTORY. */
static int classify(const char *path)
{
    rr_fileinfo fi;
    if (rr_stat(path, 1, &fi) == 0) return fi.kind == 2 ? 2 : 1;
    return 0;
}

/* A file is a WBFS image if it starts with the magic "WBFS" (used for names without an extension). */
static int looks_wbfs(const char *path)
{
    char m[4];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int ok = read(fd, m, 4) == 4 && !memcmp(m, "WBFS", 4);
    close(fd);
    return ok;
}

/* how many entries of a folder carry exactly this name (a listing can show a name twice on this console) */
static int count_named(const char *dir, const char *name)
{
    int n = 0;
    rr_dir *d = rr_opendir(dir);
    if (!d) return 0;
    const char *e;
    while ((e = rr_readdir(d))) if (!strcmp(e, name)) n++;
    rr_closedir(d);
    return n;
}

/* Puts the finished image in place. The new image is already complete and verified, so any older copy (and any duplicate entries
 * with the same name that this console's file system may have created) is removed first and the new one is renamed into place.
 * If the rename fails, the finished image stays as <name>.part and is not deleted. */
static int install_output(rr_ctx *c, const char *part, const char *opath, const char *dir, const char *name)
{
    int removed = 0;
    while (removed < 16 && unlink(opath) == 0) removed++;
    if (removed > 1) rr_logf(c, RR_LOG_WARN, "removed %d older files named %s", removed, name);
    else if (removed == 1) rr_logf(c, RR_LOG_INFO, "replacing the previous %s", name);
    if (rename(part, opath) != 0) return RR_E_IO;
    int n = count_named(dir, name);
    if (n > 1) rr_logf(c, RR_LOG_WARN, "%s is listed %d times in %s", name, n, dir);
    return 0;
}

typedef struct { int nwbfs, nother; char found[PATH_MAX], other[256], saw[300]; int nsaw; } scan_t;

static void scan_dir(const char *dir, const char *rel, int depth, scan_t *sc)
{
    rr_dir *d = rr_opendir(dir);
    if (!d) return;
    const char *ename;
    while ((ename = rr_readdir(d))) {
        if (ename[0] == '.') continue;
        char full[PATH_MAX], relname[PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", dir, ename) >= (int)sizeof full) continue;
        if (snprintf(relname, sizeof relname, "%s%s", rel, ename) >= (int)sizeof relname) continue;
        int k = classify(full);
        if (sc->nsaw < 8) {
            size_t l = strlen(sc->saw);
            snprintf(sc->saw + l, sizeof sc->saw - l, "%s%s%s", l ? ", " : "", relname, k == 2 ? "/" : k == 0 ? "?" : "");
            sc->nsaw++;
        }
        if (k == 2) {
            if (depth < 2) { char sub[PATH_MAX]; if (snprintf(sub, sizeof sub, "%s/", relname) < (int)sizeof sub) scan_dir(full, sub, depth + 1, sc); }
            continue;
        }
        if (k == 0) continue;
        if (has_ext(ename, ".wbfs")) { sc->nwbfs++; snprintf(sc->found, sizeof sc->found, "%s", relname); }
        else if (!strchr(ename, '.') && looks_wbfs(full)) { sc->nwbfs++; snprintf(sc->found, sizeof sc->found, "%s", relname); } /* WBFS file whose name has no extension */
        else if (has_ext(ename, ".iso") || has_ext(ename, ".wia") || has_ext(ename, ".rvz") || has_ext(ename, ".ciso") ||
                 has_ext(ename, ".wdf") || has_ext(ename, ".gcz") || has_ext(ename, ".wbf1")) { sc->nother++; snprintf(sc->other, sizeof sc->other, "%s", relname); }
    }
    rr_closedir(d);
}

int rr_detect_base(rr_ctx *c, const char *base_dir, char *path_out, size_t cap, char *region_out, char id_out[8])
{
    rr_dir *probe = rr_opendir(base_dir);
    if (!probe) { int e = errno; return rr_fail(c, RR_E_IO, "base game folder %s cannot be opened: %s (errno %d) [%s]", base_dir, strerror(e), e, rr_dir_diag()); }
    rr_closedir(probe);
    scan_t *sc = rr_xmalloc(sizeof *sc);
    memset(sc, 0, sizeof *sc);
    scan_dir(base_dir, "", 0, sc);
    int nwbfs = sc->nwbfs, nother = sc->nother;
    char found[PATH_MAX], other[256], saw[300];
    snprintf(found, sizeof found, "%s", sc->found); snprintf(other, sizeof other, "%s", sc->other); snprintf(saw, sizeof saw, "%s", sc->saw);
    free(sc);
    if (nwbfs == 0 && !nother) {
        /* If the folder cannot be listed on this system, try the usual file names directly. */
        static const char *const guess[] = {"RMCE01.wbfs", "RMCP01.wbfs", "RMCJ01.wbfs", "game.wbfs", "mkw.wbfs", "mkwii.wbfs",
                                            "MarioKartWii.wbfs", "Mario Kart Wii.wbfs", "base.wbfs"};
        for (size_t i = 0; i < sizeof guess / sizeof *guess; i++) {
            char full[PATH_MAX];
            snprintf(full, sizeof full, "%s/%s", base_dir, guess[i]);
            if (classify(full) == 1) { nwbfs++; snprintf(found, sizeof found, "%s", guess[i]); }
        }
    }
    if (nwbfs > 1) return rr_fail(c, RR_E_USAGE, "found %d .wbfs files in %s; keep exactly one game image there", nwbfs, base_dir);
    if (nwbfs == 0) {
        if (nother) return rr_fail(c, RR_E_UNSUPPORTED, "'%s' is not a supported format; only .wbfs images can be used", other);
        return rr_fail(c, RR_E_USAGE, "no .wbfs game image found in %s (folder %s%s) [listing: %s]%s", base_dir, saw[0] ? "holds: " : "is empty as seen by this app", saw, rr_dir_diag(),
                       saw[0] ? "" : " If the file is there, this console blocks folder listing: name it RMCE01.wbfs (USA), RMCP01.wbfs (PAL) or RMCJ01.wbfs (Japan).");
    }
    if (snprintf(path_out, cap, "%s/%s", base_dir, found) >= (int)cap) return rr_fail(c, RR_E_FORMAT, "path too long");
    wbfs_img w;
    int r = wbfs_img_open(c, &w, path_out);
    if (r) return r;
    uint8_t h[8];
    r = wbfs_img_read(c, &w, 0, h, 8);
    wbfs_img_close(&w);
    if (r) return r;
    if (memcmp(h, "RMC", 3) || h[4] != '0' || h[5] != '1' || (h[3] != 'E' && h[3] != 'P' && h[3] != 'J'))
        return rr_fail(c, RR_E_FORMAT, "the image is not a supported Mario Kart Wii disc (ID %.6s; need RMCE01, RMCP01 or RMCJ01)", (char *)h);
    if (region_out) *region_out = (char)h[3];
    if (id_out) { memcpy(id_out, h, 6); id_out[6] = 0; }
    return 0;
}

/* ---- create-rr-*.sh cross-check --------------------------------------- */

typedef struct { char src_id[16], src_type[16], dest_id[16], dest_name[80], boot[8], ticket[8], tmd[8]; } script_cfg;

static int get_assign(const char *text, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > nl + 1 && !strncmp(p, name, nl) && p[nl] == '=') {
            const char *v = p + nl + 1; size_t vl = l - nl - 1;
            if (vl && v[vl - 1] == '\r') vl--;
            if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v++; vl -= 2; }
            if (vl >= cap) return -1;
            memcpy(out, v, vl); out[vl] = 0;
            return 0;
        }
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

static int get_opt(const char *text, const char *opt, char *out, size_t cap)
{
    const char *p = strstr(text, opt);
    if (!p) return -1;
    p += strlen(opt);
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && p[n] != '\\') n++;
    if (n >= cap) return -1;
    memcpy(out, p, n); out[n] = 0;
    return 0;
}

static int read_script_cfg(rr_ctx *c, const char *builder_dir, char region, script_cfg *sc, char **copy_script)
{
    char path[PATH_MAX]; char *text; size_t len;
    snprintf(path, sizeof path, "%s/%s", builder_dir, region_script(region));
    if (rr_read_text_file(path, &text, &len, 1 << 16)) return rr_fail(c, RR_E_IO, "cannot read builder script %s: %s (copy create-rr-usa.sh, create-rr-pal.sh, create-rr-jpn.sh and copy-files.sh into the builder folder)", path, strerror(errno));
    memset(sc, 0, sizeof *sc);
    int ok = !get_assign(text, "SRC_ID", sc->src_id, sizeof sc->src_id) && !get_assign(text, "SRC_TYPE", sc->src_type, sizeof sc->src_type) &&
             !get_assign(text, "DEST_ID", sc->dest_id, sizeof sc->dest_id) && !get_assign(text, "DEST_NAME", sc->dest_name, sizeof sc->dest_name) &&
             !get_opt(text, "--boot-id=", sc->boot, sizeof sc->boot) && !get_opt(text, "--ticket-id=", sc->ticket, sizeof sc->ticket) &&
             !get_opt(text, "--tmd-id=", sc->tmd, sizeof sc->tmd);
    char img[16];
    if (ok && (get_assign(text, "IMAGE_TYPE", img, sizeof img) || strcmp(img, "wbfs"))) ok = 0;
    /* every wit option on the copy line must be one we implement */
    if (ok) {
        const char *w = strstr(text, "wit copy");
        if (!w) ok = 0;
        else {
            char *copy = rr_xstrdup(w);
            for (char *q = copy; *q; q++) if (*q == '\\') *q = ' ';
            for (char *tok = strtok(copy, " \t\r\n"); tok && ok; tok = strtok(NULL, " \t\r\n")) {
                if (tok[0] != '-') continue;
                static const char *allowed[] = {"-T0", "--DEST", "-ovv", "--links", "--id", "--ticket-id=RMCR", "--tmd-id=RMCR", "--security-fix", "--name", "--$IMAGE_TYPE", "--split", NULL};
                int good = !strncmp(tok, "--boot-id=", 10);
                for (int i = 0; allowed[i] && !good; i++) good = !strcmp(tok, allowed[i]);
                if (!good) { rr_fail(c, RR_E_UNSUPPORTED, "builder script uses an unsupported wit option '%s'", tok); free(copy); free(text); return RR_E_UNSUPPORTED; }
            }
            free(copy);
        }
    }
    if (!ok || strcmp(sc->src_id, rr_region_src_id(region)) || (strcmp(sc->src_type, region_type(region)) && !(region == 'P' && !strcmp(sc->src_type, "PAL"))) || strcmp(sc->dest_id, rr_region_dest_id(region)) ||
        strlen(sc->boot) != 4 || strlen(sc->ticket) != 4 || strlen(sc->tmd) != 4 || memcmp(sc->boot, sc->src_id, 4) || strlen(sc->dest_name) > 63) {
        free(text);
        return rr_fail(c, RR_E_UNSUPPORTED, "builder script %s does not have the expected layout", region_script(region));
    }
    free(text);
    snprintf(path, sizeof path, "%s/copy-files.sh", builder_dir);
    if (rr_read_text_file(path, copy_script, &len, 1 << 20)) return rr_fail(c, RR_E_IO, "cannot read %s", path);
    return 0;
}

/* ---- fingerprint ---------------------------------------------------------- */

static void fp_add(rr_sha1_ctx *s, const char *label, const void *d, size_t n)
{
    uint8_t len[8]; rr_put_be32(len, (uint32_t)strlen(label)); rr_put_be32(len + 4, (uint32_t)n);
    rr_sha1_update(s, len, 8); rr_sha1_update(s, label, strlen(label)); rr_sha1_update(s, d, n);
}

static void hexs(const uint8_t *h, char *o) { for (int i = 0; i < 20; i++) sprintf(o + 2 * i, "%02x", h[i]); }

static int blob_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int fingerprint(rr_ctx *c, const rr_vtree *t, const char *base_path, const char *script, const script_cfg *sc, char region, char out_hex[41])
{
    rr_sha1_ctx s; rr_sha1_init(&s);
    rr_fileinfo st;
    if (rr_stat(base_path, 1, &st)) return rr_fail(c, RR_E_IO, "cannot read the base image's size: %s", strerror(errno));
    char buf[64];
    snprintf(buf, sizeof buf, "%llu:%lld", (unsigned long long)st.st_size, (long long)st.mtime);
    fp_add(&s, "base", buf, strlen(buf));
    fp_add(&s, "script", script, strlen(script));
    fp_add(&s, "dest", sc->dest_id, strlen(sc->dest_id));
    fp_add(&s, "name", sc->dest_name, strlen(sc->dest_name));
    fp_add(&s, "ids", sc->ticket, 4); fp_add(&s, "idb", sc->boot, 4);
    fp_add(&s, "region", &region, 1);
    fp_add(&s, "engine", "porpoise-1", 10);
    /* every host file the script pulled in, plus where it lands on the disc */
    size_t n = 0;
    char **lines = rr_xmalloc((t->nn + 4) * sizeof *lines);
    for (size_t i = 0; i < t->nn; i++) {
        if (t->n[i].is_dir) continue;
        const rr_blob *b = &t->b[t->n[i].blob];
        if (b->kind != RB_HOST) continue;
        if (rr_stat(b->path, 1, &st)) { for (size_t k = 0; k < n; k++) free(lines[k]); free(lines); return rr_fail(c, RR_E_IO, "input %s vanished", b->path); }
        char *l = rr_xmalloc(strlen(t->n[i].path) + strlen(b->path) + 96);
        sprintf(l, "%s<-%s:%llu:%lld", t->n[i].path, b->path, (unsigned long long)st.st_size, (long long)st.mtime);
        lines[n++] = l;
    }
    for (int k = 0; k < 2; k++) {
        int bi = k ? t->bi2_blob : t->dol_blob;
        if (bi >= 0 && t->b[bi].kind == RB_HOST) {
            if (rr_stat(t->b[bi].path, 1, &st)) { for (size_t q = 0; q < n; q++) free(lines[q]); free(lines); return rr_fail(c, RR_E_IO, "input vanished"); }
            char *l = rr_xmalloc(strlen(t->b[bi].path) + 96);
            sprintf(l, "sys%d<-%s:%llu:%lld", k, t->b[bi].path, (unsigned long long)st.st_size, (long long)st.mtime);
            lines[n++] = l;
        }
    }
    qsort(lines, n, sizeof *lines, blob_cmp);
    for (size_t i = 0; i < n; i++) { fp_add(&s, "in", lines[i], strlen(lines[i])); free(lines[i]); }
    free(lines);
    uint8_t h[20]; rr_sha1_final(&s, h); hexs(h, out_hex);
    return 0;
}

/* ---- meta file ------------------------------------------------------------- */

static void meta_path(const rr_patch_cfg *cfg, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.wbfs.meta", cfg->out_dir, rr_region_dest_id(cfg->region));
}
static void out_path(const rr_patch_cfg *cfg, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.wbfs", cfg->out_dir, rr_region_dest_id(cfg->region));
}

static int meta_get(const char *text, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > kl && !strncmp(p, key, kl) && p[kl] == '=') { if (l - kl - 1 >= cap) return -1; memcpy(out, p + kl + 1, l - kl - 1); out[l - kl - 1] = 0; return 0; }
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

static int meta_matches_file(const rr_patch_cfg *cfg, char *fp_out, size_t fpcap)
{
    char mp[PATH_MAX], op[PATH_MAX], *txt; size_t len;
    meta_path(cfg, mp, sizeof mp); out_path(cfg, op, sizeof op);
    rr_fileinfo st;
    if (rr_stat(op, 1, &st) || st.kind != 1) return 0;
    if (rr_read_text_file(mp, &txt, &len, 4096)) return 0;
    char sz[32], mt[32];
    int ok = !meta_get(txt, "fingerprint", fp_out, fpcap) && !meta_get(txt, "size", sz, sizeof sz) && !meta_get(txt, "mtime", mt, sizeof mt) &&
             strtoull(sz, NULL, 10) == (unsigned long long)st.st_size && strtoll(mt, NULL, 10) == (long long)st.mtime;
    free(txt);
    return ok;
}

int rr_patch_status_get(rr_ctx *c, const rr_patch_cfg *cfg, rr_patch_status *st)
{
    memset(st, 0, sizeof *st);
    rr_ctx tmp = *c;
    tmp.log = NULL; tmp.progress = NULL;
    char reg = 0, id[8];
    if (rr_detect_base(&tmp, cfg->base_dir, st->base_path, sizeof st->base_path, &reg, id) == 0) {
        st->base_found = 1; st->base_region = reg; memcpy(st->base_id, id, 8);
    } else {
        snprintf(st->base_error, sizeof st->base_error, "%s", tmp.err);
        st->base_found = st->base_path[0] != 0;
    }
    const char *dest = rr_region_dest_id(cfg->region);
    if (dest) {
        char op[PATH_MAX]; rr_fileinfo s;
        out_path(cfg, op, sizeof op);
        snprintf(st->out_path, sizeof st->out_path, "%s", op);
        if (!rr_stat(op, 1, &s) && s.kind == 1) {
            st->out_exists = 1; st->out_size = (uint64_t)s.st_size;
            char fp[64];
            st->out_current = meta_matches_file(cfg, fp, sizeof fp);
        }
    }
    return 0;
}

/* ---- main run ---------------------------------------------------------------- */

static void fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
}

/* The builder files may sit directly in builder_dir or inside one or two sub folders (e.g. the folder the zip unpacked to).
 * Finds the folder that holds copy-files.sh; on failure out = builder_dir and a description of what is there is written to why. */
static int find_builder_root(const char *builder_dir, char *out, size_t cap, char *why, size_t whycap)
{
    char probe[PATH_MAX];
    snprintf(out, cap, "%s", builder_dir);
    snprintf(probe, sizeof probe, "%s/copy-files.sh", builder_dir);
    if (rr_path_kind(probe) == 1) return 0;
    rr_dir *d = rr_opendir(builder_dir);
    why[0] = 0;
    int seen = 0;
    if (d) {
        const char *e;
        while ((e = rr_readdir(d))) {
            if (e[0] == '.') continue;
            char sub[PATH_MAX];
            snprintf(sub, sizeof sub, "%s/%s", builder_dir, e);
            if (seen < 8) { size_t l = strlen(why); snprintf(why + l, whycap - l, "%s%s", l ? ", " : "", e); seen++; }
            if (rr_path_kind(sub) != 2) continue;
            snprintf(probe, sizeof probe, "%s/copy-files.sh", sub);
            if (rr_path_kind(probe) == 1) { snprintf(out, cap, "%s", sub); rr_closedir(d); return 0; }
            rr_dir *d2 = rr_opendir(sub);
            if (!d2) continue;
            const char *e2;
            while ((e2 = rr_readdir(d2))) {
                if (e2[0] == '.') continue;
                char sub2[PATH_MAX];
                snprintf(sub2, sizeof sub2, "%s/%s", sub, e2);
                snprintf(probe, sizeof probe, "%s/copy-files.sh", sub2);
                if (rr_path_kind(sub2) == 2 && rr_path_kind(probe) == 1) { snprintf(out, cap, "%s", sub2); rr_closedir(d2); rr_closedir(d); return 0; }
            }
            rr_closedir(d2);
        }
        rr_closedir(d);
    }
    return -1;
}

int rr_patch_run(rr_ctx *c, const rr_patch_cfg *cfg_in, int force, rr_patch_result *res)
{
    rr_patch_cfg cfgcopy = *cfg_in;
    const rr_patch_cfg *cfg = &cfgcopy;
    char broot[PATH_MAX], why[300];
    if (find_builder_root(cfg_in->builder_dir, broot, sizeof broot, why, sizeof why) != 0)
        return rr_fail(c, RR_E_STATE, "builder files not found: %s has no copy-files.sh (it holds: %s). Copy the CONTENTS of the Retro Rewind ISO Builder (copy-files.sh, create-rr-pal.sh, extra/, Patches/ ...) into %s",
                       cfg_in->builder_dir, why[0] ? why : "nothing", cfg_in->builder_dir);
    cfgcopy.builder_dir = broot;
    if (strcmp(broot, cfg_in->builder_dir)) rr_logf(c, RR_LOG_INFO, "Using builder files in %s", broot);
    memset(res, 0, sizeof *res);
    const char *dest = rr_region_dest_id(cfg->region);
    if (!dest) return rr_fail(c, RR_E_USAGE, "choose a region (USA, PAL or Japan) first");
    char base_path[1024], reg, id[8];
    int r = rr_detect_base(c, cfg->base_dir, base_path, sizeof base_path, &reg, id);
    if (r) return r;
    if (reg != cfg->region) return rr_fail(c, RR_E_USAGE, "the base game is %s (%.6s) but you selected %s; pick the matching region", reg == 'E' ? "USA" : reg == 'P' ? "PAL" : "Japan", id, cfg->region == 'E' ? "USA" : cfg->region == 'P' ? "PAL" : "Japan");
    /* the pack must be present */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/RetroRewind6/version.txt", cfg->pack_dir);
    if (rr_path_kind(tmp) != 1) return rr_fail(c, RR_E_STATE, "Retro Rewind pack is not installed (no %s); run the update check first", tmp);
    script_cfg sc; char *script = NULL;
    if ((r = read_script_cfg(c, cfg->builder_dir, cfg->region, &sc, &script))) return r;
    snprintf(tmp, sizeof tmp, "%s/extra/Boot/Loaders/%s/main.dol", cfg->builder_dir, region_type(cfg->region));
    if (rr_path_kind(tmp) != 1) { free(script); return rr_fail(c, RR_E_STATE, "builder files are incomplete (missing %s)", tmp); }

    wbfs_img w; wii_part p; wii_fst f; rr_vtree t;
    memset(&p, 0, sizeof p); memset(&f, 0, sizeof f);
    rr_vt_init(&t);
    char opath[PATH_MAX], part[PATH_MAX], mpath[PATH_MAX], fp[41];
    out_path(cfg, opath, sizeof opath); meta_path(cfg, mpath, sizeof mpath);
    snprintf(part, sizeof part, "%s.part", opath);
    int wopen = 0, keep_part = 0;
    if (rr_mkdir_p(cfg->out_dir)) { r = rr_fail(c, RR_E_IO, "cannot create output folder %s", cfg->out_dir); goto out; }
    unlink(part); /* stale temp file from an interrupted run */

    rr_logf(c, RR_LOG_INFO, "Reading base image %s", base_path);
    if ((r = wbfs_img_open(c, &w, base_path))) goto out;
    wopen = 1;
    if ((r = wii_find_data_partition(c, &w, &p)) || (r = wii_read_fst(c, &p, &f))) goto out;
    if ((r = rr_vt_load_base(c, &t, &p, &f))) goto out;

    char v1[64], v2[64], v3[64], v4[128];
    snprintf(v1, sizeof v1, "SRC_ID=%s", sc.src_id); snprintf(v2, sizeof v2, "SRC_TYPE=%s", region_type(cfg->region)); /* the official builder says PAL for the loader folder but ships it as EUR */
    snprintf(v3, sizeof v3, "DEST_ID=%s", sc.dest_id); snprintf(v4, sizeof v4, "DEST_NAME=%s", sc.dest_name);
    const char *vars[] = {v1, v2, v3, v4, NULL};
    rr_script_roots roots = {cfg->builder_dir, cfg->pack_dir};
    rr_logf(c, RR_LOG_INFO, "Applying Retro Rewind files");
    if ((r = rr_script_run(c, &t, script, &roots, vars))) goto out;

    if ((r = fingerprint(c, &t, base_path, script, &sc, cfg->region, fp))) goto out;
    if (!force) {
        char old[64];
        if (meta_matches_file(cfg, old, sizeof old) && !strcmp(old, fp)) {
            res->skipped_up_to_date = 1;
            rr_fileinfo st; rr_stat(opath, 1, &st);
            res->out_size = (uint64_t)st.st_size;
            snprintf(res->out_path, sizeof res->out_path, "%s", opath);
            rr_logf(c, RR_LOG_INFO, "%s is already up to date; nothing to do", opath);
            goto out;
        }
    }
    /* free space: all distinct file bytes + slack */
    {
        uint64_t need = 0;
        for (size_t i = 0; i < t.nb; i++) need += t.b[i].size;
        need += need / 50 + (256ull << 20);
        uint64_t freeb = rr_free_space(cfg->out_dir);
        /* an existing output is replaced atomically, so both copies exist briefly */
        if (freeb && freeb < need) { r = rr_fail(c, RR_E_NOSPACE, "not enough free space in %s: need about %llu MiB, have %llu MiB", cfg->out_dir, (unsigned long long)(need >> 20), (unsigned long long)(freeb >> 20)); goto out; }
    }
    rr_build_cfg bc; memset(&bc, 0, sizeof bc);
    snprintf(bc.disc_id, sizeof bc.disc_id, "%s", sc.dest_id); snprintf(bc.boot_id, sizeof bc.boot_id, "%s", sc.boot);
    snprintf(bc.ticket_id, sizeof bc.ticket_id, "%s", sc.ticket); snprintf(bc.tmd_id, sizeof bc.tmd_id, "%s", sc.tmd);
    snprintf(bc.name, sizeof bc.name, "%s", sc.dest_name);
    rr_build_result br;
    rr_logf(c, RR_LOG_INFO, "Building %s (this takes a while)", dest);
    if ((r = wii_build_wbfs(c, &p, &t, &bc, part, &br))) goto out;
    rr_logf(c, RR_LOG_INFO, "Verifying the new image");
    uint64_t nfiles = 0;
    if ((r = rr_verify_wbfs(c, part, dest, br.plain_sha1, &nfiles))) { unlink(part); goto out; }
    /* the base must be unchanged underneath us */
    {
        rr_fileinfo st; char chk[64], was[64];
        rr_stat(base_path, 1, &st);
        snprintf(chk, sizeof chk, "%llu", (unsigned long long)st.st_size);
        snprintf(was, sizeof was, "%llu", (unsigned long long)w.file_size);
        if (strcmp(chk, was)) { unlink(part); r = rr_fail(c, RR_E_STATE, "the base image changed while patching; try again"); goto out; }
    }
    {
        char oname[64];
        snprintf(oname, sizeof oname, "%s.wbfs", dest);
        if (install_output(c, part, opath, cfg->out_dir, oname)) {
            r = rr_fail(c, RR_E_IO, "could not move the finished image into place: %s. The finished image was kept as %s", strerror(errno), part);
            keep_part = 1;
            goto out;
        }
    }
    fsync_dir(cfg->out_dir);
    {
        rr_fileinfo st; rr_stat(opath, 1, &st);
        char m[512];
        int n = snprintf(m, sizeof m, "fingerprint=%s\nsize=%llu\nmtime=%lld\nregion=%c\ndisc=%s\n", fp, (unsigned long long)st.st_size, (long long)st.mtime, cfg->region, dest);
        unlink(mpath);
        if (rr_write_text_file_atomic(mpath, m, (size_t)n)) rr_logf(c, RR_LOG_WARN, "could not write state file %s (the image itself is fine)", mpath);
        res->out_size = (uint64_t)st.st_size;
    }
    res->files = nfiles;
    snprintf(res->out_path, sizeof res->out_path, "%s", opath);
    rr_logf(c, RR_LOG_INFO, "Done: %s", opath);
    r = 0;
out:
    if (r && !keep_part) unlink(part);
    if (wopen) wbfs_img_close(&w);
    wii_fst_free(&f); wii_part_free(&p); rr_vt_free(&t);
    free(script);
    return r;
}

int rr_patch_verify_output(rr_ctx *c, const rr_patch_cfg *cfg)
{
    char op[PATH_MAX];
    out_path(cfg, op, sizeof op);
    if (rr_path_kind(op) != 1) return rr_fail(c, RR_E_USAGE, "%s does not exist yet", op);
    uint64_t n;
    return rr_verify_wbfs(c, op, rr_region_dest_id(cfg->region), NULL, &n);
}
