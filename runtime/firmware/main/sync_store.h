#ifndef FLIGHTWALL_SYNC_STORE_H
#define FLIGHTWALL_SYNC_STORE_H
/* The owned Sync schedule on the board (SYNC.md "Owned Sync"): one NVS blob,
 * fw_owned/sched (under 1 KB), read at start and written only when the owner
 * saves a changed schedule on the dashboard: no routine flash writes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_schedule.h"
#ifdef __cplusplus
extern "C" {
#endif

void fw_sync_store_init(void);

/* What the screens show now (the schedule worked out at `now`). */
void fw_sync_store_view(uint32_t now, FwSchedView *out);

/* The leg to track live now (fw_sched_live_window), copied to *out; false
 * when Sync is off or no leg is in its window. The feed task only. */
bool fw_sync_store_tracking(uint32_t now, FwSyncLeg *out);

/* Bumps whenever the schedule changes. */
uint32_t fw_sync_store_serial(void);
bool fw_sync_store_enabled(void);
bool fw_sync_store_show_upcoming(void);   /* plus.sync.display.show_upcoming */
bool fw_sync_store_message_view(void);    /* main_view "message" */
bool fw_sync_store_list_view(void);       /* upcoming_view "list" */

/* The schedule as JSON (fw_sched_json); returns the length needed. */
size_t fw_sync_store_json(char *buf, size_t cap);

/* Validates and saves (fw_sched_parse). Writes flash only when the result
 * differs from what is stored. false with a reason on any error. */
bool fw_sync_store_put(const char *json, char *why, size_t why_cap);

#ifdef __cplusplus
}
#endif
#endif
