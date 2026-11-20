/* On-demand "Explore anywhere in the world" live snapshot via adsb.fi; see explore.h. */
#include "explore.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "https_get.h"
#include "owner_tls_root.h"
#include "feed.h"
#include "memory_probe.h"

namespace {

constexpr uint32_t MinGapMs = 5000;          /* rate limit: at most once per 5 s */
constexpr uint32_t CacheTtlMs = 120000;      /* free result buffer after 2 min idle */
constexpr size_t BufCap = 6144;              /* up to ~80-100 compact aircraft entries */
constexpr int MaxAc = 100;

SemaphoreHandle_t s_lock;
volatile bool s_pending;
double s_req_lat, s_req_lon;
int s_req_dist;
uint32_t s_last_req_ms, s_buf_at_ms;
bool s_ok;
unsigned s_status;
uint32_t s_at_unix;
int s_count;
char *s_ac_json;                             /* allocated only while cached (<= BufCap) */

uint32_t now_ms(){ return (uint32_t)(esp_timer_get_time() / 1000); }

void ensure_lock(){ if (!s_lock) s_lock = xSemaphoreCreateMutex(); }

void trim_copy(char *dst, size_t cap, const char *src){
    size_t n = 0;
    if (!cap) return;
    while (*src == ' ') ++src;
    for (; *src && n + 1 < cap; ++src) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7f) continue;
        dst[n++] = (char)c;
    }
    while (n > 0 && dst[n - 1] == ' ') --n;
    dst[n] = 0;
}

/* Streaming byte-by-byte scanner for adsb.fi /api/v2/lat/.../lon/.../dist/...
 * Extracts compact records: [lat,lon,trk,"type","cs","reg",alt_ft] with zero
 * JSON DOM allocation. */
struct Scan {
    char *out;
    size_t cap, len;
    int count = 0;
    int depth = 0;
    bool in_ac = false, in_obj = false;
    bool in_str = false, esc = false, after_key = false, want_val = false;
    char key[24] = {0}, str[24] = {0}, num[24] = {0};
    size_t str_n = 0, num_n = 0;

    /* current aircraft fields */
    bool have_lat = false, have_lon = false;
    double lat = 0, lon = 0, trk = -1;
    int alt = -99999;
    char type[8] = {0}, cs[12] = {0}, reg[12] = {0};

    void reset_ac(){
        have_lat = have_lon = false;
        lat = lon = 0;
        trk = -1;
        alt = -99999;
        type[0] = cs[0] = reg[0] = 0;
    }

    void emit_ac(){
        if (!have_lat || !have_lon || count >= MaxAc) return;
        char row[128];
        int n = std::snprintf(row, sizeof row, "%s[%.4f,%.4f,%d,\"%s\",\"%s\",\"%s\",%d]",
                              count ? "," : "", lat, lon, trk >= 0 ? (int)std::lround(trk) : -1,
                              type, cs, reg, alt > -90000 ? alt : -1);
        if (n > 0 && len + (size_t)n + 4 < cap) {
            std::memcpy(out + len, row, (size_t)n);
            len += (size_t)n;
            out[len] = 0;
            ++count;
        }
    }

    void value(const char *v, bool quoted){
        if (!in_obj || depth != 3) return;
        if (!std::strcmp(key, "lat") && !quoted) { lat = std::strtod(v, nullptr); have_lat = true; }
        else if (!std::strcmp(key, "lon") && !quoted) { lon = std::strtod(v, nullptr); have_lon = true; }
        else if (!std::strcmp(key, "track") && !quoted) trk = std::strtod(v, nullptr);
        else if (!std::strcmp(key, "alt_baro")) alt = quoted ? 0 : (int)std::strtol(v, nullptr, 10);
        else if (!std::strcmp(key, "t") && quoted) trim_copy(type, sizeof type, v);
        else if (!std::strcmp(key, "flight") && quoted) trim_copy(cs, sizeof cs, v);
        else if (!std::strcmp(key, "r") && quoted) trim_copy(reg, sizeof reg, v);
    }

    void end_num(){
        if (!num_n) return;
        num[num_n] = 0;
        value(num, false);
        num_n = 0;
        want_val = false;
    }

