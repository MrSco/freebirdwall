/* The weather on the board; see weather.h and WEATHER.md. */
#include "weather.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "flight_data.h"        /* ArduinoJson */
#include "https_get.h"
#include "awc_root.h"           /* DigiCert Global Root G2, for aviationweather.gov */
#include "memory_probe.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

using namespace flightwall;

namespace {

constexpr const char *Host = "aviationweather.gov";
constexpr uint32_t Minute = 60000;
constexpr uint32_t EveryMs = 10 * Minute;           /* METARs come hourly, specials in between */
constexpr uint32_t RetryMs = 2 * Minute;
constexpr uint32_t VendorFreshMs = 30 * Minute;     /* the FlightWall feed's own weather wins this long */
constexpr size_t BodyMax = 64 * 1024;
constexpr int Misses = 3;                           /* empty answers before the station is chosen again */

SemaphoreHandle_t s_lock;
FwWeather s_w;
uint32_t s_serial;
enum Source : uint8_t { None, Feed, Awc } s_source;
uint32_t s_vendor_ms;                               /* when the feed last sent a valid record, 0 never */

/* the fetcher's state: feed task only */
uint32_t s_next_ms;
char s_station[6];                                  /* the station asked for */
char s_auto[6];                                     /* the nearest one found, for s_auto_lat/lon */
float s_auto_lat = NAN, s_auto_lon = NAN;
int s_misses;
unsigned s_status, s_requests, s_failures;

uint32_t now_ms(){ return (uint32_t)(esp_timer_get_time() / 1000); }

void publish(const FwWeather &w, Source src){
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (std::memcmp(&w, &s_w, sizeof w) || s_source != src) {
        s_w = w;
        s_source = src;
        ++s_serial;
    }
    xSemaphoreGive(s_lock);
}

/* The body, bounded, for ArduinoJson. */
struct Stream {
    HttpBody *body;
    size_t n = 0;
    bool over = false;
    int read(){
        if (n >= BodyMax) { over = true; return -1; }
        int c = body->read();
        if (c >= 0) ++n;
        return c;
    }
    size_t readBytes(char *out, size_t len){
        size_t i = 0;
        for (int c; i < len && (c = read()) >= 0; ++i) out[i] = (char)c;
        return i;
    }
};

/* GET /api/data/metar?<query>&format=json into doc, through filter. */
int metar_get(const char *query, JsonDocument &filter, JsonDocument &doc, bool *parsed){
    char path[112];
    uint32_t retry;
    *parsed = false;
    std::snprintf(path, sizeof path, "/api/data/metar?%s&format=json", query);
    ++s_requests;
    int stage = fw_https_get(Host, path, nullptr, reinterpret_cast<const uint8_t *>(owner_awc_root), sizeof owner_awc_root,
                             &s_status, &retry, [&](HttpBody &body){
        if (body.status() != 200) { while (body.read() >= 0) {} return true; }
        Stream in{&body};
        *parsed = !deserializeJson(doc, in, DeserializationOption::Filter(filter),
                                   DeserializationOption::NestingLimit(6)) && !in.over;
        while (body.read() >= 0) {}
        return true;
    });
    if (stage || s_status != 200 || !*parsed) ++s_failures;
    return stage;
}

double great_circle_km(double lat1, double lon1, double lat2, double lon2){
    const double r = M_PI / 180.0;
    double a = std::sin((lat2 - lat1) * r / 2), b = std::sin((lon2 - lon1) * r / 2);
    double h = a * a + std::cos(lat1 * r) * std::cos(lat2 * r) * b * b;
    return 2 * 6371.0 * std::asin(std::sqrt(h));
}

/* The station with a METAR nearest the first area: the stations reporting in
 * a box around it (about 65 km, then 220 km). */
bool nearest_station(float lat, float lon){
    DynamicJsonDocument filter(128), doc(4096);
    filter[0]["icaoId"] = true;
    filter[0]["lat"] = true;
    filter[0]["lon"] = true;
    for (double half : {0.6, 2.0}) {
        char query[96];
        bool parsed;
        double dlon = half / std::fmax(0.2, std::cos(lat * M_PI / 180.0));
        std::snprintf(query, sizeof query, "bbox=%.2f,%.2f,%.2f,%.2f", lat - half, lon - dlon, lat + half, lon + dlon);
        doc.clear();
        int stage = metar_get(query, filter, doc, &parsed);
        if (stage || s_status != 200 || !parsed) return false;
        double best = 1e9;
        for (JsonVariantConst m : doc.as<JsonArrayConst>()) {
            const char *id = m["icaoId"] | "";
            if (std::strlen(id) != 4 || !m["lat"].is<double>() || !m["lon"].is<double>()) continue;
            double d = great_circle_km(lat, lon, m["lat"].as<double>(), m["lon"].as<double>());
            if (d < best) { best = d; std::snprintf(s_auto, sizeof s_auto, "%s", id); }
        }
        if (best < 1e9) {
            s_auto_lat = lat;
            s_auto_lon = lon;
            return true;
        }
    }
    return false;
}

/* One station's latest METAR into the store. Returns false on a failed request. */
bool fetch(const char *station){
    DynamicJsonDocument filter(384), doc(2048);
    for (const char *k : {"icaoId", "name", "rawOb", "fltCat", "wxString", "obsTime", "temp", "wdir", "wspd", "wgst",
                          "visib", "lat", "lon"})
        filter[0][k] = true;
    filter[0]["clouds"][0]["cover"] = true;
    filter[0]["clouds"][0]["base"] = true;
    char query[32];
    bool parsed;
    std::snprintf(query, sizeof query, "ids=%s", station);
    int stage = metar_get(query, filter, doc, &parsed);
    if (stage || s_status != 200 || !parsed) return false;
    JsonVariantConst m = doc[0];
    if (m.isNull()) { ++s_misses; return true; }        /* no recent METAR from it */
    s_misses = 0;
    FwAwcMetar a;
    std::memset(&a, 0, sizeof a);
    auto num = [](JsonVariantConst v){ return v.is<double>() ? v.as<double>() : NAN; };
    a.icao = m["icaoId"] | "";
    a.name = m["name"] | "";
    a.raw = m["rawOb"] | "";
    a.flt_cat = m["fltCat"] | "";
    a.wx = m["wxString"] | "";
    a.obs = num(m["obsTime"]);
    a.temp = num(m["temp"]);
    a.wdir_vrb = m["wdir"].is<const char *>() && !std::strcmp(m["wdir"].as<const char *>(), "VRB");
    a.wdir = num(m["wdir"]);
    a.wspd = num(m["wspd"]);
    a.wgst = num(m["wgst"]);
    if (m["visib"].is<const char *>()) {                /* "10+", or a number as text */
        const char *v = m["visib"].as<const char *>();
        a.visib_plus = std::strchr(v, '+') != nullptr;
        a.visib = a.visib_plus ? NAN : std::atof(v);
    } else {
        a.visib = num(m["visib"]);
    }
    a.lat = num(m["lat"]);
    a.lon = num(m["lon"]);
    for (JsonVariantConst c : m["clouds"].as<JsonArrayConst>()) {
        if (a.ncloud == FW_AWC_CLOUDS) break;
        a.cover[a.ncloud] = c["cover"] | "";
        a.base[a.ncloud] = num(c["base"]);
        ++a.ncloud;
    }
    FwWeather w;
    fw_weather_from_awc(&a, &w);
    if (fw_weather_valid(&w)) publish(w, Awc);
    return true;
}

} // namespace

