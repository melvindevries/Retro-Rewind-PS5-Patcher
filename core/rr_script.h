/* Porpoise Retro Rewind - interpreter for the supported subset of the builder's copy-files.sh.
 * Only mkdir -p, cp -f/-r/-fr, variable assignment, for-loops and `wit dolpatch` are understood;
 * anything else aborts the run (no shell is ever started).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RR_SCRIPT_H
#define RR_SCRIPT_H
#include "wii_build.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *builder_dir; /* holds copy-files.sh, extra/, Patches/ */
    const char *pack_dir;    /* the directory that contains RetroRewind6/ */
} rr_script_roots;

/* vars: NULL-terminated array of "NAME=VALUE" strings (SRC_ID, SRC_TYPE, ...). */
int rr_script_run(rr_ctx *c, rr_vtree *t, const char *script, const rr_script_roots *roots, const char *const *vars);

#ifdef __cplusplus
}
#endif
#endif
