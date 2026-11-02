#!/usr/bin/env bash
# usage: e2e_update.sh UPDCLI
set -u
CLI=$1; here=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 -I "$here/gen_updates.py" $T/srv
pass=0; fail=0
ok() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "PASS: $2"; else fail=$((fail+1)); echo "FAIL: $2"; fi; }
mkpack() { rm -rf $T/pack; mkdir -p $T/pack/RetroRewind6/{Binaries,Tracks,Assets,strm}; echo 6.13.0 > $T/pack/RetroRewind6/version.txt
  echo OLD-CODE > $T/pack/RetroRewind6/Binaries/Code.pul; echo OLD-LOADER > $T/pack/RetroRewind6/Binaries/Loader.pul
  echo KEEP > $T/pack/RetroRewind6/Tracks/untouched.szs; echo OLDASSET > $T/pack/RetroRewind6/Assets/CommonAssets.szs; }
snap() { (cd $T/pack && find . -type f | sort | xargs sha256sum | sha256sum | cut -d' ' -f1); }
vers() { tr -d '\n ' < $T/pack/RetroRewind6/version.txt; }
leftovers() { ls -A $T/pack | grep -v '^RetroRewind6$' | tr '\n' ' '; }
with_srv() { cp $T/srv/RetroRewindVersion.txt $T/srv/RetroRewindVersion.txt.orig; }

echo "--- 1. sequential multi-version update (one release has two archives)"
mkpack; OUT=$($CLI apply $T/pack $T/srv 2>$T/err); echo "$OUT"
echo "$OUT" | grep -q "pending=4 versions=3"; ok $? "plan has 4 archives over 3 versions"
echo "$OUT" | grep -q "APPLIED now=6.13.3"; ok $? "applied up to latest"
[ "$(vers)" = 6.13.3 ]; ok $? "version.txt reads latest (manifest wins over archive's wrong embedded version)"
[ "$(cat $T/pack/RetroRewind6/Binaries/Code.pul | head -c 11)" = "CODE-6.13.2" ]; ok $? "later archive of 6.13.2 replaced file from 6.13.1"
[ "$(cat $T/pack/RetroRewind6/Binaries/Loader.pul)" = "LOADER-6.13.3" ]; ok $? "matching file replaced"
[ -f $T/pack/RetroRewind6/Tracks/new_track.szs ] && [ -f $T/pack/RetroRewind6/strm/song.brstm ]; ok $? "new files added"
[ "$(cat $T/pack/RetroRewind6/Tracks/untouched.szs)" = KEEP ]; ok $? "files absent from archives preserved"
[ ! -e $T/pack/RetroRewind6/RetroRewind6 ]; ok $? "no nested RetroRewind6/RetroRewind6"
[ -z "$(leftovers)" ]; ok $? "no staging/backup/journal leftovers ($(leftovers))"

echo "--- 2. already current -> no downloads"
OUT=$($CLI apply $T/pack $T/srv 2>/dev/null); echo "$OUT" | grep -q "pending=0"; ok $? "nothing pending"
echo "$OUT" | grep -q "gets=1"; ok $? "only the version list was fetched"

echo "--- 2b. latest-only mode downloads just the newest release"
mkpack; OUT=$($CLI applylatest $T/pack $T/srv 2>$T/err); echo "$OUT"
echo "$OUT" | grep -q "pending=1 versions=1"; ok $? "plan has exactly one archive"
echo "$OUT" | grep -q "gets=2"; ok $? "fetched the version list plus one archive only"
[ "$(vers)" = 6.13.3 ]; ok $? "version.txt reads latest"
[ -z "$(leftovers)" ]; ok $? "no leftovers after latest-only update"
[ "$(cat $T/pack/RetroRewind6/Tracks/untouched.szs)" = KEEP ]; ok $? "files absent from the archive preserved"
mkpack; echo 6.0.0 > $T/pack/RetroRewind6/version.txt
OUT=$($CLI applylatest $T/pack $T/srv 2>/dev/null); echo "$OUT" | grep -q "APPLIED now=6.13.3"; ok $? "very old install can jump straight to latest"
mkpack; echo 6.13.3 > $T/pack/RetroRewind6/version.txt
OUT=$($CLI applylatest $T/pack $T/srv 2>/dev/null); echo "$OUT" | grep -q "pending=0"; ok $? "already latest: nothing downloaded"

