#!/usr/bin/env python3
"""Builds a mock update server directory: RetroRewindVersion.txt + zips (good and evil)."""
import os, sys, zipfile, stat
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
def zf(name, members, symlink=None):
    with zipfile.ZipFile(os.path.join(out, name), "w", zipfile.ZIP_DEFLATED) as z:
        for n, data in members.items():
            if n.endswith("/"):
                z.writestr(zipfile.ZipInfo(n), b"")
            else:
                z.writestr(n, data)
        if symlink:
            zi = zipfile.ZipInfo(symlink[0]); zi.create_system = 3; zi.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(zi, symlink[1])
big = os.urandom(300000)
zf("6.13.1.zip", {"RetroRewind6/": b"", "RetroRewind6/Binaries/Code.pul": b"CODE-6.13.1" * 100, "RetroRewind6/Tracks/new_track.szs": big,
                  "RetroRewind6/version.txt": b"6.13.1\n"})
zf("6.13.2a.zip", {"RetroRewind6/Assets/CommonAssets.szs": b"ASSETS-6.13.2" * 50})
zf("6.13.2b.zip", {"RetroRewind6/strm/song.brstm": b"SONG" * 1000, "RetroRewind6/Binaries/Code.pul": b"CODE-6.13.2" * 100})
zf("6.13.3.zip", {"RetroRewind6/Binaries/Loader.pul": b"LOADER-6.13.3", "RetroRewind6/version.txt": b"9.9.9\n"})  # wrong embedded version -> manifest wins
# evil archives
zf("evil_trav.zip", {"RetroRewind6/ok.txt": b"x", "RetroRewind6/../../escape.txt": b"pwn"})
zf("evil_abs.zip", {"/etc/escape.txt": b"pwn"})
zf("evil_other.zip", {"RetroRewind6/ok.txt": b"x", "Other/file.txt": b"y"})
zf("evil_back.zip", {"RetroRewind6\\..\\x.txt": b"x"})
zf("evil_link.zip", {"RetroRewind6/ok.txt": b"x"}, symlink=("RetroRewind6/link", "/etc/passwd"))
zf("evil_nested.zip", {"RetroRewind6/RetroRewind6/Binaries/Code.pul": b"NESTED"})  # legal path, must stay literal (no flattening)
zf("conflict.zip", {"RetroRewind6/Binaries": b"file where a dir lives"})
open(os.path.join(out, "garbage.zip"), "wb").write(b"this is not a zip file at all, just text" * 10)
body = {"Binaries/Code.pul": b"FULLCODE" * 50, "Binaries/Loader.pul": b"FULLLOADER", "Tracks/t1.szs": b"T1" * 5000, "strm/s.brstm": b"S" * 3000, "Assets/CommonAssets.szs": b"CA" * 100}
zf("full_rooted.zip", {"RetroRewind6/": b"", **{"RetroRewind6/" + k: v for k, v in body.items()}, "RetroRewind6/version.txt": b"6.13.3\n"})
zf("full_wrapped.zip", {**{"RetroRewind/RetroRewind6/" + k: v for k, v in body.items()}, "RetroRewind/RetroRewind6/version.txt": b"6.13.3\n"})
zf("full_bare.zip", {**body, "version.txt": b"6.13.3\n"})
zf("full_noversion.zip", {"RetroRewind6/" + k: v for k, v in body.items()})
zf("full_extra.zip", {**{"RetroRewind6/" + k: v for k, v in body.items()}, "RetroRewind6/version.txt": b"6.13.3\n", "riivolution/RetroRewind6.xml": b"<xml/>", "__MACOSX/x": b"junk"})
zf("full_evil.zip", {"RetroRewind6/ok.txt": b"x", "RetroRewind6/../../escape.txt": b"pwn"})
bsc = {"copy-files.sh": b"#!/bin/sh\n", "create-rr-usa.sh": b"u", "create-rr-pal.sh": b"p", "create-rr-jpn.sh": b"j",
       "extra/Boot/Loaders/EUR/main.dol": b"E" * 3000, "extra/Boot/Loaders/USA/main.dol": b"U" * 3000, "extra/Boot/Loaders/JPN/main.dol": b"J" * 3000,
       "extra/Boot/Loaders/KOR/": b"", "extra/Boot/Strap/Strap.szs": b"S" * 500, "extra/wiivcpatch.xml": b"<x/>", "extra/thp/banana.thp": b"T" * 800,
       "Patches/font.Font": b"F" * 300}
junk = {"copy-files.bat": b"@echo", "ddel.exe": b"MZ" * 100, "check_update.ps1": b"x", "README.txt": b"r", "create-all.sh": b"a", "set-image-type-WBFS.sh": b"w", "create-rr-pal.bat": b"b"}
zf("updatedbuilder.zip", {**bsc, **junk})
zf("builder_incomplete.zip", {"copy-files.sh": b"x", "extra/a": b"y", **junk})
zf("builder_evil.zip", {**bsc, "extra/../../escape.sh": b"pwn"})
H = "https://cdn.update.rwfc.net/RetroRewind/zip/"
def manifest(name, lines):
    open(os.path.join(out, name), "w").write("\n".join(lines) + "\n")
manifest("RetroRewindVersion.txt", [
  f"6.13.0 {H}6.13.0.zip /6.13.0.zip Assets",
  f"6.13.1 {H}6.13.1.zip /6.13.1.zip Assets",
  f"6.13.2 {H}6.13.2a.zip /6.13.2a.zip Assets",
  f"6.13.2 {H}6.13.2b.zip /6.13.2b.zip Assets",
  f"6.13.3 {H}6.13.3.zip /6.13.3.zip Assets"])
