/* Host-side command line front end to the engine (used by the test suite). SPDX-License-Identifier: GPL-3.0-or-later */
#include "../core/rr_patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void logcb(void *u, int l, const char *m) { (void)u; if (l >= 1) fprintf(stderr, "[%s] %s\n", l == 1 ? "info" : l == 2 ? "warn" : "ERROR", m); }
int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: rrcli status|patch|force|verify BASE PACK BUILDER OUT REGION\n"); return 2; }
    rr_ctx c; rr_ctx_init(&c); c.log = logcb;
    rr_patch_cfg cfg = {argv[2], argv[3], argv[4], argv[5], argv[6][0]};
    if (!strcmp(argv[1], "status")) {
        rr_patch_status st; rr_patch_status_get(&c, &cfg, &st);
        printf("base_found=%d region=%c id=%s out_exists=%d out_current=%d err=%s\n", st.base_found, st.base_region ? st.base_region : '-', st.base_id, st.out_exists, st.out_current, st.base_error);
        return 0;
    }
    if (!strcmp(argv[1], "verify")) { int r = rr_patch_verify_output(&c, &cfg); printf(r ? "VERIFY FAILED: %s\n" : "VERIFY OK\n", c.err); return r ? 1 : 0; }
    rr_patch_result res;
    int r = rr_patch_run(&c, &cfg, !strcmp(argv[1], "force"), &res);
    if (r) { printf("FAILED(%d): %s\n", r, c.err); return 1; }
    printf(res.skipped_up_to_date ? "UP-TO-DATE %s\n" : "PATCHED %s\n", res.out_path);
    return 0;
}
