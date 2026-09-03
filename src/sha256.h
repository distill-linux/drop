#ifndef DISTILL_SHA256_H
#define DISTILL_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} distill_sha256_ctx;

void distill_sha256_init(distill_sha256_ctx *ctx);
void distill_sha256_update(distill_sha256_ctx *ctx, const void *data, size_t len);
void distill_sha256_final(distill_sha256_ctx *ctx, uint8_t digest[32]);
void distill_sha256_to_hex(const uint8_t digest[32], char hex[65]);
int distill_sha256_file(const char *path, char hex[65]);

#endif /* DISTILL_SHA256_H */
