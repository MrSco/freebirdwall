#ifndef FLIGHTWALL_APP_SETTINGS_H
#define FLIGHTWALL_APP_SETTINGS_H
/* Changes made in the FlightWall app (SETTINGS.md "Changes made in the app"):
 * GET https://api.theflightwall.com/configuration with stock's API key (as
 * stock: GTS Root R4 pinned, `x-api-key`), merged into the owned settings one
 * setting at a time (components/fw_settings/fw_cloud.h).
 *
 * When, never on a blind timer: on the vendor feed as stock, once its
 * answer's config_hash differs from the last one handled (retried every 30 s
 * while that fails). Otherwise (the community source, or before the vendor
 * feed answers) once per Wi-Fi connection, as stock fetches on every connect
 * (retried every minute until it works), and when the dashboard is opened (at
 * most every 5 minutes). Runs on the feed task.
 *
 * Flash: the owned settings (fw_owned/config) and the fingerprints
 * (fw_owned/cloud_base, 520 bytes) are written only when a setting changed in
 * the app (the fingerprints also once, the first time); a fetch that finds
 * nothing new writes nothing. Logs counts only, never the document or key. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Loads the stored fingerprints (from fw_feed_start). */
void fw_app_settings_init(void);

/* The config_hash of a good vendor feed answer. */
void fw_app_settings_hash(const char *config_hash);

/* Feed task, once per pass while online: fetches and merges when due. key is
 * stock's API key (NULL: none on the board, nothing to do); follow is
 * owned_config.follow_app; by_hash is true while the vendor feed is answering
 * (its config_hash then says when); connection is fw_wifi_link_connections().
 * Returns true when it made a request. */
bool fw_app_settings_step(const char *key, bool follow, bool by_hash, unsigned connection);

/* The dashboard was opened: check soon when the feed's hash cannot say
 * (wakes the feed task). */
void fw_app_settings_ask(void);

/* The dashboard's "Copy everything from the app": queued for the feed task
 * (done even when following is off). */
void fw_app_settings_import(void);

typedef enum {
    FW_APP_NEVER,          /* not checked since start */
    FW_APP_OK,             /* checked; settings up to date */
    FW_APP_NETWORK,        /* the request failed (no answer, TLS, HTTP status) */
    FW_APP_REFUSED,        /* the answer was not a usable configuration */
    FW_APP_NOT_SAVED,      /* changed on the dashboard meanwhile, or flash failed: tried again */
} FwAppResult;

typedef struct {
    bool pending;          /* a full import is queued */
    FwAppResult result;
    uint32_t checked;      /* unix time of the last check, 0 never */
    uint32_t changed_at;   /* unix time settings last came from the app, 0 never (since start) */
    int changed;           /* how many settings then */
    unsigned status;       /* HTTP status of the last check */
} FwAppStatus;

void fw_app_settings_status(FwAppStatus *out);

#ifdef __cplusplus
}
#endif
#endif
