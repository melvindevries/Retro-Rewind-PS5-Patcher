/* Test shim: stat()/lstat() fail with EOVERFLOW for every directory, like a console whose stat rejects some entries. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
static int wrap(const char *name, const char *p, struct stat *st)
{
    int (*real)(const char *, struct stat *) = dlsym(RTLD_NEXT, name);
    int r = real(p, st);
    if (r == 0 && S_ISDIR(st->st_mode)) { errno = EOVERFLOW; return -1; }
    return r;
}
int stat(const char *p, struct stat *st) { return wrap("stat", p, st); }
int lstat(const char *p, struct stat *st) { return wrap("lstat", p, st); }
