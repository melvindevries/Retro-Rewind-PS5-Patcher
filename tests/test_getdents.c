/* Feeds hand-made kernel directory buffers (both known dirent generations) to the PS5 getdents parser. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../core/rr_ctx.h"
static unsigned char g_buf[4096]; static int g_len, g_served;
int getdents(int fd, char *buf, int n) { (void)fd; if (g_served) return 0; g_served = 1; if (g_len > n) return -1; memcpy(buf, g_buf, (size_t)g_len); return g_len; }
static void put16(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = v >> 8; }
static void add_a(const char *nm, unsigned fileno) { size_t l = strlen(nm); unsigned rec = (unsigned)((8 + l + 1 + 3) & ~3u); unsigned char *e = g_buf + g_len; memset(e, 0, rec); e[0] = fileno; put16(e + 4, rec); e[6] = 8; e[7] = (unsigned char)l; memcpy(e + 8, nm, l); g_len += (int)rec; }
static void add_b(const char *nm, unsigned fileno) { size_t l = strlen(nm); unsigned rec = (unsigned)((24 + l + 1 + 7) & ~7u); unsigned char *e = g_buf + g_len; memset(e, 0, rec); e[0] = fileno; put16(e + 16, rec); e[18] = 8; put16(e + 20, (unsigned)l); memcpy(e + 24, nm, l); g_len += (int)rec; }
static int check(int layout_b)
{
    g_len = 0; g_served = 0;
    void (*add)(const char *, unsigned) = layout_b ? add_b : add_a;
    add(".", 1); add("..", 2); add("Mario Kart Wii (USA).wbfs", 7); add("gone.wbfs", 0); add("x", 9);
    /* layout A marks deleted slots with fileno 0; layout B in this test keeps them (no such convention) */
    const char *want_a[] = {".", "..", "Mario Kart Wii (USA).wbfs", "x"};
    const char *want_b[] = {".", "..", "Mario Kart Wii (USA).wbfs", "gone.wbfs", "x"};
    const char **want = layout_b ? want_b : want_a; int total = layout_b ? 5 : 4, n = 0, bad = 0;
    rr_dir *d = rr_opendir("/"); const char *nm;
    while ((nm = rr_readdir(d))) { if (n >= total || strcmp(nm, want[n])) { fprintf(stderr, "layout %c entry %d: got '%s'\n", layout_b ? 'B' : 'A', n, nm); bad = 1; } n++; }
    rr_closedir(d);
    if (n != total) bad = 1;
    printf("layout %c: %d entries %s\n", layout_b ? 'B' : 'A', n, bad ? "WRONG" : "ok");
    return bad;
}
int main(void) { return check(0) | check(1); }
int rr_host_gde(int fd, char *buf, int n, long *b) { (void)b; return getdents(fd, buf, n); } /* built with -Dgetdirentries=rr_host_gde */
