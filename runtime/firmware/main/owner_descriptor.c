#include "owner_descriptor.h"

typedef struct {
    char magic[8];
    uint32_t version;
    uint8_t key[32];
} FwOwnerDescriptor;

_Static_assert(sizeof(FwOwnerDescriptor) == 44, "descriptor must be exactly 44 bytes");

/* The magic is spelled out byte by byte so no string literal elsewhere can be
 * merged with it; it must occur exactly once in the image. */
static const FwOwnerDescriptor s_descriptor __attribute__((used, aligned(4))) = {
    {'F', 'W', 'O', 'T', 'A', '1', 0, 0},
    0, /* placeholder; 1 after packaging */
    {0},
};

uint32_t fw_owner_descriptor_version(void){
    return *(const volatile uint32_t *)&s_descriptor.version;
}

bool fw_owner_descriptor_keyed(void){
    const volatile uint8_t *key = s_descriptor.key;
    uint8_t any = 0;
    for (unsigned i = 0; i < sizeof s_descriptor.key; ++i) any |= key[i];
    return any != 0;
}

void fw_owner_descriptor_key(uint8_t out[32]){
    const volatile uint8_t *key = s_descriptor.key;
    for (unsigned i = 0; i < sizeof s_descriptor.key; ++i) out[i] = key[i];
}
