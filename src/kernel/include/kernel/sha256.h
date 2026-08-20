/*
 * QitoOS - SHA-256 for package integrity
 */

#ifndef QITO_SHA256_H
#define QITO_SHA256_H

#include <kernel/types.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE 64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t hash[32]);

int sha256_file(const char *path, uint8_t out[32]);
int sha256_data(const void *data, size_t len, uint8_t out[32]);
void sha256_to_hex(const uint8_t hash[32], char out_hex[65]);

#endif
