/* Porpoise Retro Rewind - safe ZIP extraction for official update archives.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RR_ZIP_H
#define RR_ZIP_H
#include "rr_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if `name` is an acceptable archive member path (relative, no "..", no backslash, no control bytes, starts with "RetroRewind6/" or is exactly "RetroRewind6/"). */
int rr_zip_name_ok(const char *name);

/* Extracts every member into stage_root/<member path> (so "RetroRewind6/x" lands in stage_root/RetroRewind6/x,
 * never nested twice). Rejects encrypted members, symlinks, duplicate names, unsafe paths, files that
 * disagree with their CRC-32, and entries outside RetroRewind6/. On failure nothing is cleaned up here
 * (the caller removes stage_root).  max_total bounds the uncompressed size. */
/* Same, but appends "F <name>" / "D <name>" lines for everything extracted to list_path, so callers can
 * work from the archive's own file list instead of having to list the folder. */
int rr_zip_extract_ex(rr_ctx *c, const char *zip_path, const char *stage_root, uint64_t max_total, uint64_t *total_out, size_t *files_out, const char *list_path);
int rr_zip_extract(rr_ctx *c, const char *zip_path, const char *stage_root, uint64_t max_total, uint64_t *total_out, size_t *files_out);

#ifdef __cplusplus
}
#endif
#endif
