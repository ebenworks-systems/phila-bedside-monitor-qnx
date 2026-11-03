/*
 * sha256.h - SHA-256 (FIPS 180-4), small and dependency-free, used to chain
 * the entries of the append-only record so any later edit is detectable.
 * SPDX-License-Identifier: MIT
 */
#ifndef PHILA_SHA256_H
#define PHILA_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN 64

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t block[64];
    size_t block_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, uint8_t out[SHA256_DIGEST_LEN]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* Writes 64 lowercase hex characters and a terminating NUL (65 bytes). */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_LEN], char out[SHA256_HEX_LEN + 1]);

#ifdef __cplusplus
}
#endif
#endif /* PHILA_SHA256_H */
