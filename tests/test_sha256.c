/*
 * test_sha256.c - the SHA-256 test vectors of FIPS 180-4 (NIST examples).
 * SPDX-License-Identifier: MIT
 */
#include "check.h"
#include "phila/sha256.h"

static void hex_of(const void *data, size_t len, char out[SHA256_HEX_LEN + 1])
{
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(data, len, d);
    sha256_hex(d, out);
}

int main(void)
{
    char hex[SHA256_HEX_LEN + 1];

    hex_of("abc", 3, hex);
    CHECK_EQ_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    hex_of("", 0, hex);
    CHECK_EQ_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    hex_of(m448, strlen(m448), hex);
    CHECK_EQ_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
                       "klmnopqrlmnopqrsmnopqrstnopqrstu";
    hex_of(m896, strlen(m896), hex);
    CHECK_EQ_STR(hex, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    /* One million 'a', fed in uneven pieces to exercise the block buffer. */
    sha256_ctx_t c;
    sha256_init(&c);
    char chunk[997];
    memset(chunk, 'a', sizeof chunk);
    size_t left = 1000000;
    while (left > 0) {
        size_t n = left < sizeof chunk ? left : sizeof chunk;
        sha256_update(&c, chunk, n);
        left -= n;
    }
    uint8_t d[SHA256_DIGEST_LEN];
    sha256_final(&c, d);
    sha256_hex(d, hex);
    CHECK_EQ_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    return check_report("sha256");
}
