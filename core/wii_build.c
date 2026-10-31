/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "wii_build.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ================================================================ vtree */

void rr_vt_init(rr_vtree *t) { memset(t, 0, sizeof *t); t->dol_blob = t->bi2_blob = -1; }

static void blob_clear(rr_blob *b) { free(b->path); free(b->mem); memset(b, 0, sizeof *b); }

void rr_vt_free(rr_vtree *t)
{
    for (size_t i = 0; i < t->nn; i++) free(t->n[i].path);
    for (size_t i = 0; i < t->nb; i++) blob_clear(&t->b[i]);
    free(t->n); free(t->b);
    rr_vt_init(t);
}

static int new_blob(rr_vtree *t)
{
    if (t->nb == t->capb) { t->capb = t->capb ? t->capb * 2 : 1024; t->b = rr_xrealloc(t->b, t->capb * sizeof *t->b); }
    memset(&t->b[t->nb], 0, sizeof t->b[0]);
    return (int)t->nb++;
}

static int new_node(rr_vtree *t, const char *path, int is_dir, int blob)
{
    if (t->nn == t->capn) { t->capn = t->capn ? t->capn * 2 : 1024; t->n = rr_xrealloc(t->n, t->capn * sizeof *t->n); }
    t->n[t->nn].path = rr_xstrdup(path);
    t->n[t->nn].is_dir = is_dir;
    t->n[t->nn].blob = blob;
    return (int)t->nn++;
}

int rr_vt_find(const rr_vtree *t, const char *path)
{
    for (size_t i = 0; i < t->nn; i++) if (!strcmp(t->n[i].path, path)) return (int)i;
    return -1;
}

size_t rr_vt_count_files(const rr_vtree *t)
{
    size_t n = 0;
    for (size_t i = 0; i < t->nn; i++) n += !t->n[i].is_dir;
    return n;
}

