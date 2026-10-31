/* Cross-checks rr_crypto against OpenSSL (host test only). */
#include "../core/rr_crypto.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void rnd(uint8_t *p, size_t n, uint32_t *s) { for (size_t i = 0; i < n; i++) { *s = *s * 1664525u + 1013904223u; p[i] = (uint8_t)(*s >> 24); } }

static void ossl_cbc(int enc, const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int l;
    EVP_CipherInit_ex(c, EVP_aes_128_cbc(), NULL, key, iv, enc);
    EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_CipherUpdate(c, out, &l, in, (int)n);
    EVP_CipherFinal_ex(c, out + l, &l);
    EVP_CIPHER_CTX_free(c);
}

int main(void)
{
    uint32_t seed = 12345;
    for (int len = 0; len < 300; len += 7) {
        uint8_t d[512], a[20], b[20]; rnd(d, sizeof d, &seed);
        rr_sha1(d, len, a); SHA1(d, len, b);
        CHECK(!memcmp(a, b, 20), "sha1 len %d", len);
        rr_sha1_ctx c; rr_sha1_init(&c); rr_sha1_update(&c, d, len / 2); rr_sha1_update(&c, d + len / 2, len - len / 2); rr_sha1_final(&c, a);
        CHECK(!memcmp(a, b, 20), "sha1 split %d", len);
    }
    for (int mode = 0; mode < 2; mode++) {
        rr_aes_force_portable(mode);
        printf("AES path: %s\n", rr_aes_using_ni() ? "AES-NI" : "portable");
        for (int t = 0; t < 40; t++) {
            uint8_t key[16], iv[16], in[0x7C00 + 64], e1[sizeof in], e2[sizeof in], d1[sizeof in];
            size_t n = (size_t)(1 + (t * 37) % 2000) * 16; if (n > sizeof in) n = sizeof in & ~15u;
            rnd(key, 16, &seed); rnd(iv, 16, &seed); rnd(in, n, &seed);
            rr_aes128 k; rr_aes128_set_key(&k, key);
            rr_aes128_cbc_encrypt(&k, iv, in, e1, n); ossl_cbc(1, key, iv, in, e2, n);
            CHECK(!memcmp(e1, e2, n), "enc mode %d t %d", mode, t);
            rr_aes128_cbc_decrypt(&k, iv, e2, d1, n);
            CHECK(!memcmp(d1, in, n), "dec mode %d t %d", mode, t);
        }
    }
    printf(fails ? "crypto: %d FAILURES\n" : "crypto: all passed\n", fails);
    return fails != 0;
}
