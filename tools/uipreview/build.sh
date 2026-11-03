#!/usr/bin/env bash
# Renders the UI screens on a PC: tools/uipreview/build.sh OUTDIR  (writes main/busy/empty/log .ppm). SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); out=${1:?output dir}; mkdir -p "$out" "$root/build"
MZ="-DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -I$root/third_party/miniz"
obj="$root/build/uiobj"; mkdir -p "$obj"
for f in "$root"/core/*.c "$root"/third_party/miniz/miniz.c; do cc -std=gnu11 -O1 $MZ -I"$root/core" -c "$f" -o "$obj/$(basename "$f").o"; done
g++ -std=gnu++17 -O1 -D_Alignas=alignas -DGFX_HOST -DUI_PREVIEW -I"$root/core" -I"$root/app/src" "$root"/app/src/app.cpp "$root"/app/src/gfx.cpp "$root"/tools/uipreview/stubs.cpp "$obj"/*.o -lpthread -o "$root/build/uipreview"
"$root/build/uipreview" "$out"
