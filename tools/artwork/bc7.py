# Encodes a 3840x2160 image as BC7 (mode 6) DX10 DDS for sce_sys/pic0.dds and pic1.dds. usage: bc7.py IN.png OUT.dds. SPDX-License-Identifier: GPL-3.0-or-later
import sys, struct
import numpy as np
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
im = np.asarray(Image.open(src).convert("RGB"), dtype=np.float32)
H, W, _ = im.shape
assert (W, H) == (3840, 2160)
# blocks: (nb, 16, 3)
b = im.reshape(H//4, 4, W//4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
nb = b.shape[0]
lo = np.zeros(nb, np.uint64); hi = np.zeros(nb, np.uint64)
weights = np.array([0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64], np.float32)/64.0
CH = 65536
def put(arr_lo, arr_hi, val, off, n):
    val = val.astype(np.uint64)
    if off + n <= 64:
        arr_lo |= val << np.uint64(off)
    elif off >= 64:
        arr_hi |= val << np.uint64(off - 64)
    else:
        k = 64 - off
        arr_lo |= (val & np.uint64((1 << k) - 1)) << np.uint64(off)
        arr_hi |= val >> np.uint64(k)
for s in range(0, nb, CH):
    blk = b[s:s+CH]; n = blk.shape[0]
    mean = blk.mean(1, keepdims=True)
    d = blk - mean
    cov = np.einsum('npi,npj->nij', d, d)
    v = np.ones((n, 3), np.float32)
    for _ in range(6):
        v = np.einsum('nij,nj->ni', cov, v)
        v /= (np.linalg.norm(v, axis=1, keepdims=True) + 1e-9)
    t = np.einsum('npi,ni->np', d, v)
    e0 = mean[:, 0, :] + v * t.min(1, keepdims=True)
    e1 = mean[:, 0, :] + v * t.max(1, keepdims=True)
    # quantize: 8-bit value = 2*v7 + 1 (both p-bits = 1, so alpha 255 is exact)
    def q(e):
        v7 = np.clip(np.rint((e - 1.0) / 2.0), 0, 127)
        return v7.astype(np.int32), (v7 * 2 + 1)
    q0, d0 = q(e0); q1, d1 = q(e1)
    # indices from projection onto the quantized endpoint line
    ax = d1 - d0
    den = (ax * ax).sum(1, keepdims=True) + 1e-9
    tt = np.clip(np.einsum('npi,ni->np', blk - d0[:, None, :], ax) / den, 0, 1)
    idx = np.abs(tt[..., None] - weights[None, None, :]).argmin(-1).astype(np.int32)
    swap = idx[:, 0] >= 8
    idx = np.where(swap[:, None], 15 - idx, idx)
    q0s = np.where(swap[:, None], q1, q0); q1s = np.where(swap[:, None], q0, q1)
    L = np.zeros(n, np.uint64); Hh = np.zeros(n, np.uint64)
    put(L, Hh, np.full(n, 64), 0, 7)
    off = 7
    for c in range(3):
        put(L, Hh, q0s[:, c], off, 7); off += 7
        put(L, Hh, q1s[:, c], off, 7); off += 7
    # NOTE channel order in the block is R0 R1 G0 G1 B0 B1 A0 A1
    put(L, Hh, np.full(n, 127), off, 7); off += 7   # A0
    put(L, Hh, np.full(n, 127), off, 7); off += 7   # A1
    put(L, Hh, np.ones(n), off, 1); off += 1         # P0
    put(L, Hh, np.ones(n), off, 1); off += 1         # P1
    put(L, Hh, idx[:, 0], off, 3); off += 3
    for p in range(1, 16):
        put(L, Hh, idx[:, p], off, 4); off += 4
    assert off == 128, off
    lo[s:s+n] = L; hi[s:s+n] = Hh
data = np.stack([lo, hi], 1).astype('<u8').tobytes()
hdr = b"DDS " + struct.pack("<7I", 124, 0x81007, H, W, (W//4)*(H//4)*16, 0, 1) + b"\0"*44
hdr += struct.pack("<II4s5I", 32, 4, b"DX10", 0, 0, 0, 0, 0) + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
hdr += struct.pack("<5I", 98, 3, 0, 1, 0)
assert len(hdr) == 148, len(hdr)
open(dst, "wb").write(hdr + data)
print("written", dst, len(hdr + data))
