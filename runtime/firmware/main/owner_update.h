#ifndef FLIGHTWALL_OWNER_UPDATE_H
#define FLIGHTWALL_OWNER_UPDATE_H
/* The owner's Wi-Fi update service on port 8787 (UPDATES.md): the same keyed
 * protocol as the owner-modified stock firmware, so the desk works unchanged —
 * GET /v1/firmware, PUT /v1/firmware, POST /v1/firmware/confirm — plus
 * POST /v1/firmware/switch to boot the image in the other slot (stock). */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FW_UPDATE_PORT 8787

/* Hashes the running image and starts the server. display_ok feeds the
 * health check a confirmation requires. */
bool fw_owner_update_start(bool display_ok);

/* True once this boot's pending image was confirmed (kept). */
bool fw_owner_update_confirmed(void);

#ifdef __cplusplus
}
#endif
#endif
