#!/usr/bin/env bash
# Host test suite. usage: tests/run_all.sh [ORACLE_ROOT]   (ORACLE_ROOT holds oracle-E/-P/-J from tests/make_oracle.sh;
# without it only the update tests run). SPDX-License-Identifier: GPL-3.0-or-later
set -u
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"; mkdir -p build
MZ="-DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -Ithird_party/miniz"
F="-std=gnu11 -O2 -Wall -Wextra $MZ"
cc $F tools/rrcli.c core/*.c third_party/miniz/miniz.c -o build/rrcli 2>&1 | grep -E "error" ; 
cc $F tools/updcli.c core/rr_ctx.c core/rr_update.c core/rr_zip.c third_party/miniz/miniz.c -o build/updcli 2>&1 | grep -E "error"
cc -std=gnu11 -DRR_USE_GETDENTS -DRR_GDE=rr_host_gde -DRR_TEST_NO_LIBC_FIRST tests/test_getdents.c core/rr_ctx.c -o build/test_getdents && build/test_getdents || echo "FAIL: getdents parser"
cc $F -DRR_TEST_NO_LISTING tools/updcli.c core/rr_ctx.c core/rr_update.c core/rr_zip.c third_party/miniz/miniz.c -o build/updcli_nolist 2>&1 | grep -E "error"
cc $F tools/updcli.c tests/stat_fault.c core/rr_ctx.c core/rr_update.c core/rr_zip.c third_party/miniz/miniz.c -ldl -o build/updcli_statfault 2>&1 | grep -E "error"
rc=0
echo "== update tests with stat() failing on every directory"
bash tests/e2e_update.sh "$root/build/updcli_statfault" | grep -E "FAIL:|passed" || rc=1
echo "== update tests with folder listing disabled"
bash tests/e2e_update.sh "$root/build/updcli_nolist" | grep -E "FAIL|passed" || rc=1
bash tests/e2e_update.sh "$root/build/updcli" | tail -3 || rc=1
if [ -n "${1:-}" ]; then for r in E P J; do echo "== patch region $r"; bash tests/e2e_patch.sh "$1/oracle-$r" $r "$root/build/rrcli" | tail -4 || rc=1; done; fi
exit $rc
