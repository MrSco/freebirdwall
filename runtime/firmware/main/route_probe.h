#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Runs CONFIG_FW_PROBE_ROUNDS bounded HTTPS route requests through the ESP32
 * transport and the source-built TLS engine, printing an FWMEM line after each
 * handshake and each completed request. Returns true if every round succeeded
 * and left zero arena allocations behind. Requires Wi-Fi up, entropy enabled
 * and a valid wall clock. */
bool fw_route_probe_run(uint8_t *arena, size_t arena_bytes,
                        void *context_storage, size_t context_bytes);

#ifdef __cplusplus
}
#endif
