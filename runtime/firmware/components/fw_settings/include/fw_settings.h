#ifndef FW_SETTINGS_H
#define FW_SETTINGS_H
/* Owned settings (SETTINGS.md): a configuration document in the vendor's own
 * schema (version, request_config, display_config, plus), imported once from
 * the vendor app's stored copy and from then on edited on the owned
 * dashboard. This module validates an edit and merges it into the stored
 * document; the feed and the screens read the result with components/fw_feed.
 * Portable C (cJSON). */
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FW_SETTINGS_MAX 4096        /* stored document limit (as stock's usercfg/json) */

typedef struct {
    char field[96];                 /* e.g. "display_config.metric_display.vel_unit" */
    char why[64];
} FwSettingsError;

enum {
    FW_SETTINGS_OK = 0,
    FW_SETTINGS_INVALID = -1,       /* err says which field and why */
    FW_SETTINGS_TOO_LARGE = -2,
    FW_SETTINGS_BAD_CURRENT = -3,   /* the stored document is not a JSON object */
};

/* Applies an edit: {"display_config":{...},"request_config":{...},
 * "owned_config":{...}} with any
 * subset of the editable keys; each given key replaces the stored value after
 * validation, a null removes it (not radius_requests), everything else is
 * kept (plus, version, geo zones...). now_iso
 * stamps newly tracked flights (created_at). On success *out is a new
 * NUL-terminated document (free it with free()). */
int fw_settings_apply(const char *current, const char *edit, const char *now_iso, char **out, FwSettingsError *err);

/* A minimal document for a board without a vendor configuration. */
const char *fw_settings_default(void);

#ifdef __cplusplus
}
#endif
#endif
