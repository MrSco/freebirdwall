#ifndef FLIGHTWALL_SETTINGS_STORE_H
#define FLIGHTWALL_SETTINGS_STORE_H
/* The owned settings on the board (SETTINGS.md): one document in NVS
 * fw_owned/config with a revision (fw_owned/config_rev). The first time it is
 * missing, the vendor app's stored copy (usercfg/json) is imported once; from
 * then on the owned dashboard changes it, and changes made in the FlightWall
 * app arrive through app_settings.h. Stock's namespaces are only read. Also
 * keeps the paired browsers (fw_owned/pair). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "fw_pair.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Loads, importing once if needed. */
void fw_store_init(void);

/* A copy of the document (free() it), or NULL when there is none yet. */
char *fw_store_settings(uint32_t *revision);

/* The document copied into buf (cap bytes, NUL-terminated). Returns its
 * length, 0 when there is none yet (or not ready), -1 when cap is too small. */
int fw_store_settings_copy(char *buf, size_t cap, uint32_t *revision);

/* Bumps whenever the document changes (feed and display reload then). */
uint32_t fw_store_serial(void);

/* Saves a new document if revision still matches (ESP_ERR_INVALID_STATE
 * otherwise). *revision becomes the new one. */
esp_err_t fw_store_save(const char *doc, uint32_t *revision);

/* Before fw_store_init: fw_store_settings returns NULL (revision 0), saves
 * return ESP_ERR_INVALID_STATE and fw_store_pair_lock returns NULL. */

/* Pairing state, guarded by the store's lock (NULL when not ready). */
FwPair *fw_store_pair_lock(void);
void fw_store_pair_unlock(bool save);

#ifdef __cplusplus
}
#endif
#endif
