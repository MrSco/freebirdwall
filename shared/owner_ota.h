#ifndef FLIGHTWALL_OWNER_OTA_H
#define FLIGHTWALL_OWNER_OTA_H
#include <stddef.h>
#include <stdint.h>
#include "owner_sha256.h"

#define OO_FIRST_BYTES 288u
#define OO_MIN_IMAGE 320u
#define OO_DESCRIPTOR_BYTES 44u
enum {
    OO_OK = 0, OO_INVALID = -1, OO_IO = -2, OO_HASH = -3,
    OO_KEY = -4, OO_STATE = -5, OO_IMAGE = -6
};
enum { OO_RECEIVING = 1, OO_FAILED = 2, OO_SELECTED = 3 };

/* begin/write/end must target ONLY the inactive application partition.
 * end validates the ESP image, but never selects it. select changes the boot
 * partition only; it must not restart. abort must safely handle an already
 * ended operation. None of these callbacks may reenter the context. */
typedef struct {
    void *user;
    int (*begin)(void *, uint32_t);
    int (*write)(void *, const unsigned char *, size_t);
    int (*end)(void *);
    void (*abort)(void *);
    int (*select)(void *);
} OwnerOtaOps;

/* Caller owns the context. Initialize once per upload; failed/completed
 * contexts cannot be fed or finalized again. There is no allocator or BSS. */
typedef struct {
    OwnerOtaOps ops;
    OwnerSha256 hash;
    uint32_t expected_length, received;
    unsigned char expected_hash[32], key[32];
    unsigned char first[OO_FIRST_BYTES];
    unsigned char descriptor[OO_DESCRIPTOR_BYTES];
    unsigned int first_used, descriptor_used, descriptor_pos;
    unsigned int descriptors, begun, state;
    int error;
} OwnerOtaContext;

/* SHA is a transfer checksum, not a firmware signature. Authentication is
 * supplied by the HTTP adapter. The embedded descriptor binds future local
 * management to this device's existing key; SDK end verifies ESP integrity. */
int owner_ota_start(OwnerOtaContext *, const OwnerOtaOps *, uint32_t length,
                    const unsigned char expected_sha256[32],
                    const unsigned char active_key[32], uint32_t partition_size);
int owner_ota_feed(OwnerOtaContext *, const unsigned char *, size_t);
int owner_ota_finish(OwnerOtaContext *);
int owner_ota_cancel(OwnerOtaContext *);
#endif