echo "--- 2c. leftover working folder from a failed run does not break or pollute the update"
mkpack; mkdir -p $T/pack/.rr-stage/RetroRewind6/Binaries $T/pack/.rr-dl; echo STALE > $T/pack/.rr-stage/RetroRewind6/Binaries/code.pul; echo STALE > $T/pack/.rr-stage/RetroRewind6/stale.bin
OUT=$($CLI applylatest $T/pack $T/srv 2>$T/err); echo "$OUT" | grep -q "APPLIED now=6.13.3"; ok $? "update succeeds over a leftover stage"
[ ! -e $T/pack/RetroRewind6/stale.bin ]; ok $? "stale leftover file was not merged"
grep -q "Merged" $T/err; ok $? "merge summary logged"

echo "--- 3. corrupted download"
mkpack; B=$(snap)
OUT=$($CLI apply $T/pack $T/srv flip:6.13.1.zip 2>$T/err); echo "$OUT" | tail -1
echo "$OUT" | grep -q FAILED; ok $? "bit-flipped archive rejected"
[ "$(snap)" = "$B" ] && [ "$(vers)" = 6.13.0 ]; ok $? "pack untouched"
[ -z "$(leftovers)" ]; ok $? "no leftovers"

echo "--- 4. truncated download"
OUT=$($CLI apply $T/pack $T/srv truncate:6.13.1.zip 2>$T/err); echo "$OUT" | tail -1
echo "$OUT" | grep -q FAILED; ok $? "truncated archive rejected"
[ "$(snap)" = "$B" ]; ok $? "pack untouched"

echo "--- 5. HTTP / network errors"
OUT=$($CLI apply $T/pack $T/srv status:404:6.13.1.zip 2>&1); echo "$OUT" | grep -q "FAILED.*HTTP 404"; ok $? "404 reported"
OUT=$($CLI apply $T/pack $T/srv neterr:6.13.1.zip 2>&1); echo "$OUT" | grep -q "FAILED.*network"; ok $? "network error reported"
OUT=$($CLI apply $T/pack $T/srv status:503:RetroRewindVersion.txt 2>&1); echo "$OUT" | grep -q "FAILED.*503"; ok $? "version list failure stops everything"
[ "$(snap)" = "$B" ]; ok $? "pack untouched after all of these"

echo "--- 6. failure on the second release keeps the first"
mkpack
OUT=$($CLI apply $T/pack $T/srv flip:6.13.2b.zip 2>&1); echo "$OUT" | tail -1
echo "$OUT" | grep -q "FAILED.*now at 6.13.1"; ok $? "stopped at last good version"
[ "$(vers)" = 6.13.1 ] && [ -f $T/pack/RetroRewind6/Tracks/new_track.szs ] && [ ! -e $T/pack/RetroRewind6/strm/song.brstm ] && [ "$(head -c 11 $T/pack/RetroRewind6/Binaries/Code.pul)" = "CODE-6.13.1" ]; ok $? "6.13.1 complete, 6.13.2 not applied at all (atomic per release)"
[ ! -e $T/pack/RetroRewind6/Assets/CommonAssets.szs.new ] && [ "$(head -c 7 $T/pack/RetroRewind6/Assets/CommonAssets.szs)" = OLDASSE ]; ok $? "first archive of failed release not half-applied"
OUT=$($CLI apply $T/pack $T/srv 2>&1); echo "$OUT" | grep -q "APPLIED now=6.13.3"; ok $? "retry resumes and completes"

