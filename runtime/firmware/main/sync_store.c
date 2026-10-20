/* The owned Sync schedule on the board; see sync_store.h. */
#include "sync_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "memory_probe.h"

#define NS "fw_owned"
#define KEY "sched"

static FwSchedule s_sched;               /* ~1.6 KB */
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_serial;

void fw_sync_store_init(void){
    nvs_handle_t h;
    uint8_t *blob;
    size_t n = FW_SCHED_BLOB_MAX;
    char extra[64];
    bool loaded = false;
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    fw_sched_init(&s_sched);
    if ((blob = malloc(FW_SCHED_BLOB_MAX)) != NULL) {
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            loaded = nvs_get_blob(h, KEY, blob, &n) == ESP_OK && fw_sched_load(&s_sched, blob, n);
            nvs_close(h);
        }
        free(blob);
    }
    ++s_serial;
    snprintf(extra, sizeof extra, "\"loaded\":%s,\"legs\":%d,\"enabled\":%s", loaded ? "true" : "false", s_sched.count,
             s_sched.enabled ? "true" : "false");
    fw_mem_report("sync_loaded", extra);
}

void fw_sync_store_view(uint32_t now, FwSchedView *out){
    if (!s_lock) { memset(out, 0, sizeof *out); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_sched_view(&s_sched, now, out);
    if (out->message) {                  /* the view points into the schedule: copy the text out */
        static char msg[FW_SYNC_MESSAGE_MAX];
        snprintf(msg, sizeof msg, "%s", out->message);
        out->message = msg;
    }
    xSemaphoreGive(s_lock);
}

bool fw_sync_store_tracking(uint32_t now, FwSyncLeg *out){
    static FwSchedView v;                /* one caller: the feed task */
    bool on = false;
    if (!s_lock || !s_sched.enabled) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_sched_view(&s_sched, now, &v);
    if (v.has_active && fw_sched_live_window(&v.active, now)) { *out = v.active; on = true; }
    xSemaphoreGive(s_lock);
    return on;
}

uint32_t fw_sync_store_serial(void){ return s_serial; }
bool fw_sync_store_enabled(void){ return s_lock && s_sched.enabled; }
bool fw_sync_store_show_upcoming(void){ return s_lock && s_sched.show_upcoming; }
bool fw_sync_store_message_view(void){ return s_lock && s_sched.message_view; }
bool fw_sync_store_list_view(void){ return s_lock && s_sched.list_view; }

size_t fw_sync_store_json(char *buf, size_t cap){
    size_t n;
    if (!s_lock) return (size_t)snprintf(buf, cap, "{\"enabled\":false,\"legs\":[]}");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = fw_sched_json(&s_sched, buf, cap);
    xSemaphoreGive(s_lock);
    return n;
}

bool fw_sync_store_put(const char *json, char *why, size_t why_cap){
    FwSchedule *next = malloc(sizeof *next);
    uint8_t *blob = malloc(FW_SCHED_BLOB_MAX), *old = malloc(FW_SCHED_BLOB_MAX);
    size_t n, old_n = FW_SCHED_BLOB_MAX;
    nvs_handle_t h;
    bool ok = false, same = false;
    char extra[64];
    if (!s_lock || !next || !blob || !old) { snprintf(why, why_cap, "%s", s_lock ? "out of memory" : "starting up"); goto done; }
    if (!fw_sched_parse(json, next, why, why_cap)) goto done;
    n = fw_sched_save(next, blob, FW_SCHED_BLOB_MAX);
    if (!n) { snprintf(why, why_cap, "schedule too large"); goto done; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        same = nvs_get_blob(h, KEY, old, &old_n) == ESP_OK && old_n == n && !memcmp(old, blob, n);
        /* only a real change reaches flash */
        ok = same || (nvs_set_blob(h, KEY, blob, n) == ESP_OK && nvs_commit(h) == ESP_OK);
        nvs_close(h);
    }
    if (ok) { s_sched = *next; ++s_serial; }
    else snprintf(why, why_cap, "not saved");
    xSemaphoreGive(s_lock);
    snprintf(extra, sizeof extra, "\"ok\":%s,\"written\":%s,\"bytes\":%u,\"legs\":%d", ok ? "true" : "false",
             ok && !same ? "true" : "false", (unsigned)n, next->count);
    fw_mem_report("sync_saved", extra);
done:
    free(next); free(blob); free(old);
    return ok;
}
