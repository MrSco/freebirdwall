#ifndef FLIGHTWALL_SETUP_SERVER_H
#define FLIGHTWALL_SETUP_SERVER_H
/* Setup mode (SETUP.md): the stock-compatible access point and provisioning
 * endpoints, plus the owned setup page. /provision test-joins the network;
 * with CONFIG_FW_SETUP_SAVE it then saves the station config exactly as stock
 * does and restarts, otherwise it restores the previous config. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_screens.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Starts the access point (alongside the station) and the servers. Wi-Fi
 * must already be initialised. Returns false and shows the stock error page
 * when the identity is missing or the access point cannot start. */
bool fw_setup_start(void);

/* Stops the servers and the access point (station only again). */
void fw_setup_stop(void);

/* Phones on the access point now (0 when setup is off), and when the current
 * ones arrived (ms since boot; a phone back within a minute keeps its time). */
int fw_setup_clients(uint32_t *joined);

bool fw_setup_active(void);

/* True when the stock HTTPS endpoints (and so the vendor QR page) are on. */
bool fw_setup_https(void);

/* The setup strings for the display (valid once fw_setup_start succeeded). */
const FwSetupInfo *fw_setup_info_get(void);

/* A short status message the panel shows instead of the pages, as stock does
 * (two lines; e.g. "Connecting...", "Wrong password?"). Returns false when no
 * message is current. */
bool fw_setup_message(char *line1, size_t cap1, char *line2, size_t cap2);

/* True while /provision owns the station: app_main's reconnect logic stands
 * aside until the trial join finishes and the old config is restored. */
bool fw_setup_station_busy(void);

#ifdef __cplusplus
}
#endif
#endif
