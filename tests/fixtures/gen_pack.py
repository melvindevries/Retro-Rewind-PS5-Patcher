#!/usr/bin/env python3
"""Create a fake Retro Rewind pack tree that satisfies every source path that the
supplied builder's copy-files.sh reads (so the patch pipeline can run without the real pack).

usage: gen_pack.py COPY_FILES_SH OUTDIR VERSION
OUTDIR gets RetroRewind6/... and version.txt content VERSION in RetroRewind6/version.txt
"""
import os, re, shlex, sys, hashlib

script, out, version = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(script, encoding="utf-8").read().replace("\\\n", " ")
srcs = set()
for line in text.splitlines():
    line = line.strip()
    if not re.match(r"cp\s", line):
        continue
    parts = shlex.split(line)
    args = [p for p in parts[1:] if not p.startswith("-")]
    for a in args[:-1]:
        srcs.add(a)

def data(path, n=777):
    seed = hashlib.sha256(path.encode()).digest()
    return (seed * (n // 32 + 1))[:n]

def write(rel, n=777):
    p = os.path.join(out, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data(rel, n))

for s in sorted(srcs):
    rel = s[2:] if s.startswith("./") else s
    if rel.startswith("RetroRewind6/"):
        if rel.endswith("/*"):
            base = rel[:-2]
            write(base + "/a.bin", 1000)
            write(base + "/sub/b.bin", 33333)
        elif "." in os.path.basename(rel):
            write(rel, 4000)
        else:                       # a directory copied as a whole
            write(rel + "/inner.bin", 1234)
# files read through $RR_BINARIES / $RR_ASSETS variables
for f in ("Code", "ConfigBT", "ConfigCT", "ConfigRT", "Loader"):
    write("RetroRewind6/Binaries/%s.pul" % f, 5000)
for f in ("CommonAssets", "RaceAssets", "UIAssets", "ReplacedAssets"):
    write("RetroRewind6/Assets/%s.szs" % f, 6000)
os.makedirs(os.path.join(out, "RetroRewind6"), exist_ok=True)
with open(os.path.join(out, "RetroRewind6", "version.txt"), "w") as f:
    f.write(version + "\n")
print("pack written:", out, "(%d distinct source patterns)" % len(srcs))
