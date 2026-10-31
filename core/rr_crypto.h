/* Porpoise Retro Rewind - crypto primitives (SHA-1, AES-128).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small, dependency-free implementations used by the Wii disc engine.
 * AES has an AES-NI path (selected at runtime) and a portable fallback.
 */
#ifndef RR_CRYPTO_H
#define RR_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} rr_sha1_ctx;

void rr_sha1_init(rr_sha1_ctx *c);
void rr_sha1_update(rr_sha1_ctx *c, const void *data, size_t n);
void rr_sha1_final(rr_sha1_ctx *c, uint8_t out[20]);
void rr_sha1(const void *data, size_t n, uint8_t out[20]);

typedef struct {
    _Alignas(16) uint8_t enc[176]; /* 11 round keys, byte order (AES-NI and portable) */
    _Alignas(16) uint8_t dec[176]; /* AES-NI: InvMixColumns keys; portable: 44 host words */
    int use_ni;
} rr_aes128;

void rr_aes128_set_key(rr_aes128 *k, const uint8_t key[16]);
/* CBC over whole 16-byte blocks. iv is NOT updated. n must be a multiple of 16. */
void rr_aes128_cbc_encrypt(const rr_aes128 *k, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n);
void rr_aes128_cbc_decrypt(const rr_aes128 *k, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n);

/* Test hook: 1 forces the portable path, 0 restores auto-detection. */
void rr_aes_force_portable(int on);
int rr_aes_using_ni(void);

#ifdef __cplusplus
}
#endif
#endif
