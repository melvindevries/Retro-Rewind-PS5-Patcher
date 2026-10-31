#!/usr/bin/env bash
# Build the reference output with the REAL supplied builder scripts + real WIT.
# usage: make_oracle.sh WORKROOT REGION(E|P|J) PACK_VERSION BUILDER_DIR WIT_BIN_DIR [total_mb]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$1 region=$2 version=$3 builder=$4 witbin=$5 mb=${6:-24}
export PATH="$witbin:$PATH"
case $region in E) script=create-rr-usa.sh;; P) script=create-rr-pal.sh;; J) script=create-rr-jpn.sh;; esac
mkdir -p "$work"
rm -rf "$work/synth-$region" "$work/oracle-$region"
python3 "$here/fixtures/gen_synth.py" "$work/synth-$region" "$region" "$mb" >/dev/null
mkdir -p "$work/oracle-$region"
( cd "$work" && wit copy "synth-$region" "oracle-$region/base.wbfs" --wbfs -o >/dev/null 2>&1 )
cp -r "$builder"/. "$work/oracle-$region/"
rm -f "$work"/oracle-$region/*.bat "$work/oracle-$region/ddel.exe"
python3 "$here/fixtures/gen_pack.py" "$work/oracle-$region/copy-files.sh" "$work/oracle-$region" "$version" >/dev/null
( cd "$work/oracle-$region" && bash "$script" > run.log 2>&1 )
if grep -q '^cp:' "$work/oracle-$region/run.log"; then echo "oracle cp errors:"; grep '^cp:' "$work/oracle-$region/run.log" | head; exit 1; fi
ls "$work/oracle-$region/new-image/"
