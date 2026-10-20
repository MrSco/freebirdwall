#ifndef FLIGHTWALL_DASHBOARD_API_H
#define FLIGHTWALL_DASHBOARD_API_H
/* The owned dashboard's sign-in and settings API (SETTINGS.md). */
#include <stdbool.h>
#include "esp_http_server.h"
#ifdef __cplusplus
extern "C" {
#endif

void fw_dashboard_api_register(httpd_handle_t h);

/* The pairing code the wall should show now, if any. */
bool fw_dashboard_pair_code(char code[7], int *seconds_left);

#ifdef __cplusplus
}
#endif
#endif
