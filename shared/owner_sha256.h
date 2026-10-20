#ifndef FLIGHTWALL_OWNER_SHA256_H
#define FLIGHTWALL_OWNER_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    unsigned char block[64];
    unsigned int used;
} OwnerSha256;
void owner_sha256_init(OwnerSha256 *);
void owner_sha256_update(OwnerSha256 *, const void *, size_t);
void owner_sha256_final(OwnerSha256 *, unsigned char digest[32]);
#endif
