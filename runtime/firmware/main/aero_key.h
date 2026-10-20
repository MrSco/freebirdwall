#ifndef FLIGHTWALL_AERO_KEY_H
#define FLIGHTWALL_AERO_KEY_H
/* The owner's own FlightAware AeroAPI key (FEED.md "Tracked flights on the
 * community source"): NVS fw_owned/aero_key, written only when the owner
 * saves or removes it on the dashboard. Never logged, never sent back in
 * full (the dashboard sees its last four characters). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FW_AERO_KEY_MAX 64

/* The key into out (cap > FW_AERO_KEY_MAX); false when none is saved. */
bool fw_aero_key_load(char *out, size_t cap);

/* Saves a key (8..64 of A-Z a-z 0-9 - _), or removes it with NULL or "".
 * ESP_ERR_INVALID_ARG for anything else. Bumps fw_aero_key_serial. */
esp_err_t fw_aero_key_save(const char *key);

/* Changes whenever the key is saved or removed. */
uint32_t fw_aero_key_serial(void);

#ifdef __cplusplus
}
#endif
#endif
