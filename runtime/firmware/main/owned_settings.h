#ifndef FLIGHTWALL_OWNED_SETTINGS_H
#define FLIGHTWALL_OWNED_SETTINGS_H
/* Settings that only the owned firmware has (DASHBOARD.md), kept in their own
 * NVS namespace "fw_owned": stock never reads it, and nothing here touches
 * stock's namespaces. Written only when a value actually changes. */
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool games_in_rotation;   /* "Scan to play" between the normal screens; default off */
} FwOwnedSettings;

/* Reads the settings (defaults for anything missing). Safe to call often. */
void fw_owned_settings_get(FwOwnedSettings *out);

/* Stores games_in_rotation if it differs. *written tells whether NVS changed. */
esp_err_t fw_owned_set_games_in_rotation(bool on, bool *written);

#ifdef __cplusplus
}
#endif
#endif
