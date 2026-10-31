#!/usr/bin/env bash
# Porpoise Retro Rewind - stages the sources and runs the boilerplate build (compile, link, sign, assemble).
# Output: dist/PPSA97531/ (+ dist/PPSA97531.zip). SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
stage="$root/build/appsrc"
rm -rf "$stage"
mkdir -p "$stage"
cp "$root"/app/src/*.c "$root"/app/src/*.cpp "$root"/app/src/*.h "$stage/"
cp "$root"/core/*.c "$root"/core/*.h "$stage/"
cp "$root"/third_party/miniz/miniz.c "$root"/third_party/miniz/miniz.h "$stage/"
mkdir -p "$root/build/appmeta"
rm -rf "$root/build/appmeta/sce_sys"
cp -r "$root/app/sce_sys" "$root/build/appmeta/sce_sys"
export APP_SOURCE_DIR=build/appsrc APP_PARAM=build/appmeta/sce_sys/param.json APP_SCE_SYS=build/appmeta/sce_sys
export APP_ASSETS="" USE_CCACHE=0
export APP_INCLUDE_PATHS="build/appsrc"
export APP_DEFINITIONS="MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES NDEBUG RR_USE_GETDENTS"
exec bash "$root/tools/build.sh" Folder
