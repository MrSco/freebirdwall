/* Changes made in the FlightWall app; see app_settings.h. */
#include "app_settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "https_get.h"
#include "owner_tls_root.h"     /* GTS Root R4, as stock pins for /configuration */
#include "fw_cloud.h"
#include "fw_settings.h"
#include "settings_store.h"
#include "feed.h"
#include "memory_probe.h"

namespace {

constexpr const char *Host = "api.theflightwall.com";
constexpr uint32_t RetryHashMs = 30000;            /* stock: 30 s between attempts */
constexpr uint32_t RetryMs = 60000;                /* the connect check, while it fails */
constexpr uint32_t AskGapMs = 5 * 60000;           /* dashboard asks: at most one check per 5 min */
constexpr uint32_t AdoptMs = 120000;               /* a check this recent covers the feed's first hash */

FwCloudBase s_base;
bool s_has_base;
char s_hash[40], s_seen[40];                       /* the newest config_hash, the last one handled */
volatile bool s_full;
int s_full_tries;
uint32_t s_last_try, s_ok_at;
bool s_tried, s_ok;
unsigned s_ok_connection;                          /* the connection the last good check was on */
volatile bool s_asked;
unsigned s_requests;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
FwAppStatus s_status;

uint32_t now_ms(){ return (uint32_t)(esp_timer_get_time() / 1000); }
uint32_t unix_now(){ time_t t = std::time(nullptr); return t > 1700000000 ? (uint32_t)t : 0; }

void set_status(FwAppResult r, unsigned status, int changed){
    portENTER_CRITICAL(&s_mux);
    s_status.pending = s_full;
    s_status.result = r;
    s_status.status = status;
    s_status.checked = unix_now();
    if (changed > 0) { s_status.changed_at = s_status.checked; s_status.changed = changed; }
    portEXIT_CRITICAL(&s_mux);
}

bool save_base(const FwCloudBase &b){
    nvs_handle_t h;
    esp_err_t e = nvs_open("fw_owned", NVS_READWRITE, &h);
    if (e != ESP_OK) return false;
    e = nvs_set_blob(h, "cloud_base", &b, sizeof b);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

/* GET /configuration into a fresh buffer (free()); NULL on failure. */
char *fetch(const char *key, int *stage, unsigned *status){
    char *buf = static_cast<char *>(std::malloc(FW_SETTINGS_MAX + 1));
    char headers[96];
    size_t n = 0;
    bool over = false;
    uint32_t retry;
    *status = 0;
    if (!buf) { *stage = 7; return nullptr; }
    std::snprintf(headers, sizeof headers, "x-api-key: %s\r\n", key);
    *stage = fw_https_get(Host, "/configuration", headers, reinterpret_cast<const uint8_t *>(owner_tls_gts_r4),
                          sizeof owner_tls_gts_r4, status, &retry, [&](flightwall::HttpBody &body){
        int c;
        while ((c = body.read()) >= 0) { if (n < FW_SETTINGS_MAX) buf[n++] = (char)c; else over = true; }
        return c == -1;
    });
    std::memset(headers, 0, sizeof headers);
    buf[n] = 0;
    if (*stage || *status != 200 || over) { std::free(buf); if (!*stage && over) *stage = 8; return nullptr; }
    return buf;
}

} // namespace

extern "C" void fw_app_settings_init(void){
    nvs_handle_t h;
    size_t n = sizeof s_base;
    if (nvs_open("fw_owned", NVS_READONLY, &h) == ESP_OK) {
        s_has_base = nvs_get_blob(h, "cloud_base", &s_base, &n) == ESP_OK && n == sizeof s_base &&
                     s_base.magic == FW_CLOUD_MAGIC && s_base.count <= FW_CLOUD_UNITS;
        nvs_close(h);
    }
}

extern "C" void fw_app_settings_hash(const char *config_hash){
    if (!config_hash || !config_hash[0] || std::strspn(config_hash, "0") == std::strlen(config_hash)) return;   /* stock: empty or all '0' means nothing */
    std::snprintf(s_hash, sizeof s_hash, "%s", config_hash);
}

extern "C" void fw_app_settings_import(void){
    s_full = true;
    portENTER_CRITICAL(&s_mux);
    s_status.pending = true;
    portEXIT_CRITICAL(&s_mux);
    fw_feed_settings_changed();                       /* wakes the feed task */
}

extern "C" void fw_app_settings_status(FwAppStatus *out){
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

extern "C" void fw_app_settings_ask(void){
    s_asked = true;
    fw_feed_settings_changed();
}

extern "C" bool fw_app_settings_step(const char *key, bool follow, bool by_hash, unsigned connection){
    uint32_t now = now_ms();
    bool full = s_full, due, asked = s_asked;
    s_asked = false;
    if (!key) {                                       /* no key on the board: nothing to ask with */
        if (full) { s_full = false; set_status(FW_APP_NETWORK, 0, 0); }
        return false;
    }
    bool this_connection = s_ok && s_ok_connection == connection;
    if (by_hash && !s_seen[0] && s_hash[0] && this_connection && now - s_ok_at < AdoptMs)
        std::snprintf(s_seen, sizeof s_seen, "%s", s_hash);   /* just checked: the feed's first hash is that one */
    if (full) due = !s_full_tries || now - s_last_try >= 5000;
    else if (!follow) due = false;
    else if (by_hash) due = s_hash[0] && std::strcmp(s_hash, s_seen) && (!s_tried || now - s_last_try >= RetryHashMs);
    else if (!this_connection) due = !s_tried || now - s_last_try >= RetryMs;        /* once per connection */
    else due = asked && now - s_ok_at >= AskGapMs;                                    /* the dashboard opened */
    if (!due) return false;

    /* Room first: the merge parses the app's document (and, when something
     * changed, the owned one) with cJSON, several KiB in small pieces. */
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 24 * 1024 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 8 * 1024) {
        s_tried = true;
        s_last_try = now;
        fw_mem_report("app_settings_low_memory", nullptr);
        return false;
    }
    static char handled[40];                          /* feed task only: kept off its stack */
    std::snprintf(handled, sizeof handled, "%s", s_hash);
    s_tried = true;
    s_last_try = now;
    ++s_requests;
    int stage = 0, rc = 0, changed = 0;
    unsigned status = 0;
    bool saved = false, based = false;
    FwAppResult result = FW_APP_OK;
    char *cloud = fetch(key, &stage, &status);
    if (!cloud) {
        result = FW_APP_NETWORK;
    } else {
        uint32_t rev = 0;
        char *owned = fw_store_settings(&rev);
        if (!owned && (owned = static_cast<char *>(std::malloc(std::strlen(fw_settings_default()) + 1))))
            std::strcpy(owned, fw_settings_default());          /* a board with no settings yet */
        static FwCloudBase next;                     /* 520 bytes, off the feed task's stack */
        char *doc = nullptr;
        rc = owned ? fw_cloud_merge(owned, cloud, s_has_base ? &s_base : nullptr, full, &next, &doc, &changed)
                   : FW_CLOUD_TOO_LARGE;
        if (rc == FW_CLOUD_BAD_CONFIG || rc == FW_CLOUD_TOO_MANY) result = FW_APP_REFUSED;
        else if (rc) result = FW_APP_NOT_SAVED;
        else {
            /* the settings first, then the fingerprints: a restart in
             * between only repeats the same merge */
            if (doc) saved = fw_store_save(doc, &rev) == ESP_OK;
            if (doc && !saved) result = FW_APP_NOT_SAVED;
            else if (!s_has_base || !fw_cloud_same(&s_base, &next)) {
                based = save_base(next);
                if (based) { s_base = next; s_has_base = true; }
                else result = FW_APP_NOT_SAVED;
            }
        }
        if (doc) { std::memset(doc, 0, std::strlen(doc)); std::free(doc); }
        if (owned) { std::memset(owned, 0, std::strlen(owned)); std::free(owned); }
        std::memset(cloud, 0, std::strlen(cloud));
        std::free(cloud);
    }
    if (result == FW_APP_OK) {
        if (by_hash) std::snprintf(s_seen, sizeof s_seen, "%s", handled);
        s_ok = true;
        s_ok_at = now;
        s_ok_connection = connection;
    }
    if (full && (result != FW_APP_NETWORK || ++s_full_tries >= 3)) {   /* a network failure: up to three tries */
        s_full = false;
        s_full_tries = 0;
    }
    set_status(result, status, saved ? changed : 0);
    static char extra[192];                           /* feed task only */
    std::snprintf(extra, sizeof extra,
                  "\"n\":%u,\"full\":%s,\"by_hash\":%s,\"stage\":%d,\"status\":%u,\"rc\":%d,\"result\":%d,"
                  "\"changed\":%d,\"saved\":%s,\"base_saved\":%s",
                  s_requests, full ? "true" : "false", by_hash ? "true" : "false", stage, status, rc, (int)result,
                  changed, saved ? "true" : "false", based ? "true" : "false");
    fw_mem_report("app_settings", extra);
    return true;
}
