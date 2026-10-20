#pragma once
/* Owner-update capability descriptor (44 bytes, in flash read-only data).
 *
 * The device's Wi-Fi updater (shared/owner_ota.c) only accepts an
 * image containing exactly one "FWOTA1\0\0" + version 1 + this device's 32-byte
 * key. The build leaves a placeholder (version 0, zero key) so the compiled
 * image carries no secret; tools/package_owned.py writes version 1 and the key
 * into the finished image and recomputes its checksum and SHA-256. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Read through volatile pointers: the packer changes these bytes after
 * linking, so the compiler must never fold the placeholder values. */
uint32_t fw_owner_descriptor_version(void);
bool fw_owner_descriptor_keyed(void);
/* The device key the packer wrote (all zero in an unpackaged build). It is
 * also the owner's bearer key for the update service; never log it. */
void fw_owner_descriptor_key(uint8_t out[32]);

#ifdef __cplusplus
}
#endif
