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
H = "https://cdn.update.rwfc.net/RetroRewind/zip/"
def manifest(name, lines):
    open(os.path.join(out, name), "w").write("\n".join(lines) + "\n")
manifest("RetroRewindVersion.txt", [
  f"6.13.0 {H}6.13.0.zip /6.13.0.zip Assets",
  f"6.13.1 {H}6.13.1.zip /6.13.1.zip Assets",
  f"6.13.2 {H}6.13.2a.zip /6.13.2a.zip Assets",
  f"6.13.2 {H}6.13.2b.zip /6.13.2b.zip Assets",
  f"6.13.3 {H}6.13.3.zip /6.13.3.zip Assets"])
