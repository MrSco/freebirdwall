#ifndef FLIGHTWALL_WIFI_LINK_H
#define FLIGHTWALL_WIFI_LINK_H
/* Wi-Fi on the board: stock's saved networks, its join / lose / reconnect
 * rules (components/fw_wifi fw_link) and setup mode only when Wi-Fi fails.
 * SETUP.md "Wi-Fi failure and reconnect". */
#include <stdbool.h>
#include <stdint.h>
#include "fw_link.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Brings Wi-Fi up (station, RAM-only driver storage), reads stock's saved
 * networks and starts the link task. Returns false when Wi-Fi could not be
 * initialised at all. */
bool fw_wifi_link_start(void);

/* Waits until the board is online (connected, network check passed), up to
 * `ms`. */
bool fw_wifi_link_wait_online(uint32_t ms);

/* What the panel should show for the link (a copy; lines point to constant
 * strings). */
void fw_wifi_link_view(FwLinkView *out);

/* Reconnect attempts so far, for reports. */
unsigned fw_wifi_link_attempts(void);

/* Counts the times the board got an address: a new value means a new
 * connection (app_settings.cpp checks the app's settings once per connection,
 * as stock fetches its configuration on every connect). */
unsigned fw_wifi_link_connections(void);

#ifdef __cplusplus
}
#endif
#endif
