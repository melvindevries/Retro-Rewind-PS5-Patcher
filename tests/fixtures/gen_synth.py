#!/usr/bin/env python3
"""Generate a synthetic (non-Nintendo) Wii game directory in WIT's "extract" layout.

The content is deterministic pseudo-random data. It is NOT Mario Kart Wii; it only
has the structure a Wii data partition needs (boot.bin, bi2.bin, apploader, DOL, files),
so the patch engine can be tested end to end without any copyrighted material.

usage: gen_synth.py OUTDIR REGION(E|P|J) [total_mb]
"""
import os, random, struct, sys

out, region = sys.argv[1], sys.argv[2]
total_mb = int(sys.argv[3]) if len(sys.argv) > 3 else 24
rng = random.Random(0x5EED + ord(region))

def blob(n):
    return bytes(rng.getrandbits(8) for _ in range(min(n, 4096))) * (n // 4096 + 1)

def put(path, data):
    p = os.path.join(out, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data[:len(data)] if isinstance(data, bytes) else data)

disc_id = ("RMC%s01" % region).encode()
boot = bytearray(0x440)
boot[0:6] = disc_id
boot[0x18:0x1C] = struct.pack(">I", 0x5D1C9EA3)
name = b"SYNTHETIC KART TEST DISC"
boot[0x20:0x20 + len(name)] = name
put("sys/boot.bin", bytes(boot))
put("sys/bi2.bin", blob(0x2000)[:0x2000])

# apploader: 0x20 byte header (date, entry, size, trailer size) + body
body = blob(0x3000)[:0x3000]
hdr = b"2004/02/01".ljust(16, b"\0") + struct.pack(">III", 0x81200258, len(body), 0)
hdr = hdr.ljust(0x20, b"\0")
put("sys/apploader.img", hdr + body)

# DOL: one text section at 0x80004000 (0x6000 bytes) and one data at 0x80010000
text = blob(0x6000)[:0x6000]
data = blob(0x2000)[:0x2000]
dol = bytearray(0x100)
text_off, data_off = 0x100, 0x100 + len(text)
struct.pack_into(">I", dol, 0x00, text_off)           # text0 offset
struct.pack_into(">I", dol, 0x1C, data_off)           # data0 offset (slot 7)
struct.pack_into(">I", dol, 0x48, 0x80004000)         # text0 addr
struct.pack_into(">I", dol, 0x64, 0x80010000)         # data0 addr (slot 7)
struct.pack_into(">I", dol, 0x90, len(text))
struct.pack_into(">I", dol, 0xAC, len(data))
struct.pack_into(">I", dol, 0xE0, 0x80004000)
put("sys/main.dol", bytes(dol) + text + data)

# game files; tree mimics a few paths the Retro Rewind builder overlays
files = {
    "Race/Course/old_track.szs": 3 << 20,
    "Race/Common.szs": 1 << 19,
    "Scene/UI/Title_E.szs": 300_000,
    "Scene/UI/Race_E.szs": 200_001,      # non multiple of 4 / sector sizes on purpose
    "sound/strm/music.brstm": 5 << 20,
    "thp/button/single_top.thp": 700_003,
    "Boot/Strap/us/English.szs": 90_000,
    "Binaries/placeholder.bin": 1,
    "Race/Kart/placeholder.szs": 20_000,
    "Scene/Model/Kart/placeholder.szs": 20_001,
    "Scene/Model/Driver/placeholder.szs": 20_002,
    "Race/Map/placeholder.szs": 20_003,
    "sound/placeholder.brsar": 20_004,
    "Boot/Strap/jp/jp.szs": 90_001,
    "Boot/Strap/eu/English.szs": 90_002,
    "Boot/Strap/us/French.szs": 90_003,
    "Boot/savebanner.tpl": 29_312,
}
for d in ("course", "battle", "title", "ending", "button"):
    files["thp/%s/placeholder.thp" % d] = 50_000 + len(d)
used = sum(files.values())
filler = max(0, (total_mb << 20) - used - (1 << 20))
if filler:
    files["data/filler.bin"] = filler
for rel, size in sorted(files.items()):
    put("files/" + rel, blob(size)[:size])
print("synthetic dir:", out, disc_id.decode())