    void feed(int c){
        if (in_str) {
            if (esc) { esc = false; return; }
            if (c == '\\') { esc = true; return; }
            if (c == '"') {
                in_str = false;
                str[str_n] = 0;
                if (want_val) { value(str, true); want_val = false; }
                else {
                    std::snprintf(key, sizeof key, "%s", str);
                    after_key = true;
                }
                return;
            }
            if (str_n + 1 < sizeof str) str[str_n++] = (char)c;
            return;
        }
        if (num_n && !(c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' || (c >= '0' && c <= '9'))) end_num();
        switch (c) {
        case '"': in_str = true; str_n = 0; break;
        case ':': if (after_key) { want_val = true; after_key = false; } break;
        case '[':
            ++depth;
            if (depth == 2 && (!std::strcmp(key, "ac") || !std::strcmp(key, "aircraft"))) in_ac = true;
            want_val = false;
            break;
        case ']':
            if (depth == 2) in_ac = false;
            --depth;
            want_val = false;
            break;
        case '{':
            ++depth;
            if (in_ac && depth == 3) { in_obj = true; reset_ac(); }
            want_val = false;
            break;
        case '}':
            if (in_obj && depth == 3) { emit_ac(); in_obj = false; }
            --depth;
            want_val = false;
            break;
        case ',': want_val = false; after_key = false; break;
        default:
            if (want_val && (c == '-' || (c >= '0' && c <= '9') || c == '.')) {
                if (num_n + 1 < sizeof num) num[num_n++] = (char)c;
            } else if (want_val && c > ' ') {
                want_val = false;
            }
        }
    }
};

} // namespace

extern "C" bool fw_explore_request(double lat, double lon, int dist_nm){
    ensure_lock();
    if (!s_lock || lat != lat || lon != lon || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    if (dist_nm < 5) dist_nm = 5;
    if (dist_nm > 150) dist_nm = 150;
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_last_req_ms && now - s_last_req_ms < MinGapMs && !s_pending) {
        xSemaphoreGive(s_lock);
        return false;
    }
    s_req_lat = lat;
    s_req_lon = lon;
    s_req_dist = dist_nm;
    s_pending = true;
    s_last_req_ms = now ? now : 1;
    xSemaphoreGive(s_lock);
    fw_feed_settings_changed();
    return true;
}

extern "C" void fw_explore_step(void){
    ensure_lock();
    if (!s_lock) return;
    uint32_t now = now_ms();
    double lat, lon;
    int dist;
    bool run = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending) {
        lat = s_req_lat;
        lon = s_req_lon;
        dist = s_req_dist;
        run = true;
    } else if (s_ac_json && now - s_buf_at_ms >= CacheTtlMs) {
        std::free(s_ac_json);
        s_ac_json = nullptr;
        s_count = 0;
    }
    xSemaphoreGive(s_lock);
    if (!run) return;

    char *buf = static_cast<char *>(std::malloc(BufCap));
    if (!buf) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pending = false;
        s_ok = false;
        s_status = 503;
        xSemaphoreGive(s_lock);
        return;
    }
    buf[0] = 0;
    char path[72];
    std::snprintf(path, sizeof path, "/api/v2/lat/%.4f/lon/%.4f/dist/%d", lat, lon, dist);
    unsigned status = 0;
    uint32_t retry = 0;
    Scan scan;
    scan.out = buf;
    scan.cap = BufCap;
    scan.len = 0;
    int stage = fw_https_get("opendata.adsb.fi", path, nullptr,
                             reinterpret_cast<const uint8_t *>(owner_tls_gts_r4), sizeof owner_tls_gts_r4,
                             &status, &retry, [&](flightwall::HttpBody &body){
        int c;
        while ((c = body.read()) >= 0) if (status == 200) scan.feed(c);
        return c == -1;
    });
    bool ok = !stage && status == 200;
    time_t unix_now = std::time(nullptr);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::free(s_ac_json);
    s_ac_json = ok ? buf : nullptr;
    if (!ok) std::free(buf);
    s_pending = false;
    s_ok = ok;
    s_status = status;
    s_count = ok ? scan.count : 0;
    s_at_unix = unix_now > 1700000000 ? (uint32_t)unix_now : 0;
    s_buf_at_ms = now_ms();
    xSemaphoreGive(s_lock);
    {
        char extra[96];
        std::snprintf(extra, sizeof extra, "\"ok\":%s,\"stage\":%d,\"status\":%u,\"count\":%d",
                      ok ? "true" : "false", stage, status, scan.count);
        fw_mem_report("explore_fetch", extra);
    }
}

extern "C" size_t fw_explore_json(char *buf, size_t cap){
    ensure_lock();
    if (!s_lock) return (size_t)std::snprintf(buf, cap, "{\"pending\":false,\"ok\":false,\"ac\":[]}");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = (size_t)std::snprintf(buf, cap,
                                     "{\"pending\":%s,\"ok\":%s,\"status\":%u,\"at\":%u,"
                                     "\"lat\":%.4f,\"lon\":%.4f,\"dist\":%d,\"count\":%d,\"ac\":[%s]}",
                                     s_pending ? "true" : "false", s_ok ? "true" : "false",
                                     s_status, (unsigned)s_at_unix, s_req_lat, s_req_lon, s_req_dist,
                                     s_count, s_ac_json ? s_ac_json : "");
    xSemaphoreGive(s_lock);
    return n;
}