/* a path is acceptable if every component is non-empty, not "." / "..", has no control bytes, and <=255 chars */
static int path_ok(const char *p)
{
    if (!*p) return 0;
    const char *s = p;
    for (;;) {
        const char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == 0 || l > 255) return 0;
        if ((l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return 0;
        for (size_t i = 0; i < l; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return 0;
        if (!e) break;
        s = e + 1;
    }
    return strlen(p) < 900;
}

int rr_vt_mkdir_p(rr_ctx *c, rr_vtree *t, const char *path)
{
    if (!path_ok(path)) return rr_fail(c, RR_E_UNSAFE, "invalid disc path '%s'", path);
    char *tmp = rr_xstrdup(path);
    for (char *p = tmp;; p++) {
        if (*p == '/' || *p == 0) {
            char save = *p;
            *p = 0;
            int i = rr_vt_find(t, tmp);
            if (i < 0) new_node(t, tmp, 1, -1);
            else if (!t->n[i].is_dir) { int r = rr_fail(c, RR_E_FORMAT, "'%s' exists as a file; cannot make it a directory", tmp); free(tmp); return r; }
            *p = save;
            if (!save) break;
        }
    }
    free(tmp);
    return 0;
}

int rr_vt_put_host(rr_ctx *c, rr_vtree *t, const char *path, const char *host, uint64_t size)
{
    if (!path_ok(path)) return rr_fail(c, RR_E_UNSAFE, "invalid disc path '%s'", path);
    if (size > 0xFFFFFFFFull) return rr_fail(c, RR_E_UNSUPPORTED, "%s is larger than 4 GiB and cannot be stored on a Wii disc", host);
    int i = rr_vt_find(t, path);
    if (i >= 0) {
        if (t->n[i].is_dir) return rr_fail(c, RR_E_FORMAT, "'%s' is a directory on the disc", path);
        rr_blob *b = &t->b[t->n[i].blob];
        blob_clear(b);
        b->kind = RB_HOST; b->path = rr_xstrdup(host); b->size = size;
        return 0;
    }
    const char *sl = strrchr(path, '/');
    if (sl) {
        char *dir = rr_xstrdup(path);
        dir[sl - path] = 0;
        int j = rr_vt_find(t, dir);
        free(dir);
        if (j < 0) return rr_fail(c, RR_E_FORMAT, "parent directory of '%s' does not exist on the disc", path);
    }
    int b = new_blob(t);
    t->b[b].kind = RB_HOST; t->b[b].path = rr_xstrdup(host); t->b[b].size = size;
    new_node(t, path, 0, b);
    return 0;
}

static uint32_t dol_size_from_header(const uint8_t *h)
{
    uint32_t mx = 0x100;
    for (int i = 0; i < 18; i++) {
        uint32_t off = rr_be32(h + 4 * i), sz = rr_be32(h + 0x90 + 4 * i);
        if (sz && off + sz > mx) mx = off + sz;
    }
    return mx;
}

int rr_vt_put_sys_host(rr_ctx *c, rr_vtree *t, const char *name, const char *host, uint64_t size)
{
    int *slot;
    if (!strcmp(name, "main.dol")) {
        slot = &t->dol_blob;
        if (size < 0x100 || size > 0x1000000) return rr_fail(c, RR_E_FORMAT, "%s is not a plausible main.dol (%llu bytes)", host, (unsigned long long)size);
    } else if (!strcmp(name, "bi2.bin")) {
        slot = &t->bi2_blob;
        if (size != 0x2000) return rr_fail(c, RR_E_FORMAT, "%s must be exactly 8192 bytes to replace bi2.bin", host);
    } else return rr_fail(c, RR_E_UNSUPPORTED, "replacing sys/%s is not supported", name);
    /* new standalone blob (sys files are never hard-linked) */
    int b = new_blob(t);
    t->b[b].kind = RB_HOST; t->b[b].path = rr_xstrdup(host); t->b[b].size = size;
    *slot = b;
    return 0;
}

static int read_blob_mem(rr_ctx *c, rr_blob *b)
{
    if (b->kind == RB_MEM) return 0;
    if (b->kind != RB_HOST) return rr_fail(c, RR_E_USAGE, "internal: blob is not a host file");
    uint8_t *m = rr_xmalloc(b->size ? b->size : 1);
    int fd = open(b->path, O_RDONLY);
    if (fd < 0) { free(m); return rr_fail(c, RR_E_IO, "cannot open %s", b->path); }
    int r = rr_pread_full(fd, m, b->size, 0);
    close(fd);
    if (r) { free(m); return rr_fail(c, RR_E_IO, "cannot read %s", b->path); }
    free(b->path); b->path = NULL;
    b->mem = m; b->kind = RB_MEM;
    return 0;
}

int rr_vt_patch_dol(rr_ctx *c, rr_vtree *t, uint32_t addr, const uint8_t *bytes, size_t n)
{
    if (t->dol_blob < 0) return rr_fail(c, RR_E_FORMAT, "dolpatch: no main.dol present");
    rr_blob *b = &t->b[t->dol_blob];
    if (b->kind == RB_BASE) return rr_fail(c, RR_E_USAGE, "dolpatch: main.dol has not been replaced yet");
    int r = read_blob_mem(c, b);
    if (r) return r;
    if (b->size < 0x100) return rr_fail(c, RR_E_FORMAT, "dolpatch: main.dol is too small");
    for (int i = 0; i < 18; i++) {
        uint32_t off = rr_be32(b->mem + 4 * i), a = rr_be32(b->mem + 0x48 + 4 * i), sz = rr_be32(b->mem + 0x90 + 4 * i);
        if (!sz) continue;
        if ((uint64_t)addr >= a && (uint64_t)addr + n <= (uint64_t)a + sz) {
            uint64_t fo = (uint64_t)off + (addr - a);
            if (fo + n > b->size) return rr_fail(c, RR_E_FORMAT, "dolpatch: section extends beyond the end of main.dol");
            memcpy(b->mem + fo, bytes, n);
            rr_logf(c, RR_LOG_INFO, "main.dol patched at %08x (+%zu bytes, file offset %llx)", addr, n, (unsigned long long)fo);
            return 0;
        }
    }
    return rr_fail(c, RR_E_FORMAT, "dolpatch: address %08x is not inside any main.dol section", addr);
}

typedef struct { uint64_t off; uint32_t size; size_t i; } lk_item;
static int lk_cmp(const void *a, const void *b)
{
    const lk_item *x = a, *y = b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

int rr_vt_load_base(rr_ctx *c, rr_vtree *t, wii_part *p, const wii_fst *f)
{
    /* hard-link groups: identical non-empty extents share one blob (what `wit extract --links` produces) */
    size_t nf = 0;
    lk_item *idx = rr_xmalloc((f->n ? f->n : 1) * sizeof *idx);
    for (size_t i = 0; i < f->n; i++)
        if (!f->e[i].is_dir && f->e[i].size) { idx[nf].off = f->e[i].offset; idx[nf].size = f->e[i].size; idx[nf].i = i; nf++; }
    qsort(idx, nf, sizeof *idx, lk_cmp);
    int *blob_of = rr_xmalloc((f->n ? f->n : 1) * sizeof *blob_of);
    for (size_t i = 0; i < f->n; i++) blob_of[i] = -1;
    for (size_t k = 0; k < nf; k++) {
        if (k && idx[k - 1].off == idx[k].off && idx[k - 1].size == idx[k].size) { blob_of[idx[k].i] = blob_of[idx[k - 1].i]; continue; }
        int b = new_blob(t);
        t->b[b].kind = RB_BASE; t->b[b].base_off = idx[k].off; t->b[b].size = idx[k].size;
        blob_of[idx[k].i] = b;
    }
    free(idx);
    for (size_t i = 0; i < f->n; i++) {
        const wii_fst_entry *e = &f->e[i];
        if (!path_ok(e->name)) { free(blob_of); return rr_fail(c, RR_E_FORMAT, "base image contains an unusable path name"); }
        if (e->is_dir) { new_node(t, e->name, 1, -1); continue; }
        int b = blob_of[i];
        if (b < 0) { b = new_blob(t); t->b[b].kind = RB_BASE; t->b[b].base_off = e->offset; t->b[b].size = 0; }
        new_node(t, e->name, 0, b);
    }
    free(blob_of);
    /* sys files from the base: main.dol, bi2.bin */
    uint8_t hdr[0x100];
    int r = wii_part_read(c, p, f->dol_off, hdr, sizeof hdr);
    if (r) return r;
    int b = new_blob(t);
    t->b[b].kind = RB_BASE; t->b[b].base_off = f->dol_off; t->b[b].size = dol_size_from_header(hdr);
    t->dol_blob = b;
    b = new_blob(t);
    t->b[b].kind = RB_BASE; t->b[b].base_off = 0x440; t->b[b].size = 0x2000;
    t->bi2_blob = b;
    return 0;
}

/* ============================================================ ordering */

int nintendo_cmp(const char *path1, const char *path2)
{
    static unsigned char tr[0x100];
    if (!tr[1]) {
        unsigned idx = 1, i;
        tr['/'] = (unsigned char)idx++;
        tr['.'] = (unsigned char)idx++;
        for (i = 1; i < 'A'; i++) if (!tr[i]) tr[i] = (unsigned char)idx++;
        for (i = 'A'; i <= 'Z'; i++) tr[i] = tr[i - 'A' + 'a'] = (unsigned char)idx++;
        for (i = 1; i < sizeof tr; i++) if (!tr[i]) tr[i] = (unsigned char)idx++;
    }
    while (*path1 == *path2) {
        if (!*path1) return 0;
        path1++; path2++;
    }
    const unsigned char *p1 = (const unsigned char *)path1, *p2 = (const unsigned char *)path2;
    while (*p1 || *p2) {
        int c1 = tr[*p1++], c2 = tr[*p2++];
        int st = c1 - c2;
        if (st) return st;
        if (c1 == 1) break;
    }
    return (unsigned char)*path1 - (unsigned char)*path2;
}

typedef struct { char *key; const rr_node *n; } sort_item;

static int sort_cmp(const void *a, const void *b)
{
    return nintendo_cmp(((const sort_item *)a)->key, ((const sort_item *)b)->key);
}

/* ============================================================ builder */

typedef struct {
    uint64_t off, len;
    int kind;             /* RB_* */
    uint64_t base_off;
    const char *path;
    const uint8_t *mem;
} extent;

typedef struct {
    int fd;
    uint64_t blocks_map_first, blocks_map_last; /* virtual wbfs block range of the partition */
    uint32_t wbfs_sec;
} outfile;

#define WBFS_BLOCK 0x200000ull

static uint64_t phys_of(const outfile *o, uint64_t voff)
{
    uint64_t vb = voff / WBFS_BLOCK, in = voff % WBFS_BLOCK;
    if (vb == 0) return WBFS_BLOCK + in;
    return (1 + 1 + (vb - o->blocks_map_first)) * WBFS_BLOCK + in;
}

static int vwrite(rr_ctx *c, const outfile *o, uint64_t voff, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        uint64_t in = voff % WBFS_BLOCK;
        size_t take = (size_t)(WBFS_BLOCK - in);
        if (take > n) take = n;
        uint64_t vb = voff / WBFS_BLOCK;
        if (vb != 0 && (vb < o->blocks_map_first || vb > o->blocks_map_last)) return rr_fail(c, RR_E_USAGE, "internal: write outside mapped blocks");
        if (rr_pwrite_full(o->fd, p, take, phys_of(o, voff))) return rr_fail(c, RR_E_IO, "write error: %s", strerror(errno));
        p += take; voff += take; n -= take;
    }
    return 0;
}

static void patch_id(uint8_t *dst, const char *id, size_t n) { memcpy(dst, id, n); }

static void set_title(uint8_t *dst64, const char *name)
{
    memset(dst64, 0, 64);
    size_t l = strlen(name);
    memcpy(dst64, name, l > 63 ? 63 : l);
}

int wii_build_wbfs(rr_ctx *c, wii_part *base, rr_vtree *t, const rr_build_cfg *cfg, const char *out_path, rr_build_result *res)
{
    int r = 0;
    memset(res, 0, sizeof *res);
    const wbfs_img *img = base->img;
    extent *ext = NULL; size_t next = 0, capext = 0;
    sort_item *items = NULL; size_t nitems = 0;
    uint8_t *fst = NULL; uint8_t *h3 = NULL;
    uint8_t *gdec = NULL, *ghdr = NULL, *genc = NULL;
    outfile of = {.fd = -1};
    int cur_fd = -1; size_t cur_fd_ext = (size_t)-1;
    uint64_t n_items_dummy = 0; (void)n_items_dummy;
    rr_sha1_ctx plain;

    /* ---- boot.bin from base, patched --------------------------------- */
    uint8_t boot[0x440];
    if ((r = wii_part_read(c, base, 0, boot, sizeof boot))) return r;
    uint32_t base_dol4 = rr_be32(boot + 0x420), base_fst4 = rr_be32(boot + 0x424);

    /* ---- apploader --------------------------------------------------- */
    uint8_t aph[0x20];
    if ((r = wii_part_read(c, base, 0x2440, aph, sizeof aph))) return r;
    uint64_t app_size = 0x20 + (uint64_t)rr_be32(aph + 0x14) + rr_be32(aph + 0x18);
    if (app_size > 0x1000000 || 0x2440 + app_size > wii_part_data_capacity(base)) return rr_fail(c, RR_E_FORMAT, "apploader size is implausible");

    uint64_t cur4 = (0x2440 >> 2) + ((app_size + 3) >> 2);

    /* ---- dol --------------------------------------------------------- */
    if (t->dol_blob < 0 || t->bi2_blob < 0) return rr_fail(c, RR_E_USAGE, "internal: sys files missing");
    rr_blob *dol = &t->b[t->dol_blob];
    if (base_dol4 >= cur4 && base_dol4 <= cur4 + 0x100) cur4 = base_dol4;
    uint32_t dol4 = (uint32_t)cur4;
    uint64_t dol_dest = (uint64_t)cur4 << 2;
    cur4 += (dol->size + 3) >> 2;
    dol->dest_off = dol_dest;

    /* ---- sorted node list -------------------------------------------- */
    items = rr_xmalloc((t->nn ? t->nn : 1) * sizeof *items);
    for (size_t i = 0; i < t->nn; i++) {
        size_t l = strlen(t->n[i].path);
        char *k = rr_xmalloc(l + 8);
        memcpy(k, "files/", 6); memcpy(k + 6, t->n[i].path, l);
        size_t kl = 6 + l;
        if (t->n[i].is_dir) k[kl++] = '/';
        k[kl] = 0;
        items[nitems].key = k; items[nitems].n = &t->n[i]; nitems++;
    }
    qsort(items, nitems, sizeof *items, sort_cmp);

    /* ---- FST ---------------------------------------------------------- */
    uint32_t file_used = (uint32_t)nitems + 1;
    uint64_t pool = 0;
    for (size_t i = 0; i < nitems; i++) {
        const char *p = items[i].n->path, *s = strrchr(p, '/');
        pool += strlen(s ? s + 1 : p) + 1;
    }
    pool = (pool + 3) & ~3ull;
    uint64_t ftab_size = 12ull * file_used + pool;
    if (ftab_size > (16u << 20)) { r = rr_fail(c, RR_E_FORMAT, "file table too large"); goto done; }
    fst = rr_xmalloc(ftab_size); memset(fst, 0, ftab_size);
    uint32_t fst4 = (uint32_t)cur4;
    if (base_fst4 >= cur4 && base_fst4 <= cur4 + 0x100) fst4 = base_fst4;
    cur4 = fst4;
    uint64_t fst_dest = (uint64_t)fst4 << 2;
    cur4 += ftab_size >> 2;
    rr_put_be32(fst, 0x01000000); rr_put_be32(fst + 4, 0); rr_put_be32(fst + 8, file_used);
    if (cur4 < 0x7C000) cur4 = 0x7C000; else cur4 = (cur4 + 15) & ~15ull;
    uint64_t off4 = cur4;

    {
        char *namebase = (char *)fst + 12ull * file_used, *np = namebase;
        /* parent tracking via a stack of directory paths */
        struct { const char *key; size_t klen; uint32_t idx; } stk[64]; int sp = 0;
        stk[0].key = "files/"; stk[0].klen = 6; stk[0].idx = 0;
        for (size_t i = 0; i < nitems; i++) {
            const rr_node *n = items[i].n;
            uint32_t idx = (uint32_t)i + 1;
            while (sp > 0 && strncmp(items[i].key, stk[sp].key, stk[sp].klen)) sp--;
            uint8_t *e = fst + 12ull * idx;
            const char *s = strrchr(n->path, '/');
            const char *nm = s ? s + 1 : n->path;
            rr_put_be32(e, (uint32_t)(np - namebase) | (n->is_dir ? 0x01000000u : 0));
            memcpy(np, nm, strlen(nm)); np += strlen(nm) + 1;
            if (n->is_dir) {
                size_t kl = strlen(items[i].key), cnt = 0;
                for (size_t j = i + 1; j < nitems && !strncmp(items[j].key, items[i].key, kl); j++) cnt++;
                rr_put_be32(e + 4, stk[sp].idx);
                rr_put_be32(e + 8, idx + 1 + (uint32_t)cnt);
                if (sp >= 62) { r = rr_fail(c, RR_E_FORMAT, "directory nesting too deep"); goto done; }
                sp++; stk[sp].key = items[i].key; stk[sp].klen = kl; stk[sp].idx = idx;
            } else {
                rr_blob *b = &t->b[n->blob];
                if (b->size > 0xFFFFFFFFull) { r = rr_fail(c, RR_E_UNSUPPORTED, "file %s is too large", n->path); goto done; }
                if (!b->placed) {
                    b->placed = 1; b->dest_off = off4 << 2;
                    off4 += (b->size + 3) >> 2;
                    if (b->size) {
                        if (next == capext) { capext = capext ? capext * 2 : 4096; ext = rr_xrealloc(ext, capext * sizeof *ext); }
                        ext[next].off = b->dest_off; ext[next].len = b->size; ext[next].kind = b->kind;
                        ext[next].base_off = b->base_off; ext[next].path = b->path; ext[next].mem = b->mem; next++;
                    }
                }
                rr_put_be32(e + 4, (uint32_t)(b->dest_off >> 2));
                rr_put_be32(e + 8, (uint32_t)b->size);
                res->file_count++;
            }
        }
    }
    uint64_t end_bytes = off4 << 2;
    uint32_t blocks = (uint32_t)((off4 - 1) / 0x1F00 + 1);
    if ((uint64_t)blocks * WII_DATA_BYTES < end_bytes) blocks++;
    uint32_t groups = (blocks - 1) / 64 + 1;
    if (groups > 4915) { r = rr_fail(c, RR_E_NOSPACE, "the patched game does not fit on a Wii disc"); goto done; }
    uint64_t part_off = base->part_off;
    uint64_t part_end = part_off + 0x20000 + (uint64_t)blocks * WII_SECTOR;
    if (part_end > WII_MAX_DISC_SIZE) { r = rr_fail(c, RR_E_NOSPACE, "the patched game (%llu MiB) is larger than a single-layer Wii disc", (unsigned long long)(part_end >> 20)); goto done; }

    /* ---- boot.bin patch ------------------------------------------------ */
    patch_id(boot, cfg->boot_id, 4);
    set_title(boot + 0x20, cfg->name);
    rr_put_be32(boot + 0x420, dol4);
    rr_put_be32(boot + 0x424, fst4);
    rr_put_be32(boot + 0x428, (uint32_t)(ftab_size >> 2));
    rr_put_be32(boot + 0x42c, (uint32_t)(ftab_size >> 2));

    /* ---- remaining fixed extents (boot, bi2, apploader, dol, fst), sorted first --------- */
    {
        extent fixed[5]; int nf = 0;
        rr_blob *bi2 = &t->b[t->bi2_blob];
        fixed[nf++] = (extent){0, 0x440, RB_MEM, 0, NULL, boot};
        fixed[nf++] = (extent){0x440, 0x2000, bi2->kind, bi2->base_off, bi2->path, bi2->mem};
        fixed[nf++] = (extent){0x2440, app_size, RB_BASE, 0x2440, NULL, NULL};
        fixed[nf++] = (extent){dol_dest, dol->size, dol->kind, dol->base_off, dol->path, dol->mem};
        fixed[nf++] = (extent){fst_dest, ftab_size, RB_MEM, 0, NULL, fst};
        if (next + 5 > capext) { capext = next + 5; ext = rr_xrealloc(ext, capext * sizeof *ext); }
        memmove(ext + nf, ext, next * sizeof *ext);
        memcpy(ext, fixed, nf * sizeof *ext);
        next += nf;
        for (size_t i = 1; i < next; i++)
            if (ext[i].off < ext[i - 1].off + ext[i - 1].len) { r = rr_fail(c, RR_E_STATE, "internal: overlapping extents"); goto done; }
    }

    /* ---- output file --------------------------------------------------- */
    uint64_t pb_first = part_off / WBFS_BLOCK, pb_last = (part_end - 1) / WBFS_BLOCK;
    if (pb_first == 0) { r = rr_fail(c, RR_E_FORMAT, "unsupported partition position"); goto done; }
    uint32_t nmapped = 1 + (uint32_t)(pb_last - pb_first + 1);
    if (nmapped > 4400) { r = rr_fail(c, RR_E_FORMAT, "too many WBFS blocks"); goto done; }
    res->wbfs_size = (uint64_t)(1 + nmapped) * WBFS_BLOCK;
    res->data_bytes = end_bytes; res->clusters = blocks;
    of.fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (of.fd < 0) { r = rr_fail(c, RR_E_IO, "cannot create %s: %s", out_path, strerror(errno)); goto done; }
    of.blocks_map_first = pb_first; of.blocks_map_last = pb_last;
    if (ftruncate(of.fd, (off_t)res->wbfs_size) != 0) { r = rr_fail(c, RR_E_NOSPACE, "cannot allocate %llu MiB for the output: %s", (unsigned long long)(res->wbfs_size >> 20), strerror(errno)); goto done; }

    /* ---- disc header area (virtual block 0) ----------------------------- */
    uint8_t dhead[0x100], blk0[0x50000];
    memset(blk0, 0, sizeof blk0);
    if ((r = wbfs_img_read(c, img, 0, blk0, 0x60))) goto done;
    if ((r = wbfs_img_read(c, img, 0x40000, blk0 + 0x40000, 0x40))) goto done;
    if ((r = wbfs_img_read(c, img, 0x4E000, blk0 + 0x4E000, 0x20))) goto done;
    if ((r = wbfs_img_read(c, img, 0x4FFFC, blk0 + 0x4FFFC, 4))) goto done;
    patch_id(blk0, cfg->disc_id, 6);
    set_title(blk0 + 0x20, cfg->name);
    if (rr_be32(blk0 + 0x18) != 0x5D1C9EA3u) { r = rr_fail(c, RR_E_FORMAT, "base disc lacks the Wii magic word"); goto done; }
    memcpy(dhead, blk0, sizeof dhead); /* only the first 0x100 bytes are mirrored into WBFS */
    if ((r = vwrite(c, &of, 0, blk0, sizeof blk0))) goto done;

    /* ---- stream the data clusters ---------------------------------------- */
    uint8_t ticket[WII_TICKET_SIZE];
    memcpy(ticket, base->ticket, sizeof ticket);
    patch_id(ticket + 0x1dc + 4, cfg->ticket_id, 4);
    uint8_t key[16];
    wii_decrypt_title_key(ticket, key);
    rr_aes128 aes; rr_aes128_set_key(&aes, key);

    gdec = rr_xmalloc(64ull * WII_DATA_BYTES);
    ghdr = rr_xmalloc(64ull * WII_HASH_BYTES);
    genc = rr_xmalloc(64ull * WII_SECTOR);
    h3 = rr_xmalloc(WII_H3_SIZE); memset(h3, 0, WII_H3_SIZE);
    rr_sha1_init(&plain);

    size_t ei = 0;
    for (uint32_t g = 0; g < groups; g++) {
        if (rr_cancelled(c)) { r = RR_E_CANCEL; rr_fail(c, RR_E_CANCEL, "cancelled"); goto done; }
        uint32_t nc = blocks - g * 64; if (nc > 64) nc = 64;
        uint64_t gs = (uint64_t)g * 64 * WII_DATA_BYTES, ge = gs + (uint64_t)nc * WII_DATA_BYTES;
        memset(gdec, 0, 64ull * WII_DATA_BYTES); /* missing tail clusters are hashed as all-zero clusters (as wit does) */
        memset(ghdr, 0, 64ull * WII_HASH_BYTES);
        while (ei < next && ext[ei].off + ext[ei].len <= gs) {
            if (cur_fd >= 0 && cur_fd_ext == ei) { close(cur_fd); cur_fd = -1; }
            ei++;
        }
        for (size_t k = ei; k < next && ext[k].off < ge; k++) {
            uint64_t a = ext[k].off > gs ? ext[k].off : gs;
            uint64_t b = ext[k].off + ext[k].len < ge ? ext[k].off + ext[k].len : ge;
            uint8_t *dst = gdec + (a - gs);
            uint64_t src = a - ext[k].off;
            if (ext[k].kind == RB_MEM) memcpy(dst, ext[k].mem + src, (size_t)(b - a));
            else if (ext[k].kind == RB_BASE) { if ((r = wii_part_read(c, base, ext[k].base_off + src, dst, (size_t)(b - a)))) goto done; }
            else {
                if (cur_fd >= 0 && cur_fd_ext != k) { close(cur_fd); cur_fd = -1; }
                if (cur_fd < 0) {
                    cur_fd = open(ext[k].path, O_RDONLY);
                    if (cur_fd < 0) { r = rr_fail(c, RR_E_IO, "cannot open %s: %s", ext[k].path, strerror(errno)); goto done; }
                    cur_fd_ext = k;
                    uint64_t cur_sz = 0;
                    if (rr_fd_size(cur_fd, &cur_sz) || cur_sz != ext[k].len) { r = rr_fail(c, RR_E_STATE, "%s changed while patching", ext[k].path); goto done; }
                }
                if (rr_pread_full(cur_fd, dst, (size_t)(b - a), src)) { r = rr_fail(c, RR_E_IO, "read error in %s", ext[k].path); goto done; }
            }
        }
        rr_sha1_update(&plain, gdec, (size_t)nc * WII_DATA_BYTES);

        /* hash tree for this group (same arithmetic as wit's EncryptSectorGroup) */
        for (uint32_t s = 0; s < 64; s++)
            for (int j = 0; j < 31; j++) rr_sha1(gdec + (size_t)s * WII_DATA_BYTES + (size_t)j * 0x400, 0x400, ghdr + (size_t)s * WII_HASH_BYTES + j * 20);
        for (uint32_t s0 = 0; s0 < 64; s0 += 8) {
            uint8_t *store = ghdr + (size_t)s0 * WII_HASH_BYTES + 0x280;
            for (uint32_t j = 0; j < 8; j++) rr_sha1(ghdr + (size_t)(s0 + j) * WII_HASH_BYTES, 31 * 20, store + j * 20);
            for (uint32_t j = 1; j < 8; j++) memcpy(ghdr + (size_t)(s0 + j) * WII_HASH_BYTES + 0x280, store, 160);
        }
        for (uint32_t s0 = 0, j = 0; s0 < 64; s0 += 8, j++) rr_sha1(ghdr + (size_t)s0 * WII_HASH_BYTES + 0x280, 160, ghdr + 0x340 + j * 20);
        for (uint32_t s = 1; s < 64; s++) memcpy(ghdr + (size_t)s * WII_HASH_BYTES + 0x340, ghdr + 0x340, 160);
        rr_sha1(ghdr + 0x340, 160, h3 + (size_t)g * 20);

        /* encrypt + write */
        for (uint32_t s = 0; s < nc; s++) {
            uint8_t iv0[16] = {0};
            uint8_t *eo = genc + (size_t)s * WII_SECTOR;
            rr_aes128_cbc_encrypt(&aes, iv0, ghdr + (size_t)s * WII_HASH_BYTES, eo, WII_HASH_BYTES);
            uint8_t iv[16]; memcpy(iv, eo + 0x3d0, 16);
            rr_aes128_cbc_encrypt(&aes, iv, gdec + (size_t)s * WII_DATA_BYTES, eo + WII_HASH_BYTES, WII_DATA_BYTES);
        }
        if ((r = vwrite(c, &of, part_off + 0x20000 + (uint64_t)g * 64 * WII_SECTOR, genc, (size_t)nc * WII_SECTOR))) goto done;
        rr_progress(c, "Building disc image", (uint64_t)g * 64 + nc, blocks);
    }
    rr_sha1_final(&plain, res->plain_sha1);

    /* ---- partition header: ticket, TMD, cert, H3 --------------------------- */
    {
        uint8_t *ph = rr_xmalloc(0x8000); memset(ph, 0, 0x8000);
        uint8_t *tmd = rr_xmalloc(base->tmd_size);
        memcpy(tmd, base->tmd, base->tmd_size);
        if (base->tmd_size != 0x208) { free(ph); free(tmd); r = rr_fail(c, RR_E_FORMAT, "unexpected TMD size %u (single-content TMD required)", base->tmd_size); goto done; }
        patch_id(tmd + 0x18c + 4, cfg->tmd_id, 4);
        rr_sha1(h3, WII_H3_SIZE, tmd + 0x1f4);
        wii_fake_sign_tmd(tmd, base->tmd_size);
        wii_fake_sign_ticket(ticket);
        uint32_t tmd_off = 0x2c0, cert_off = (tmd_off + base->tmd_size + 0x1f) & ~0x1fu;
        if (cert_off + base->cert_size > 0x8000) { free(ph); free(tmd); r = rr_fail(c, RR_E_FORMAT, "certificate chain too large"); goto done; }
        memcpy(ph, ticket, sizeof ticket);
        rr_put_be32(ph + 0x2a4, base->tmd_size); rr_put_be32(ph + 0x2a8, tmd_off >> 2);
        rr_put_be32(ph + 0x2ac, base->cert_size); rr_put_be32(ph + 0x2b0, cert_off >> 2);
        rr_put_be32(ph + 0x2b4, 0x8000 >> 2); rr_put_be32(ph + 0x2b8, 0x20000 >> 2);
        rr_put_be32(ph + 0x2bc, (uint32_t)(((uint64_t)blocks * WII_SECTOR) >> 2));
        memcpy(ph + tmd_off, tmd, base->tmd_size);
        memcpy(ph + cert_off, base->cert, base->cert_size);
        r = vwrite(c, &of, part_off, ph, 0x8000);
        free(ph); free(tmd);
        if (r) goto done;
        if ((r = vwrite(c, &of, part_off + 0x8000, h3, WII_H3_SIZE))) goto done;
    }

    /* ---- WBFS header, disc info, wlba, free-block map ---------------------- */
    {
        uint8_t *b0 = rr_xmalloc(WBFS_BLOCK); memset(b0, 0, WBFS_BLOCK);
        memcpy(b0, "WBFS", 4);
        rr_put_be32(b0 + 4, (uint32_t)(res->wbfs_size / 512));
        b0[8] = 9; b0[9] = 21; b0[10] = 1; b0[12] = 1;
        memcpy(b0 + 0x200, dhead, 0x100);
        uint32_t n_wlba = 143432u * 2u >> 6;
        uint16_t phys = 1;
        rr_put_be16(b0 + 0x300 + 0, phys++);
        for (uint64_t vb = pb_first; vb <= pb_last; vb++) rr_put_be16(b0 + 0x300 + 2 * vb, phys++);
        (void)n_wlba;
        /* free-block bitmap: 128 words, bit i of the big-endian word array <=> block i+1, set = free */
        for (uint32_t w = 0; w < 128; w++) {
            uint32_t v = 0;
            for (uint32_t bit = 0; bit < 32; bit++) if (w * 32 + bit + 1 > nmapped) v |= 1u << bit;
            rr_put_be32(b0 + 0x1ffc00 + 4 * w, v);
        }
        r = rr_pwrite_full(of.fd, b0, WBFS_BLOCK, 0) ? rr_fail(c, RR_E_IO, "write error: %s", strerror(errno)) : 0;
        free(b0);
        if (r) goto done;
    }
    if (fsync(of.fd) != 0) { r = rr_fail(c, RR_E_IO, "fsync failed: %s", strerror(errno)); goto done; }
    r = 0;
done:
    if (cur_fd >= 0) close(cur_fd);
    if (of.fd >= 0) close(of.fd);
    for (size_t i = 0; i < nitems; i++) free(items[i].key);
    free(items); free(ext); free(fst); free(h3); free(gdec); free(ghdr); free(genc);
    if (r) unlink(out_path);
    return r;
}