echo "--- 7. unsafe archives"
for evil in evil_trav evil_abs evil_other evil_back evil_link garbage; do
  mkpack; B=$(snap)
  cp $T/srv/RetroRewindVersion.txt $T/srv/orig.txt
  sed -i "s#/6.13.1.zip#/$evil.zip#; s#zip/6.13.1.zip#zip/$evil.zip#" $T/srv/RetroRewindVersion.txt
  sed -i "/6.13.[23]/d" $T/srv/RetroRewindVersion.txt
  OUT=$($CLI apply $T/pack $T/srv 2>&1); echo "$OUT" | grep -q FAILED; ok $? "$evil rejected: $(echo "$OUT" | grep FAILED | head -1 | cut -c1-110)"
  [ "$(snap)" = "$B" ] && [ ! -e $T/escape.txt ] && [ ! -e /etc/escape.txt ] && [ ! -e $T/pack/escape.txt ]; ok $? "$evil: nothing written anywhere"
  cp $T/srv/orig.txt $T/srv/RetroRewindVersion.txt
done
echo "--- 8. literal nested folder name is not flattened, conflict is detected"
mkpack; sed -i "s#zip/6.13.1.zip#zip/evil_nested.zip#; /6.13.[23]/d" $T/srv/RetroRewindVersion.txt
$CLI apply $T/pack $T/srv >/dev/null 2>&1; [ "$(cat $T/pack/RetroRewind6/RetroRewind6/Binaries/Code.pul 2>/dev/null)" = NESTED ] && [ "$(head -c 8 $T/pack/RetroRewind6/Binaries/Code.pul)" = OLD-CODE ]; ok $? "archive path kept exactly (no silent restructuring)"
cp $T/srv/orig.txt $T/srv/RetroRewindVersion.txt
mkpack; B=$(snap); sed -i "s#zip/6.13.1.zip#zip/conflict.zip#; /6.13.[23]/d" $T/srv/RetroRewindVersion.txt
OUT=$($CLI apply $T/pack $T/srv 2>&1); echo "$OUT" | grep -q FAILED; ok $? "file-vs-folder conflict refused"
[ "$(snap)" = "$B" ]; ok $? "pack untouched after conflict"
cp $T/srv/orig.txt $T/srv/RetroRewindVersion.txt

echo "--- 9. bad version lists"
mkpack; B=$(snap)
for bad in "6.13.1 http://cdn.update.rwfc.net/a.zip /a.zip x" "6.13.1 https://evil.example.com/a.zip /a.zip x" "6.13.1 https://cdn.update.rwfc.net.evil.com/a.zip /a.zip x" "garbage"; do
  printf '6.13.0 https://cdn.update.rwfc.net/a/6.13.0.zip /6.13.0.zip x\n%s\n' "$bad" > $T/srv/RetroRewindVersion.txt
  OUT=$($CLI check $T/pack $T/srv 2>&1); echo "$OUT" | grep -q FAILED; ok $? "rejected: $bad"
done
printf '6.13.2 https://cdn.update.rwfc.net/a/x.zip /x.zip x\n6.13.1 https://cdn.update.rwfc.net/a/y.zip /y.zip x\n' > $T/srv/RetroRewindVersion.txt
$CLI check $T/pack $T/srv 2>&1 | grep -q "FAILED.*ascending"; ok $? "non-ascending list rejected"
printf '6.12.0 https://cdn.update.rwfc.net/a/x.zip /x.zip x\n' > $T/srv/RetroRewindVersion.txt
$CLI check $T/pack $T/srv 2>&1 | grep -q "pending=0"; ok $? "installed newer than server -> nothing to do"
printf '6.14.0 https://cdn.update.rwfc.net/a/x.zip /x.zip x\n' > $T/srv/RetroRewindVersion.txt
$CLI check $T/pack $T/srv 2>&1 | grep -q "FAILED.*older than the oldest"; ok $? "unverifiable chain refused"
cp $T/srv/orig.txt $T/srv/RetroRewindVersion.txt
echo "--- 10. no pack installed"
rm -rf $T/pack; mkdir $T/pack; $CLI check $T/pack $T/srv 2>&1 | grep -q "FAILED.*not installed"; ok $? "missing pack reported"