extern "C" void fw_weather_store_init(void){
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

/* The vendor's listener (FUN_4200eecc): flights listed -> keep; else the record or nothing. */
extern "C" void fw_weather_store_vendor(const FwWeather *w, bool has_flights){
    if (!s_lock || has_flights) return;
    if (fw_weather_valid(w)) {
        s_vendor_ms = now_ms() | 1;
        publish(*w, Feed);
    } else if (s_source == Feed) {
        FwWeather none;
        fw_weather_init(&none);
        publish(none, None);
    }
}

extern "C" uint32_t fw_weather_store_copy(FwWeather *out){
    uint32_t serial;
    if (!s_lock) { fw_weather_init(out); return 0; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_w;
    serial = s_serial;
    xSemaphoreGive(s_lock);
    return serial;
}

extern "C" void fw_weather_step(const FwWeatherWant *want){
    uint32_t now = now_ms();
    char station[6];
    if (!s_lock || !want->enabled) { s_next_ms = 0; return; }
    if (!want->community && s_vendor_ms && now - s_vendor_ms < VendorFreshMs) return;   /* the feed sends it */
    if (s_next_ms && (int32_t)(now - s_next_ms) < 0) return;
    if (std::time(nullptr) < 1700000000) return;                                     /* TLS needs the clock */
    if (want->station[0]) {
        std::snprintf(station, sizeof station, "%s", want->station);
    } else {
        if (!want->has_area) { s_next_ms = now + EveryMs; return; }
        if (!s_auto[0] || s_misses >= Misses || s_auto_lat != want->lat || s_auto_lon != want->lon) {
            s_auto[0] = 0;
            s_misses = 0;
            if (!nearest_station(want->lat, want->lon)) {
                s_next_ms = now + 30000u;                    /* no station yet: soon again */
                fw_mem_report("weather_station", nullptr);
                return;
            }
        }
        std::snprintf(station, sizeof station, "%s", s_auto);
    }
    std::snprintf(s_station, sizeof s_station, "%s", station);
    bool ok = fetch(station);
    FwWeather held;
    fw_weather_store_copy(&held);
    s_next_ms = now_ms() + (ok ? EveryMs : fw_weather_valid(&held) ? RetryMs : 30000u);   /* none yet: soon again */
    char extra[96];
    std::snprintf(extra, sizeof extra, "\"ok\":%s,\"status\":%u,\"requests\":%u,\"failures\":%u", ok ? "true" : "false",
                  s_status, s_requests, s_failures);
    fw_mem_report("weather_fetch", extra);                 /* never the station or the position */
}

extern "C" void fw_weather_status(char *source, int source_cap, char *station, int station_cap, unsigned *status,
                                  unsigned *requests, unsigned *failures){
    FwWeather w;
    Source src;
    if (!s_lock) { std::snprintf(source, source_cap, "%s", ""); std::snprintf(station, station_cap, "%s", ""); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    w = s_w;
    src = s_source;
    xSemaphoreGive(s_lock);
    std::snprintf(source, (size_t)source_cap, "%s", src == Feed ? "feed" : src == Awc ? "aviationweather" : "");
    std::snprintf(station, (size_t)station_cap, "%s", fw_weather_valid(&w) ? w.stn : s_station);
    *status = s_status;
    *requests = s_requests;
    *failures = s_failures;
}
