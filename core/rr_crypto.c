/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rr_crypto.h"
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#define RR_X86 1
#include <immintrin.h>
#endif

/* ------------------------------------------------------------------ SHA-1 */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f, k, t;
    for (int i = 0; i < 80; i++) {
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void rr_sha1_init(rr_sha1_ctx *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->len = 0;
    c->fill = 0;
}

void rr_sha1_update(rr_sha1_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    if (c->fill) {
        size_t take = 64 - c->fill;
        if (take > n) take = n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take; p += take; n -= take;
        if (c->fill == 64) { sha1_block(c->h, c->buf); c->fill = 0; }
    }
    while (n >= 64) { sha1_block(c->h, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, n); c->fill = n; }
}

void rr_sha1_final(rr_sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    rr_sha1_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->fill != 56) rr_sha1_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    rr_sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8); out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

void rr_sha1(const void *data, size_t n, uint8_t out[20])
{
    rr_sha1_ctx c;
    rr_sha1_init(&c);
    rr_sha1_update(&c, data, n);
    rr_sha1_final(&c, out);
}

/* ------------------------------------------------------------------- AES */

static uint8_t SBOX[256], INV[256];
static uint32_t TE[4][256], TD[4][256];
static int tables_ready;
static int force_portable;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}

static void build_tables(void)
{
    if (tables_ready) return;
    /* S-box by multiplicative inverse + affine transform */
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        SBOX[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    SBOX[0] = 0x63;
    for (int i = 0; i < 256; i++) INV[SBOX[i]] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        uint8_t s = SBOX[i];
        uint32_t e = (uint32_t)gmul(s, 2) << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | gmul(s, 3);
        uint8_t v = INV[i];
        uint32_t d = (uint32_t)gmul(v, 14) << 24 | (uint32_t)gmul(v, 9) << 16 | (uint32_t)gmul(v, 13) << 8 | gmul(v, 11);
        for (int t = 0; t < 4; t++) {
            TE[t][i] = (e >> (8 * t)) | (e << (32 - 8 * t));
            TD[t][i] = (d >> (8 * t)) | (d << (32 - 8 * t));
        }
    }
    tables_ready = 1;
}

static uint32_t load_be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void store_be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int ni_supported(void)
{
#ifdef RR_X86
    if (force_portable) return 0;
    {   /* CPUID leaf 1: ECX bit 25 = AES-NI, EDX bit 26 = SSE2 (no libgcc cpu model dependency) */
        unsigned a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(1), "2"(0));
        return ((c >> 25) & 1) && ((d >> 26) & 1);
    }
#else
    return 0;
#endif
}

void rr_aes_force_portable(int on) { force_portable = on; }

/* Portable key schedule (word-based, big-endian words). rk: 44 words stored in enc[] as bytes BE. */
static void expand_key(const uint8_t key[16], uint32_t rk[44])
{
    static const uint8_t rcon[10] = {1, 2, 4, 8, 16, 32, 64, 128, 0x1B, 0x36};
    for (int i = 0; i < 4; i++) rk[i] = load_be(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);
            t = (uint32_t)SBOX[t >> 24] << 24 | (uint32_t)SBOX[(t >> 16) & 255] << 16 | (uint32_t)SBOX[(t >> 8) & 255] << 8 | SBOX[t & 255];
            t ^= (uint32_t)rcon[i / 4 - 1] << 24;
        }
        rk[i] = rk[i - 4] ^ t;
    }
}

#ifdef RR_X86
__attribute__((target("aes,sse2"))) static void ni_prepare(rr_aes128 *k)
{
    /* enc[] holds the plain (byte-order) round keys; build InvMixColumns keys into dec[] */
    __m128i *e = (__m128i *)k->enc, *d = (__m128i *)k->dec;
    _mm_storeu_si128(d + 0, _mm_loadu_si128(e + 10));
    for (int i = 1; i < 10; i++) _mm_storeu_si128(d + i, _mm_aesimc_si128(_mm_loadu_si128(e + 10 - i)));
    _mm_storeu_si128(d + 10, _mm_loadu_si128(e + 0));
}
#endif

static void make_drk(const rr_aes128 *k, uint32_t drk[44]);

