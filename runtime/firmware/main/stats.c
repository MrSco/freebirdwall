/* Flight stats on the board; see stats.h. */
#include "stats.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "fw_analytics.h"
#include "fw_clock.h"
#include "fw_feed.h"
#include "feed.h"
#include "memory_probe.h"

static FwAnalytics *s_an;          /* ~8 KiB, allocated once at start */
static SemaphoreHandle_t s_lock;
static uint32_t s_now;             /* set by fw_stats_begin, used by its flights */
static int s_offset;
static bool s_ready;

#define CLOCK_SET 1700000000u

void fw_stats_init(void){
    if (s_an) return;
    s_lock = xSemaphoreCreateMutex();
    s_an = heap_caps_malloc(sizeof *s_an, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_lock || !s_an) { fw_mem_report("stats_no_memory", NULL); return; }
    fw_an_init(s_an);
}

/* The settings' zone at `now`: standard offset, plus an hour inside daylight
 * saving; no zone = UTC. */
static int local_offset(uint32_t now){
    static FwFeedDisplay d;          /* feed task only */
    FwZone z;
    FwTime t;
    fw_feed_display_copy(&d);
    if (d.utc_offset_min == FW_FEED_ZONE_UNSET) return 0;
    z.standard_offset_min = d.utc_offset_min;
    z.dst_rule = !strcmp(d.dst_rule, "north_america") ? FW_DST_NORTH_AMERICA
               : !strcmp(d.dst_rule, "europe") ? FW_DST_EUROPE : FW_DST_NONE;
    return fw_local_time((int64_t)now, z, &t) && t.dst ? d.utc_offset_min + 60 : d.utc_offset_min;
}

bool fw_stats_begin(void){
    time_t now = time(NULL);
    s_ready = s_an && now >= (time_t)CLOCK_SET;
    if (!s_ready) return false;
    s_now = (uint32_t)now;
    s_offset = local_offset(s_now);
    return true;
}

static bool letters3(const char *s){
    return s && s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z' && s[2] >= 'A' && s[2] <= 'Z';
}

void fw_stats_flight(const char *hex, const char *callsign, const char *reg, const char *airline){
    char code[FW_AN_CODE] = {0};
    uint32_t key;
    if (!s_ready) return;
    if (letters3(airline) && !airline[3]) memcpy(code, airline, 3);
    else if (letters3(callsign) && callsign[3] >= '0' && callsign[3] <= '9') memcpy(code, callsign, 3);
    key = fw_an_flight_key(hex, callsign, reg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_an_observe(s_an, key, code[0] ? code : NULL, s_now, s_offset);
    xSemaphoreGive(s_lock);
}

void fw_stats_covered(void){
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_an_covered(s_an, s_now, s_offset);
    xSemaphoreGive(s_lock);
}

size_t fw_stats_json(char *buf, size_t cap){
    time_t now = time(NULL);
    bool set = now >= (time_t)CLOCK_SET;
    size_t head, body;
    if (!s_an) return (size_t)snprintf(buf, cap, "{\"hour\":-1,\"day\":-1,\"offset\":0,\"stats\":{\"hours\":[],\"days\":[]}}");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    head = (size_t)snprintf(buf, cap, "{\"hour\":%ld,\"day\":%ld,\"offset\":%d,\"stats\":",
                            set ? (long)fw_an_local_hour((uint32_t)now, s_offset) : -1L,
                            set ? (long)fw_an_local_day((uint32_t)now, s_offset) : -1L, s_offset);
    body = fw_an_json(s_an, head < cap ? buf + head : NULL, head < cap ? cap - head : 0);
    xSemaphoreGive(s_lock);
    if (head + body + 1 < cap) { buf[head + body] = '}'; buf[head + body + 1] = 0; }
    return head + body + 1;
}
