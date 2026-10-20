#ifndef FW_CLOUD_H
#define FW_CLOUD_H
/* Following the FlightWall app (SETTINGS.md "Changes made in the app"): the
 * vendor's configuration (GET /configuration, the same document stock stores
 * in usercfg/json) is merged into the owned settings one setting at a time.
 *
 * The wall keeps a fingerprint of every setting as the app last had it
 * (FwCloudBase). When a fetched configuration differs, only the settings whose
 * fingerprint changed are copied over (or removed, when the app removed them):
 * a change made in the app wins for that setting, and everything changed only
 * on the dashboard stays. The first configuration ever seen only sets the
 * fingerprints, so nothing the dashboard already changed is undone by old app
 * values. owned_config is never touched. Portable C (cJSON). */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FW_CLOUD_UNITS 64            /* settings tracked (stock's documents have about 35) */
#define FW_CLOUD_MAGIC 0x31444C43u   /* "CLD1" */

/* What is stored (NVS fw_owned/cloud_base, 520 bytes). A setting is one
 * member of a section (display_config.brightness_percent), a top-level value
 * (version), or the request's areas together (radius_request, radius_requests,
 * geo_request, geo_requests, type: one setting, since they replace each
 * other). */
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t key[FW_CLOUD_UNITS];
    uint32_t value[FW_CLOUD_UNITS];
} FwCloudBase;

enum {
    FW_CLOUD_OK = 0,
    FW_CLOUD_BAD_CONFIG = -1,        /* not stock-usable: no request_config/display_config objects */
    FW_CLOUD_BAD_OWNED = -2,         /* the owned document is not a JSON object */
    FW_CLOUD_TOO_LARGE = -3,         /* the result would pass FW_SETTINGS_MAX, or out of memory */
    FW_CLOUD_TOO_MANY = -4,          /* more than FW_CLOUD_UNITS settings */
};

/* Merges a fetched configuration (cloud) into the owned document.
 *  old:   the stored fingerprints, NULL (or a wrong magic) when there are none
 *         yet: then only *base is set.
 *  full:  copy everything from the app (the dashboard's "Import from the
 *         FlightWall app"): the result is the app's document plus the owned
 *         owned_config.
 * On FW_CLOUD_OK *base holds the new fingerprints, *changed how many settings
 * were copied or removed, and *doc the new document (free()) or NULL when it
 * is unchanged. */
int fw_cloud_merge(const char *owned, const char *cloud, const FwCloudBase *old, bool full,
                   FwCloudBase *base, char **doc, int *changed);

/* True when both hold the same fingerprints. */
bool fw_cloud_same(const FwCloudBase *a, const FwCloudBase *b);

#ifdef __cplusplus
}
#endif
#endif