void rr_aes128_set_key(rr_aes128 *k, const uint8_t key[16])
{
    build_tables();
    uint32_t rk[44];
    expand_key(key, rk);
    /* enc: round keys as bytes in natural order (works for AES-NI and for the portable path) */
    for (int i = 0; i < 44; i++) store_be(k->enc + 4 * i, rk[i]);
    k->use_ni = ni_supported();
#ifdef RR_X86
    if (k->use_ni) ni_prepare(k);
#endif
    if (!k->use_ni) {
        uint32_t drk[44];
        make_drk(k, drk);
        memcpy(k->dec, drk, sizeof(drk));
    }
}

static void blk_encrypt(const rr_aes128 *k, const uint8_t in[16], uint8_t out[16])
{
    uint32_t s0 = load_be(in) ^ load_be(k->enc), s1 = load_be(in + 4) ^ load_be(k->enc + 4);
    uint32_t s2 = load_be(in + 8) ^ load_be(k->enc + 8), s3 = load_be(in + 12) ^ load_be(k->enc + 12);
    for (int r = 1; r < 10; r++) {
        const uint8_t *rk = k->enc + 16 * r;
        uint32_t t0 = TE[0][s0 >> 24] ^ TE[1][(s1 >> 16) & 255] ^ TE[2][(s2 >> 8) & 255] ^ TE[3][s3 & 255] ^ load_be(rk);
        uint32_t t1 = TE[0][s1 >> 24] ^ TE[1][(s2 >> 16) & 255] ^ TE[2][(s3 >> 8) & 255] ^ TE[3][s0 & 255] ^ load_be(rk + 4);
        uint32_t t2 = TE[0][s2 >> 24] ^ TE[1][(s3 >> 16) & 255] ^ TE[2][(s0 >> 8) & 255] ^ TE[3][s1 & 255] ^ load_be(rk + 8);
        uint32_t t3 = TE[0][s3 >> 24] ^ TE[1][(s0 >> 16) & 255] ^ TE[2][(s1 >> 8) & 255] ^ TE[3][s2 & 255] ^ load_be(rk + 12);
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    const uint8_t *rk = k->enc + 160;
    uint32_t o0 = ((uint32_t)SBOX[s0 >> 24] << 24 | (uint32_t)SBOX[(s1 >> 16) & 255] << 16 | (uint32_t)SBOX[(s2 >> 8) & 255] << 8 | SBOX[s3 & 255]) ^ load_be(rk);
    uint32_t o1 = ((uint32_t)SBOX[s1 >> 24] << 24 | (uint32_t)SBOX[(s2 >> 16) & 255] << 16 | (uint32_t)SBOX[(s3 >> 8) & 255] << 8 | SBOX[s0 & 255]) ^ load_be(rk + 4);
    uint32_t o2 = ((uint32_t)SBOX[s2 >> 24] << 24 | (uint32_t)SBOX[(s3 >> 16) & 255] << 16 | (uint32_t)SBOX[(s0 >> 8) & 255] << 8 | SBOX[s1 & 255]) ^ load_be(rk + 8);
    uint32_t o3 = ((uint32_t)SBOX[s3 >> 24] << 24 | (uint32_t)SBOX[(s0 >> 16) & 255] << 16 | (uint32_t)SBOX[(s1 >> 8) & 255] << 8 | SBOX[s2 & 255]) ^ load_be(rk + 12);
    store_be(out, o0); store_be(out + 4, o1); store_be(out + 8, o2); store_be(out + 12, o3);
}

/* Decryption with the straightforward inverse cipher (uses the encryption schedule in reverse). */
static void blk_decrypt(const rr_aes128 *k, const uint8_t in[16], uint8_t out[16])
{
    uint8_t st[16];
    const uint8_t *rk = k->enc + 160;
    for (int i = 0; i < 16; i++) st[i] = in[i] ^ rk[i];
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        /* InvShiftRows + InvSubBytes */
        for (int c = 0; c < 4; c++)
            for (int rw = 0; rw < 4; rw++)
                t[((c + rw) & 3) * 4 + rw] = INV[st[c * 4 + rw]];
        rk = k->enc + 16 * r;
        for (int i = 0; i < 16; i++) t[i] ^= rk[i];
        if (r) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
                st[c * 4] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                st[c * 4 + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                st[c * 4 + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                st[c * 4 + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        } else memcpy(st, t, 16);
    }
    memcpy(out, st, 16);
}

/* Fast portable decryption using the equivalent inverse cipher and TD tables. */
static void blk_decrypt_fast(const uint32_t *drk, const uint8_t in[16], uint8_t out[16])
{
    uint32_t s0 = load_be(in) ^ drk[0], s1 = load_be(in + 4) ^ drk[1], s2 = load_be(in + 8) ^ drk[2], s3 = load_be(in + 12) ^ drk[3];
    for (int r = 1; r < 10; r++) {
        const uint32_t *rk = drk + 4 * r;
        uint32_t t0 = TD[0][s0 >> 24] ^ TD[1][(s3 >> 16) & 255] ^ TD[2][(s2 >> 8) & 255] ^ TD[3][s1 & 255] ^ rk[0];
        uint32_t t1 = TD[0][s1 >> 24] ^ TD[1][(s0 >> 16) & 255] ^ TD[2][(s3 >> 8) & 255] ^ TD[3][s2 & 255] ^ rk[1];
        uint32_t t2 = TD[0][s2 >> 24] ^ TD[1][(s1 >> 16) & 255] ^ TD[2][(s0 >> 8) & 255] ^ TD[3][s3 & 255] ^ rk[2];
        uint32_t t3 = TD[0][s3 >> 24] ^ TD[1][(s2 >> 16) & 255] ^ TD[2][(s1 >> 8) & 255] ^ TD[3][s0 & 255] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    const uint32_t *rk = drk + 40;
    store_be(out, ((uint32_t)INV[s0 >> 24] << 24 | (uint32_t)INV[(s3 >> 16) & 255] << 16 | (uint32_t)INV[(s2 >> 8) & 255] << 8 | INV[s1 & 255]) ^ rk[0]);
    store_be(out + 4, ((uint32_t)INV[s1 >> 24] << 24 | (uint32_t)INV[(s0 >> 16) & 255] << 16 | (uint32_t)INV[(s3 >> 8) & 255] << 8 | INV[s2 & 255]) ^ rk[1]);
    store_be(out + 8, ((uint32_t)INV[s2 >> 24] << 24 | (uint32_t)INV[(s1 >> 16) & 255] << 16 | (uint32_t)INV[(s0 >> 8) & 255] << 8 | INV[s3 & 255]) ^ rk[2]);
    store_be(out + 12, ((uint32_t)INV[s3 >> 24] << 24 | (uint32_t)INV[(s2 >> 16) & 255] << 16 | (uint32_t)INV[(s1 >> 8) & 255] << 8 | INV[s0 & 255]) ^ rk[3]);
}

static void make_drk(const rr_aes128 *k, uint32_t drk[44])
{
    /* equivalent inverse cipher key schedule: reverse round order, InvMixColumns on middle keys */
    for (int r = 0; r <= 10; r++) {
        for (int c = 0; c < 4; c++) {
            uint32_t w = load_be(k->enc + 16 * (10 - r) + 4 * c);
            if (r != 0 && r != 10) {
                uint8_t b[4] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
                uint8_t o0 = gmul(b[0], 14) ^ gmul(b[1], 11) ^ gmul(b[2], 13) ^ gmul(b[3], 9);
                uint8_t o1 = gmul(b[0], 9) ^ gmul(b[1], 14) ^ gmul(b[2], 11) ^ gmul(b[3], 13);
                uint8_t o2 = gmul(b[0], 13) ^ gmul(b[1], 9) ^ gmul(b[2], 14) ^ gmul(b[3], 11);
                uint8_t o3 = gmul(b[0], 11) ^ gmul(b[1], 13) ^ gmul(b[2], 9) ^ gmul(b[3], 14);
                w = (uint32_t)o0 << 24 | (uint32_t)o1 << 16 | (uint32_t)o2 << 8 | o3;
            }
            drk[4 * r + c] = w;
        }
    }
}

#ifdef RR_X86
__attribute__((target("aes,sse2"))) static void ni_cbc_encrypt(const rr_aes128 *k, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n)
{
    const __m128i *rk = (const __m128i *)k->enc;
    __m128i rks[11];
    for (int i = 0; i < 11; i++) rks[i] = _mm_loadu_si128(rk + i);
    __m128i prev = _mm_loadu_si128((const __m128i *)iv);
    for (size_t o = 0; o < n; o += 16) {
        __m128i b = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(in + o)), prev);
        b = _mm_xor_si128(b, rks[0]);
        for (int r = 1; r < 10; r++) b = _mm_aesenc_si128(b, rks[r]);
        b = _mm_aesenclast_si128(b, rks[10]);
        _mm_storeu_si128((__m128i *)(out + o), b);
        prev = b;
    }
}

__attribute__((target("aes,sse2"))) static void ni_cbc_decrypt(const rr_aes128 *k, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n)
{
    const __m128i *rk = (const __m128i *)k->dec;
    __m128i rks[11];
    for (int i = 0; i < 11; i++) rks[i] = _mm_loadu_si128(rk + i);
    __m128i prev = _mm_loadu_si128((const __m128i *)iv);
    size_t o = 0;
    for (; o + 64 <= n; o += 64) {
        __m128i c0 = _mm_loadu_si128((const __m128i *)(in + o)), c1 = _mm_loadu_si128((const __m128i *)(in + o + 16));
        __m128i c2 = _mm_loadu_si128((const __m128i *)(in + o + 32)), c3 = _mm_loadu_si128((const __m128i *)(in + o + 48));
        __m128i b0 = _mm_xor_si128(c0, rks[0]), b1 = _mm_xor_si128(c1, rks[0]), b2 = _mm_xor_si128(c2, rks[0]), b3 = _mm_xor_si128(c3, rks[0]);
        for (int r = 1; r < 10; r++) {
            b0 = _mm_aesdec_si128(b0, rks[r]); b1 = _mm_aesdec_si128(b1, rks[r]);
            b2 = _mm_aesdec_si128(b2, rks[r]); b3 = _mm_aesdec_si128(b3, rks[r]);
        }
        b0 = _mm_aesdeclast_si128(b0, rks[10]); b1 = _mm_aesdeclast_si128(b1, rks[10]);
        b2 = _mm_aesdeclast_si128(b2, rks[10]); b3 = _mm_aesdeclast_si128(b3, rks[10]);
        _mm_storeu_si128((__m128i *)(out + o), _mm_xor_si128(b0, prev));
        _mm_storeu_si128((__m128i *)(out + o + 16), _mm_xor_si128(b1, c0));
        _mm_storeu_si128((__m128i *)(out + o + 32), _mm_xor_si128(b2, c1));
        _mm_storeu_si128((__m128i *)(out + o + 48), _mm_xor_si128(b3, c2));
        prev = c3;
    }
    for (; o < n; o += 16) {
        __m128i c = _mm_loadu_si128((const __m128i *)(in + o));
        __m128i b = _mm_xor_si128(c, rks[0]);
        for (int r = 1; r < 10; r++) b = _mm_aesdec_si128(b, rks[r]);
        b = _mm_aesdeclast_si128(b, rks[10]);
        _mm_storeu_si128((__m128i *)(out + o), _mm_xor_si128(b, prev));
        prev = c;
    }
}
#endif

int rr_aes_using_ni(void) { return ni_supported(); }

void rr_aes128_cbc_encrypt(const rr_aes128 *k, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n)
{
#ifdef RR_X86
    if (k->use_ni) { ni_cbc_encrypt(k, iv, in, out, n); return; }
#endif
    uint8_t prev[16], x[16];
    memcpy(prev, iv, 16);
    for (size_t o = 0; o < n; o += 16) {
        for (int i = 0; i < 16; i++) x[i] = in[o + i] ^ prev[i];
        blk_encrypt(k, x, out + o);
        memcpy(prev, out + o, 16);
    }
}

void rr_aes128_cbc_decrypt(const rr_aes128 *k, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n)
{
#ifdef RR_X86
    if (k->use_ni) { ni_cbc_decrypt(k, iv, in, out, n); return; }
#endif
    const uint32_t *drk = (const uint32_t *)(const void *)k->dec;
    uint8_t prev[16], c[16], x[16];
    memcpy(prev, iv, 16);
    for (size_t o = 0; o < n; o += 16) {
        memcpy(c, in + o, 16);
        blk_decrypt_fast(drk, c, x);
        for (int i = 0; i < 16; i++) out[o + i] = x[i] ^ prev[i];
        memcpy(prev, c, 16);
    }
    (void)blk_decrypt;
}