echo "--- 11. interrupted merge is rolled back on next start"
mkpack; B=$(snap)
mkdir -p $T/pack/.rr-backup/Binaries
mv $T/pack/RetroRewind6/Binaries/Code.pul $T/pack/.rr-backup/Binaries/Code.pul     # as if backed up ...
echo HALF-NEW > $T/pack/RetroRewind6/Binaries/Code.pul                                # ... and new file placed
echo JUNK > $T/pack/RetroRewind6/Tracks/added.szs
printf 'RRJ1\nM Binaries/Code.pul\nA Tracks/added.szs\nM Binaries/Loader.pul\n' > $T/pack/.rr-journal
$CLI recover $T/pack $T/srv | grep -q RECOVERED; ok $? "recover runs"
[ "$(snap)" = "$B" ] && [ -z "$(leftovers)" ]; ok $? "pack restored exactly, leftovers cleaned"

echo "--- 12. full install (no pack yet): folders are created, files placed, version written"
FULL() { rm -rf $T/pack; $CLI full $T/pack $T/srv "${2:-}" $1 2>$T/err; }
for Z in full_rooted full_wrapped full_bare full_extra; do
  rm -rf $T/pack; OUT=$($CLI full $T/pack $T/srv "" $Z.zip 2>$T/err); echo "$OUT"
  echo "$OUT" | grep -q "INSTALLED now=6.13.3"; ok $? "$Z: installed from nothing (pack folder did not exist)"
  [ "$(head -c 8 $T/pack/RetroRewind6/Binaries/Code.pul)" = FULLCODE ] && [ -f $T/pack/RetroRewind6/strm/s.brstm ] && [ -f $T/pack/RetroRewind6/Tracks/t1.szs ]; ok $? "$Z: files on the right place"
  [ ! -e $T/pack/RetroRewind6/RetroRewind6 ] && [ ! -e $T/pack/RetroRewind6/riivolution ] && [ ! -e $T/pack/RetroRewind6/__MACOSX ]; ok $? "$Z: no nesting, foreign folders not copied"
  [ -z "$(leftovers)" ]; ok $? "$Z: no leftovers ($(leftovers))"
done
rm -rf $T/pack; OUT=$($CLI full $T/pack $T/srv "" full_noversion.zip 2>$T/err); echo "$OUT" | grep -q "INSTALLED now=6.13.3"; ok $? "archive without version.txt: version taken from the server list"
rm -rf $T/pack; OUT=$($CLI full $T/pack $T/srv "" full_evil.zip 2>$T/err); echo "$OUT" | grep -q FAILED; ok $? "unsafe full archive refused"
[ ! -e $T/escape.txt ] && [ ! -e $T/pack/RetroRewind6 ]; ok $? "nothing written after refusal"
rm -rf $T/pack; OUT=$($CLI full $T/pack $T/srv "truncate:full_rooted.zip" full_rooted.zip 2>$T/err); echo "$OUT" | grep -q FAILED; ok $? "truncated download refused"
[ ! -e $T/pack/RetroRewind6 ] && [ -z "$(leftovers)" ]; ok $? "no half-installed pack after a failed download"
rm -rf $T/pack; OUT=$($CLI full $T/pack $T/srv "" missing.zip 2>$T/err); echo "$OUT" | grep -q FAILED; ok $? "HTTP 404 reported as failure"

