#ifndef FLIGHTWALL_SAVED_NETWORKS_H
#define FLIGHTWALL_SAVED_NETWORKS_H
/* Stock's saved network list in NVS (namespace "wifi"; SETUP.md "Saved
 * networks"). The list format and rules are in components/fw_wifi. */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "fw_nets.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Holds a loaded list; the entries point into its own storage. */
typedef struct {
    FwNet net[FW_NETS_MAX];
    int count;
    bool has_nets;        /* the "nets" key exists */
    bool has_legacy;      /* any of the legacy "ssid"/"pass"/"open" keys exist */
    char *store;          /* heap; wiped and released by fw_saved_networks_free */
    size_t store_cap;
    char *nets_json;      /* the stored "nets" value, or NULL */
} FwSavedNetworks;

/* Reads the list read-only, as stock does (FUN_4202d6c4): "nets" when the key
 * exists, else the legacy single network. A missing namespace is an empty
 * list. Returns ESP_OK or an NVS/allocation error (list then empty). */
esp_err_t fw_saved_networks_load(FwSavedNetworks *s);
void fw_saved_networks_free(FwSavedNetworks *s);

/* Stock FUN_4202dbec: puts the network first in the list and writes "nets"
 * (then removes the legacy keys, as stock does after a successful write).
 * Skips the write when the stored value is already identical and no legacy
 * key is left. *written tells whether NVS was changed. An unusable network
 * or a list over FW_NETS_JSON_MAX bytes is not saved, as in stock
 * (ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE). */
esp_err_t fw_saved_networks_add(const FwNet *added, bool *written);

/* The station config stock builds (FUN_42036dc4, Arduino WiFi.begin): all-
 * channel scan, strongest access point first, RSSI threshold -127, WPA2
 * minimum when a password is given, PMF capable. */
void fw_station_config(wifi_config_t *c, const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
#endif
