#!/usr/bin/env bash
# End-to-end host test of the patch engine against real WIT output.
# usage: e2e_patch.sh ORACLE_DIR(from make_oracle.sh)  REGION  RRCLI
set -u
O=$1 R=$2 CLI=$3
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p $T/base $T/pack $T/builder $T/out
cp $O/base.wbfs $T/base/mkw.wbfs
cp -r $O/RetroRewind6 $T/pack/
for f in copy-files.sh create-rr-usa.sh create-rr-pal.sh create-rr-jpn.sh; do cp $O/$f $T/builder/; done
cp -r $O/extra $O/Patches $T/builder/
DEST=$(case $R in E) echo RMCETO;; P) echo RMCPTO;; J) echo RMCJTO;; esac)
OUT=$T/out/$DEST.wbfs
run() { "$CLI" "$1" $T/base $T/pack $T/builder $T/out ${2:-$R}; }
pass=0; fail=0
ok() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "PASS: $2"; else fail=$((fail+1)); echo "FAIL: $2"; fi; }
sum() { sha256sum "$1" | cut -d' ' -f1; }
BASE_SUM=$(sum $T/base/mkw.wbfs)

echo "--- 1. patch from scratch"
run patch | tee $T/o1; grep -q '^PATCHED' $T/o1; ok $? "first patch succeeds"
cmp -s $OUT $O/new-image/*.wbfs; ok $? "output is byte-identical to real WIT output"
[ "$(sum $T/base/mkw.wbfs)" = "$BASE_SUM" ]; ok $? "base image untouched"
[ ! -e $OUT.part ]; ok $? "no temp file left"
run verify | grep -q 'VERIFY OK'; ok $? "verify action accepts output"

echo "--- 2. nothing changed -> skip"
run patch | grep -q '^UP-TO-DATE'; ok $? "second run skips unnecessary patching"
run status | grep -q 'out_current=1'; ok $? "status reports current"

echo "--- 3. pack changed -> rebuild"
sleep 1; echo "x" >> $T/pack/RetroRewind6/Binaries/Loader.pul
GOOD=$(sum $OUT)
run status | grep -q 'out_current=1'; ok $? "status still current before rerun (cheap check)"
run patch | grep -q '^PATCHED'; ok $? "changed pack input triggers rebuild"
[ "$(sum $OUT)" != "$GOOD" ]; ok $? "rebuilt output differs"
run verify | grep -q 'VERIFY OK'; ok $? "rebuilt output verifies"

echo "--- 4. failed patch keeps previous valid output"
KEEP=$(sum $OUT)
mv $T/pack/RetroRewind6/Binaries/Code.pul $T/code.bak
run patch | grep -q '^FAILED'; ok $? "missing required pack file fails"
[ "$(sum $OUT)" = "$KEEP" ]; ok $? "previous valid output preserved"
[ ! -e $OUT.part ]; ok $? "no partial output left"
mv $T/code.bak $T/pack/RetroRewind6/Binaries/Code.pul

echo "--- 5. region mismatch"
W=$(case $R in E) echo P;; *) echo E;; esac)
run patch $W | grep -q '^FAILED'; ok $? "wrong region refused"
[ "$(sum $OUT)" = "$KEEP" ]; ok $? "output untouched after refusal"

echo "--- 6. two game images"
cp $T/base/mkw.wbfs $T/base/second.wbfs
run patch | grep -q 'FAILED.*exactly one'; ok $? "ambiguous base refused"
rm $T/base/second.wbfs
touch $T/base/game.iso; mv $T/base/mkw.wbfs $T/base/hold.bak
run patch | grep -q 'FAILED.*not a supported format'; ok $? "unsupported format named"
mv $T/base/hold.bak $T/base/mkw.wbfs; rm $T/base/game.iso

echo "--- 7. corrupted base"
python3 -I - "$T/base/mkw.wbfs" <<'PY'
import sys
p=sys.argv[1]; d=bytearray(open(p,'rb').read())
# flip a byte deep inside file data (past the partition headers)
i=0x200000*8+0x12345; d[i]^=0xFF; open(p,'wb').write(d)
PY
run force | grep -q '^FAILED'; ok $? "corrupt base detected, patch refused"
[ "$(sum $OUT)" = "$KEEP" ]; ok $? "valid output still preserved"

echo "--- 8. corrupted output is caught by verify"
python3 -I - "$OUT" <<'PY'
import sys
p=sys.argv[1]; d=bytearray(open(p,'rb').read()); d[0x400000+0x20000+0x40000+777]^=1; open(p,'wb').write(d)
PY
run verify | grep -q 'VERIFY FAILED'; ok $? "verify detects a flipped bit"

echo "--- 9. builder files inside a sub folder / missing builder"
cp $O/base.wbfs $T/base/mkw.wbfs; rm -f $OUT $OUT.meta; mkdir -p $T/builder/Unpacked; for f in $(ls $T/builder | grep -v '^Unpacked$'); do mv $T/builder/$f $T/builder/Unpacked/; done
run patch > $T/o9 2>&1; grep -q "^PATCHED" $T/o9; ok $? "builder found one level down"; grep -v "^\[info\]" $T/o9 | head -3
rm -rf $T/builder; mkdir $T/builder
run patch | grep -q 'builder files not found'; ok $? "missing builder gives a clear message"

echo "--- 10. pack file names that differ only in letter case (Windows scripts, case-sensitive console)"
rm -rf $T/builder; mkdir -p $T/builder; for f in copy-files.sh create-rr-usa.sh create-rr-pal.sh create-rr-jpn.sh; do cp $O/$f $T/builder/; done; cp -r $O/extra $O/Patches $T/builder/
cp $O/base.wbfs $T/base/mkw.wbfs; rm -f $OUT $OUT.meta
run patch > /dev/null 2>&1; REF=$(sum $OUT); rm -f $OUT $OUT.meta
mv $T/pack/RetroRewind6/Binaries/Code.pul $T/pack/RetroRewind6/Binaries/code.pul 2>/dev/null || mv $T/pack/RetroRewind6/binaries/Code.pul $T/pack/RetroRewind6/binaries/code.pul
run patch > $T/o10 2>&1; grep -q "^PATCHED" $T/o10; ok $? "patch works with a lower-case file name in the pack"
[ "$(sum $OUT)" = "$REF" ]; ok $? "output identical to the exact-case build"

echo "--- 11. a symbolic link in the pack is refused"
rm -f $OUT $OUT.meta; echo secret > $T/outside.txt; rm -f $T/pack/RetroRewind6/Binaries/code.pul $T/pack/RetroRewind6/Binaries/Code.pul; ln -s $T/outside.txt $T/pack/RetroRewind6/Binaries/Code.pul
run patch 2>&1 | grep -q "symbolic link"; ok $? "symlinked pack file refused"
[ ! -e $OUT ]; ok $? "no output created"

echo "--- 12. optional language files missing (a pack without Language/*/Font), like the official script"
rm -f $T/pack/RetroRewind6/Binaries/Code.pul $T/pack/RetroRewind6/Binaries/code.pul
cp $O/RetroRewind6/Binaries/* $T/pack/RetroRewind6/Binaries/ 2>/dev/null || true
rm -rf $T/pack/RetroRewind6/Language/*/Font; rm -f $OUT $OUT.meta
run patch > $T/o12 2>&1; grep -q '^PATCHED' $T/o12; ok $? "patch succeeds without the Font folders"
grep -q "optional language files" $T/o12; ok $? "the skipped files are reported"
run verify | grep -q 'VERIFY OK'; ok $? "output verifies"
rm -f $T/pack/RetroRewind6/Binaries/Code.pul $T/pack/RetroRewind6/Binaries/code.pul; rm -f $OUT $OUT.meta
run patch 2>&1 | grep -q "does not exist"; ok $? "a missing core file (Binaries/Code.pul) still stops the patch"
[ ! -e $OUT ]; ok $? "no output created"

echo; echo "passed=$pass failed=$fail"; [ $fail = 0 ]
