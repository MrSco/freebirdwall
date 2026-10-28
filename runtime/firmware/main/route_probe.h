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

/* True from the start until app_main.c has run the startup route probe (or
 * skipped it). Its TLS handshake takes the heap down to about 21 KB; work
 * that needs heap of its own and can wait (the 130 KB display preview,
 * games_server.c) waits for this. */
bool fw_boot_probe_pending(void);

#ifdef __cplusplus
}
#endif