echo "--- 13. builder install: folders created, only needed files kept, junk dropped"
leftb() { ls -A $T/bld 2>/dev/null | tr '\n' ' '; }
rm -rf $T/bld; OUT=$($CLI builder $T/bld $T/srv "" RetroRewindBuilder.zip 2>$T/err); echo "$OUT"
echo "$OUT" | grep -q "BUILDER-OK present=1 current=1"; ok $? "builder installed from nothing (builder folder did not exist) and marked current"
[ -f $T/bld/copy-files.sh ] && [ -f $T/bld/create-rr-pal.sh ] && [ -f $T/bld/extra/Boot/Loaders/EUR/main.dol ] && [ -f $T/bld/Patches/font.Font ] ; ok $? "scripts, extra/ and Patches/ in place"
[ ! -e $T/bld/copy-files.bat ] && [ ! -e $T/bld/ddel.exe ] && [ ! -e $T/bld/check_update.ps1 ] && [ ! -e $T/bld/README.txt ] && [ ! -e $T/bld/create-all.sh ] && [ ! -e $T/bld/create-rr-pal.bat ]; ok $? "unneeded files not written"
[ -z "$(leftb | tr -d ' ' | grep -v '^copy-files.sh')" ] || [ "$(leftb)" = ".rr-builder-rev Patches copy-files.sh create-rr-jpn.sh create-rr-pal.sh create-rr-usa.sh extra " ]; ok $? "no staging leftovers ($(leftb))"
rm -f $T/bld/.rr-builder-rev; OUT=$($CLI builder $T/bld $T/srv "" RetroRewindBuilder.zip 2>$T/err); echo "$OUT" | grep -q "current=1"; ok $? "an older builder without a revision marker is refreshed and marked current"
echo "--- 13b. builder refresh driven by REVISION in the repository"
rm -rf $T/bld; rm -f $T/srv/REVISION
OUT=$($CLI builderrefresh $T/bld $T/srv "" 2>$T/err); echo "$OUT" | grep -q "refreshed=1 current=1 rev=2026-10-29"; ok $? "missing builder installed; REVISION unreadable -> built-in revision used"
OUT=$($CLI builderrefresh $T/bld $T/srv "" 2>$T/err); echo "$OUT" | grep -q "refreshed=0"; ok $? "up to date: nothing downloaded"
echo 2026-10-29 > $T/srv/REVISION
OUT=$($CLI builderrefresh $T/bld $T/srv "" 2>$T/err); echo "$OUT" | grep -q "refreshed=0 current=1 rev=2026-10-29 gets=1"; ok $? "same revision published: only the small revision file is read"
echo 2027-01-05 > $T/srv/REVISION
OUT=$($CLI builderrefresh $T/bld $T/srv "" 2>$T/err); echo "$OUT" | grep -q "refreshed=1 current=1 rev=2027-01-05"; ok $? "newer revision published: builder downloaded again and marked with it"
echo 'garbage;rm -rf' > $T/srv/REVISION
OUT=$($CLI builderrefresh $T/bld $T/srv "" 2>$T/err); echo "$OUT" | grep -q "refreshed=0"; ok $? "invalid revision file ignored"
echo 2020-01-01 > $T/bld/.rr-builder-rev
OUT=$($CLI builderrefresh $T/bld $T/srv "" 2>$T/err); echo "$OUT" | grep -q "refreshed=1 current=1"; ok $? "builder older than the app expects is refreshed"
rm -f $T/srv/REVISION

rm -rf $T/bld; OUT=$($CLI builder $T/bld $T/srv "" builder_incomplete.zip 2>$T/err); echo "$OUT" | grep -q FAILED; ok $? "archive without the create scripts refused"
[ ! -e $T/bld/copy-files.sh ] && [ ! -e $T/bld/extra ]; ok $? "nothing left behind after refusal"
rm -rf $T/bld; OUT=$($CLI builder $T/bld $T/srv "" builder_evil.zip 2>$T/err); echo "$OUT" | grep -q FAILED; ok $? "unsafe builder archive refused"
[ ! -e $T/escape.sh ]; ok $? "nothing escaped the builder folder"
rm -rf $T/bld; OUT=$($CLI builder $T/bld $T/srv "truncate:RetroRewindBuilder.zip" RetroRewindBuilder.zip 2>$T/err); echo "$OUT" | grep -q FAILED; ok $? "truncated builder download refused"
rm -rf $T/bld; OUT=$($CLI builder $T/bld $T/srv "" missing.zip 2>$T/err); echo "$OUT" | grep -q FAILED; ok $? "HTTP 404 reported as failure"
mkdir -p $T/bld; echo MINE > $T/bld/notes.txt; $CLI builder $T/bld $T/srv "" RetroRewindBuilder.zip >/dev/null 2>&1; [ "$(cat $T/bld/notes.txt)" = MINE ]; ok $? "user's own files in the builder folder are kept"

echo; echo "passed=$pass failed=$fail"; [ $fail = 0 ]
