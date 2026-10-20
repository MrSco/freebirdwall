/* The community flight source; see community.h. */
#include "community.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include "community_cache.h"
#include "stream_adsb.h"
#include "http_body.h"
#include "https_get.h"
#include "esp32_transport.h"
#include "tls_port.h"
#include "fw_assets.h"
#include "fw_community_frame.h"
#include "fw_aero.h"
#include "aero_key.h"
#include "aero_root.h"     /* SSL.com RSA root, for FlightAware */
#include "big_buffer.h"
#include "stats.h"
#include "games_server.h"
#include "sync_store.h"
#include "fw_schedule.h"
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "memory_probe.h"
#include "owner_route_root.h"   /* ISRG Root X2 (Let's Encrypt), pinned by SHA in its header */
#include "owner_tls_root.h"     /* GTS Root R4 (Google Trust Services), pinned by SHA in its header */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace flightwall;

extern "C" {
uint8_t fw_tls_context[2048] __attribute__((aligned(8)));
const size_t fw_tls_context_bytes_reserved = 2048;
}

namespace {

constexpr uint32_t RouteGapMs = 1500;      /* between route lookups */
constexpr int RoutesPerStep = 2;
constexpr size_t RouteBodyMax = 4096;

/* The owner's tracked flights on this source (FEED.md "Tracked flights on the
 * community source"): each looked up by callsign on adsb.fi. */
constexpr int TrackMax = 8;                 /* request_config.tracked_flights, as stock */
constexpr int TrackShown = 5;               /* records on the panel at once, as stock */
struct Target {
    char number[12];                        /* as the owner typed it: "AA1234" */
    char cs[12];                            /* the callsign looked for: "AAL1234" */
    bool metrics, distance;                 /* show_metrics, show_distance_travelled */
    bool auto_remove, done;                 /* auto_remove: landed, then not looked for again (until restart) */
    bool coords;
    uint8_t route_tries;
    double c[4];                            /* origin lat/lon, destination lat/lon */
    int8_t slot;                            /* its record in State::shown, -1 none */
    bool was_ground;                        /* last seen on the ground */
    uint32_t next_ms, seen_ms, landed_ms, rest_until_ms;
    uint32_t dep_unix;                      /* when it took off: seen, FlightAware's, or estimated */
    uint8_t adsb_misses;                    /* adsb.fi lookups in a row without it */
    bool has_fa;                            /* FlightAware times known (fa) */
    FwAeroFlight fa;
    uint32_t sched_ms, pos_ms;              /* next FlightAware times / position request */
};

/* Everything the source keeps, allocated the first time it is chosen (about
 * 25 KiB), so a wall on the vendor feed pays nothing for it. */
struct State {
    CommunityConfig config;
    CommunityCache cache;
    Batch staging, show;
    PollBudget positions;
    StaticJsonDocument<4096> doc;
    char row[4096];
    /* Both pinned roots for both hosts, so either service moving between
     * these CAs keeps working. One PEM buffer, one terminating NUL. */
    char roots[sizeof owner_tls_gts_r4 + sizeof owner_route_root - 1];
    size_t next_zone;
    uint32_t last_route;
    bool good;
    uint32_t polls, fails, routes, routes_found;
    /* tracked flights: allocated only while there are any (about 2.5 + 2.8 KiB) */
    Target *targets;
    int target_count;
    int probe_next;
    uint32_t next_probe_ms;
    FwFlight *shown;                        /* TrackShown records, then one to work in */
    /* the owner's FlightAware AeroAPI key (tracking_source "aeroapi") */
    bool aero_wanted, aero_rejected;
    char aero_key[FW_AERO_KEY_MAX + 1];
    uint32_t aero_key_serial;
    unsigned aero_flights_calls, aero_position_calls, aero_last_status;
    /* the Sync leg in the air, tracked live by callsign */
    struct {
        char cs[12];
        uint32_t next_pos, next_route;
        int route_tries;
        bool coords;
        double olat, olon, dlat, dlon;
    } track;
};
State *s;

/* What the display reads: the live record for one callsign. */
SemaphoreHandle_t s_live_lock;
char s_live_cs[12];
bool s_live_found;
uint32_t s_live_at;
FwFlight s_live_flight;

uint32_t now_ms(){ return (uint32_t)(esp_timer_get_time() / 1000); }

bool asset_exists(void *, const char *path){ return fw_assets_exists(path); }
const LogoCatalog s_logos{nullptr, asset_exists, nullptr, 0};

double rank(void *, const Flight &f){
    double best = 1e100;
    for (size_t z = 0; z < s->config.count; ++z) { double r = zone_rank(s->config.zones[z], f); if (r < best) best = r; }
    return best;
}
/* Every aircraft that passes the owner's filters counts for the stats, not
 * just the eight kept. */
bool allow(void *, const Flight &f){
    if (!community_candidate(s->config, f, s_logos)) return false;
    fw_stats_flight(f.aircraft_id, f.callsign, f.registration, f.airline.icao);
    return true;
}

int body_byte(void *p){ return static_cast<HttpBody *>(p)->read(); }

/* ArduinoJson reads a route answer straight off the connection, bounded. */
struct RouteStream {
    HttpBody *body;
    size_t n;
    bool over;
    size_t max = RouteBodyMax;
    int read(){
        if (n >= max) { over = true; return -1; }
        int c = body->read();
        if (c < 0) return -1;
        ++n;
        return c;
    }
    size_t readBytes(char *out, size_t len){
        size_t i = 0;
        for (int c; i < len && (c = read()) >= 0; ++i) out[i] = (char)c;
        return i;
    }
};

/* GET https://host/path on both pinned roots (https_get.h). */
template <class Consume>
int https_get(const char *host, const char *path, unsigned *status, uint32_t *retry_after, Consume consume){
    return fw_https_get(host, path, nullptr, reinterpret_cast<const uint8_t *>(s->roots), sizeof s->roots, status,
                        retry_after, consume);
}

void poll_positions(uint32_t now){
    if (!s->positions.reserve(now)) return;
    size_t zone = s->next_zone++ % s->config.count;
    Request rq;
    if (!make_request(Provider::AdsbFi, s->config.zones[zone].zone, rq)) { s->positions.complete(now, 400); return; }
    ParseOptions opts;
    opts.allow = allow;
    opts.rank = rank;
    Result parsed = Result::Invalid;
    unsigned status;
    uint32_t retry, t0 = now_ms();
    fw_stats_begin();
    int stage = https_get(rq.host, rq.path, &status, &retry, [&](HttpBody &body){
        if (body.status() != 200) { while (body.read() >= 0) {} return true; }
        parsed = decode_adsb_stream({&body, body_byte}, s->doc, s->row, sizeof s->row, s->staging, opts);
        return parsed == Result::Ok;
    });
    s->doc.clear();
    uint32_t done = now_ms();
    bool ok = !stage && status == 200 && parsed == Result::Ok;
    s->positions.complete(done, stage ? 0 : status, retry);
    ++s->polls;
    if (ok) { s->cache.replace_zone(zone, s->staging, s->config, done); s->good = true; fw_stats_covered(); }
    else ++s->fails;
    if (!ok || s->polls <= 3 || s->polls % 30 == 0) {
        char extra[200];
        std::snprintf(extra, sizeof extra,
                      "\"n\":%u,\"zone\":%u,\"stage\":%d,\"status\":%u,\"result\":%d,\"kept\":%u,\"rejected\":%u,"
                      "\"overflow\":%s,\"cached\":%u,\"ms\":%u,\"fails\":%u",
                      (unsigned)s->polls, (unsigned)zone, stage, status, (int)parsed, (unsigned)s->staging.count,
                      (unsigned)s->staging.rejected, s->staging.overflow ? "true" : "false",
                      (unsigned)s->cache.batch.count, (unsigned)(done - t0), (unsigned)s->fails);
        fw_mem_report("community_poll", extra);
    }
}

void lookup_route(Flight &f){
    char path[48];
    int n = std::snprintf(path, sizeof path, "/v0/callsign/%s", f.callsign);
    if (n <= 0 || (size_t)n >= sizeof path) return;
    unsigned status;
    uint32_t retry;
    bool found = false;
    int stage = https_get("api.adsbdb.com", path, &status, &retry, [&](HttpBody &body){
        if (body.status() != 200) { while (body.read() >= 0) {} return true; }
        RouteStream in{&body, 0, false};
        s->doc.clear();
        bool parsed = !deserializeJson(s->doc, in, DeserializationOption::NestingLimit(12)) && !in.over &&
                      body.read() == -1;                      /* the whole answer, nothing after it */
        if (parsed && enrich_route(s->doc.as<JsonVariantConst>(), f.callsign, f)) {
            s->cache.route_result(f);
            found = true;
        }
        s->doc.clear();
        return parsed;
    });
    ++s->routes;
    if (found) ++s->routes_found;
    if (stage || (status != 200 && status != 404) || s->routes <= 3 || s->routes % 30 == 0) {
        char extra[128];
        std::snprintf(extra, sizeof extra, "\"n\":%u,\"found\":%u,\"stage\":%d,\"status\":%u,\"hit\":%s",
                      (unsigned)s->routes, (unsigned)s->routes_found, stage, status, found ? "true" : "false");
        fw_mem_report("community_route", extra);
    }
}

/* The airports' coordinates for a callsign (adsbdb): origin lat/lon, then
 * destination lat/lon, for progress, distance and the time left; with names,
 * also their codes and names. */
bool route_coords(const char *cs, double c[4], FwAeroFlight *names){
    char path[48];
    unsigned status;
    uint32_t retry;
    bool ok = false;
    std::snprintf(path, sizeof path, "/v0/callsign/%s", cs);
    https_get("api.adsbdb.com", path, &status, &retry, [&](HttpBody &body){
        if (body.status() != 200) { while (body.read() >= 0) {} return true; }
        RouteStream in{&body, 0, false};
        s->doc.clear();
        if (!deserializeJson(s->doc, in, DeserializationOption::NestingLimit(12)) && !in.over) {
            JsonVariantConst r = s->doc["response"]["flightroute"];
            if (r["origin"]["latitude"].is<double>() && r["origin"]["longitude"].is<double>() &&
                r["destination"]["latitude"].is<double>() && r["destination"]["longitude"].is<double>()) {
                c[0] = r["origin"]["latitude"]; c[1] = r["origin"]["longitude"];
                c[2] = r["destination"]["latitude"]; c[3] = r["destination"]["longitude"];
                ok = true;
                if (names) {
                    auto put = [](char *dst, size_t cap, JsonVariantConst v){
                        std::snprintf(dst, cap, "%s", v.is<const char *>() ? v.as<const char *>() : "");
                    };
                    put(names->orig_iata, sizeof names->orig_iata, r["origin"]["iata_code"]);
                    put(names->orig_icao, sizeof names->orig_icao, r["origin"]["icao_code"]);
                    put(names->orig, sizeof names->orig, r["origin"]["name"]);
                    put(names->dest_iata, sizeof names->dest_iata, r["destination"]["iata_code"]);
                    put(names->dest_icao, sizeof names->dest_icao, r["destination"]["icao_code"]);
                    put(names->dest, sizeof names->dest, r["destination"]["name"]);
                }
            }
        }
        s->doc.clear();
        return true;
    });
    return ok;
}

/* One aircraft by callsign on adsb.fi (/api/v2/callsign): true with its
 * record in s->staging.flights[0]; *stage and *status for the log. */
bool callsign_fix(const char *cs, int *stage, unsigned *status){
    char path[48];
    uint32_t retry;
    Result parsed = Result::Invalid;
    ParseOptions opts;
    std::snprintf(path, sizeof path, "/api/v2/callsign/%s", cs);
    *stage = https_get("opendata.adsb.fi", path, status, &retry, [&](HttpBody &body){
        if (body.status() != 200) { while (body.read() >= 0) {} return true; }
        parsed = decode_adsb_stream({&body, body_byte}, s->doc, s->row, sizeof s->row, s->staging, opts);
        return parsed == Result::Ok;
    });
    s->doc.clear();
    return !*stage && *status == 200 && parsed == Result::Ok && s->staging.count > 0;
}

/* While a Sync leg is in its window (from 30 min before departure until 30
 * min after arrival), look its callsign up on adsb.fi once a minute, and the
 * airports' coordinates once (adsbdb), for progress, distance and the time
 * left. Positions change little in a minute; no endpoint is hammered.
 * Counts only in the log. */
constexpr uint32_t LiveEveryMs = 60000, LiveRouteEveryMs = 60000;
constexpr uint32_t LiveValidMs = 150000;    /* a sighting stays on the panel this long */

void live_forget(){
    xSemaphoreTake(s_live_lock, portMAX_DELAY);
    s_live_found = false;
    s_live_cs[0] = 0;
    xSemaphoreGive(s_live_lock);
    s->track.cs[0] = 0;
}

void live_step(uint32_t now){
    uint32_t t = (uint32_t)std::time(nullptr);
    static FwSyncLeg leg;                                       /* this task only */
    const char *icao = fw_sync_store_tracking(t, &leg) ? fw_sched_icao_airline(leg.al) : nullptr;
    if (!icao) {
        if (s->track.cs[0]) live_forget();
        return;
    }
    char cs[12];
    std::snprintf(cs, sizeof cs, "%s%s", icao, leg.fn);
    if (std::strcmp(cs, s->track.cs)) {                       /* a new leg: start over */
        live_forget();
        std::memset(&s->track, 0, sizeof s->track);
        std::snprintf(s->track.cs, sizeof s->track.cs, "%s", cs);
    }
    if (!s->track.coords && s->track.route_tries < 3 && (int32_t)(now - s->track.next_route) >= 0) {
        double c[4];
        s->track.next_route = now + LiveRouteEveryMs;
        ++s->track.route_tries;
        if (route_coords(cs, c, nullptr)) {
            s->track.olat = c[0]; s->track.olon = c[1]; s->track.dlat = c[2]; s->track.dlon = c[3];
            s->track.coords = true;
        }
    }
    if ((int32_t)(now - s->track.next_pos) < 0) return;
    s->track.next_pos = now + LiveEveryMs;
    int stage;
    unsigned status;
    bool found = callsign_fix(cs, &stage, &status);
    if (found) {
        static FwFlight f;                                      /* this task only */
        const Flight &fl = s->staging.flights[0];
        fw_community_flight(fl, s_logos, &f);
        fw_community_track(&f, fl.latitude, fl.longitude, s->track.coords, s->track.olat, s->track.olon,
                           s->track.dlat, s->track.dlon, leg.dep_at, leg.arr_at, (uint32_t)std::time(nullptr));
        xSemaphoreTake(s_live_lock, portMAX_DELAY);
        s_live_flight = f;
        std::snprintf(s_live_cs, sizeof s_live_cs, "%s", cs);
        s_live_found = true;
        s_live_at = now;
        xSemaphoreGive(s_live_lock);
    }
    static unsigned lookups;
    if (++lookups <= 3 || lookups % 40 == 0 || stage) {
        char extra[120];
        std::snprintf(extra, sizeof extra, "\"n\":%u,\"stage\":%d,\"status\":%u,\"found\":%s,\"coords\":%s",
                      lookups, stage, status, found ? "true" : "false", s->track.coords ? "true" : "false");
        fw_mem_report("sync_live", extra);
    }
}


/* ------------------------------------------------------------ tracked flights
 * The owner's tracked flights on this source (FEED.md "Tracked flights on the
 * community source"). Positions always come from adsb.fi, looked up by
 * callsign once a minute while a flight is in the air.
 *
 * Without times (tracking_source "community"): a flight not yet in the air is
 * looked for in turn, one lookup a minute for all of them together; take-off
 * is what the wall saw, or an estimate (distance flown / speed), and arrival
 * is distance left / speed.
 *
 * With the owner's FlightAware AeroAPI key (tracking_source "aeroapi"): the
 * times and status come from /flights/{ident}, asked rarely (about every 12 h
 * with nothing near, 2 h before departure, every 30-60 min around the flight),
 * and adsb.fi is asked only from 30 min before take-off. FlightAware's own
 * position (/flights/{id}/position) only when adsb.fi cannot see a flight that
 * FlightAware says is in the air, every 5 minutes. */
constexpr uint32_t TrackEveryMs = 60000;
constexpr uint32_t TrackLandedMs = 10 * 60000;     /* shown this long after landing */
constexpr uint32_t TrackRestMs = 60 * 60000;       /* then not looked for again for an hour */
constexpr uint32_t AeroRetryMs = 15 * 60000;       /* a failed FlightAware request */
constexpr uint32_t AeroPositionMs = 5 * 60000;
constexpr uint32_t Minute = 60000, Hour = 60 * Minute;
constexpr const char *AeroHost = "aeroapi.flightaware.com";

bool aero_on(){ return s->aero_wanted && s->aero_key[0] && !s->aero_rejected; }

/* ms from now until unix time t (clamped to [lo, hi]). */
uint32_t until(uint32_t t, uint32_t unix_now, uint32_t lo, uint32_t hi){
    int64_t ms = ((int64_t)t - (int64_t)unix_now) * 1000;
    if (ms < lo) ms = lo;
    if (ms > hi) ms = hi;
    return (uint32_t)ms;
}

int free_slot(){
    for (int slot = 0; slot < TrackShown; ++slot) {
        bool used = false;
        for (int i = 0; i < s->target_count; ++i) if (s->targets[i].slot == slot) used = true;
        if (!used) return slot;
    }
    return -1;
}

void track_release(Target &t){
    t.slot = -1;
    t.dep_unix = 0;
    t.landed_ms = 0;
    t.was_ground = false;
}

/* The route and aircraft the wall knows (FlightAware, else adsbdb) onto a
 * record, where the record has none. */
void track_names(const Target &t, FwFlight &f){
    const FwAeroFlight &a = t.fa;
    auto fill = [](char *dst, size_t cap, const char *src){ if (!dst[0] && src[0]) std::snprintf(dst, cap, "%s", src); };
    fill(f.orig_iata, sizeof f.orig_iata, a.orig_iata);
    fill(f.orig_icao, sizeof f.orig_icao, a.orig_icao);
    fill(f.dest_iata, sizeof f.dest_iata, a.dest_iata);
    fill(f.dest_icao, sizeof f.dest_icao, a.dest_icao);
    if (!f.orig[0] && a.orig[0]) fw_community_airport_name(a.orig, f.orig, sizeof f.orig);
    if (!f.dest[0] && a.dest[0]) fw_community_airport_name(a.dest, f.dest, sizeof f.dest);
    fill(f.ac_type, sizeof f.ac_type, a.ac_type);
    fill(f.reg, sizeof f.reg, a.reg);
}

/* Tracked fields from what is known: FlightAware's times when there are
 * any, else take-off seen or estimated. */
void track_fill(Target &t, FwFlight &f, double lat, double lon, uint32_t unix_now){
    uint32_t arr = t.has_fa && !t.fa.landed ? t.fa.arr : 0;
    if (t.has_fa && (t.fa.airborne || t.fa.landed) && t.fa.dep) t.dep_unix = t.fa.dep;
    track_names(t, f);
    fw_community_track(&f, lat, lon, t.coords, t.c[0], t.c[1], t.c[2], t.c[3], t.dep_unix, arr, unix_now);
    if (arr > unix_now && !f.gnd) {                        /* FlightAware's estimate beats distance / speed */
        char rel[24];
        fw_sched_rel(arr, unix_now, rel, sizeof rel);
        std::snprintf(f.arr, sizeof f.arr, "%s", std::strncmp(rel, "in ", 3) ? rel : rel + 3);
    }
    if (!(f.prog == f.prog) && t.has_fa && t.fa.progress == t.fa.progress) f.prog = t.fa.progress;
    f.trk_metrics = t.metrics;
    if (!t.distance) f.dist = NAN;
}

void track_show(Target &t, const FwFlight &f, uint32_t now){
    if (t.slot < 0) t.slot = (int8_t)free_slot();
    if (t.slot >= 0) s->shown[t.slot] = f;
    t.seen_ms = now;
    if (f.prog >= 100 && t.dep_unix && !t.landed_ms) t.landed_ms = now;
}

void track_log(const char *stage_name, int stage, unsigned status, bool found){
    static unsigned n;
    if (++n > 3 && n % 60 && !stage && (status == 200 || status == 0)) return;
    char extra[160];
    int shown = 0;
    for (int i = 0; i < s->target_count; ++i) shown += s->targets[i].slot >= 0;
    std::snprintf(extra, sizeof extra,
                  "\"n\":%u,\"what\":\"%s\",\"stage\":%d,\"status\":%u,\"found\":%s,\"in_air\":%d,\"tracked\":%d,"
                  "\"aero_calls\":%u",
                  n, stage_name, stage, status, found ? "true" : "false", shown, s->target_count,
                  s->aero_flights_calls + s->aero_position_calls);
    fw_mem_report("tracked_lookup", extra);
}

void track_lookup(Target &t, uint32_t now){
    FwFlight &f = s->shown[TrackShown];                         /* the work record */
    int stage;
    unsigned status;
    t.next_ms = now + TrackEveryMs;
    bool found = callsign_fix(t.cs, &stage, &status);
    uint32_t unix_now = (uint32_t)std::time(nullptr);
    if (found) t.adsb_misses = 0;
    else if (t.adsb_misses < 255) ++t.adsb_misses;
    if (found) {
        const Flight &fl = s->staging.flights[0];
        double lat = fl.latitude, lon = fl.longitude;
        fw_community_flight(fl, s_logos, &f);
        if (!t.coords && t.route_tries < 3) { ++t.route_tries; t.coords = route_coords(t.cs, t.c, t.has_fa ? nullptr : &t.fa); }
        if (f.gnd) {
            if (!t.dep_unix) t.was_ground = true;               /* before take-off */
        } else if (!t.dep_unix) {
            if (t.was_ground) t.dep_unix = unix_now;            /* seen taking off */
            else if (t.coords && f.vel == f.vel && f.vel > 30)  /* in the air when first seen: estimated */
                t.dep_unix = unix_now - (uint32_t)(fw_community_distance(t.c[0], t.c[1], lat, lon) / f.vel);
            t.was_ground = false;
        }
        track_fill(t, f, lat, lon, unix_now);
        track_show(t, f, now);
    } else if (t.slot >= 0 && now - t.seen_ms >= LiveValidMs && !(t.has_fa && t.fa.airborne)) {
        track_release(t);                                       /* out of sight */
    }
    if (t.landed_ms && now - t.landed_ms >= TrackLandedMs) {
        track_release(t);
        t.rest_until_ms = now + TrackRestMs;
        t.done = t.auto_remove;             /* the settings are not written: it stays listed */
        if (t.has_fa) t.sched_ms = now + 6 * Hour;
    }
    track_log("adsb", stage, status, found);
}

/* GET https://aeroapi.flightaware.com/aeroapi<path> with the owner's key,
 * read through filter into s->doc. */
int aero_get(const char *path, JsonDocument &filter, unsigned *status, bool *parsed){
    char headers[96];
    uint32_t retry;
    *parsed = false;
    std::snprintf(headers, sizeof headers, "x-apikey: %s\r\n", s->aero_key);
    s->doc.clear();
    int stage = fw_https_get(AeroHost, path, headers, reinterpret_cast<const uint8_t *>(owner_aero_root),
                             sizeof owner_aero_root, status, &retry, [&](HttpBody &body){
        if (body.status() != 200) { while (body.read() >= 0) {} return true; }
        RouteStream in{&body, 0, false, 64 * 1024};
        *parsed = !deserializeJson(s->doc, in, DeserializationOption::Filter(filter),
                                   DeserializationOption::NestingLimit(12)) && !in.over;
        while (body.read() >= 0) {}
        return true;
    });
    std::memset(headers, 0, sizeof headers);
    s->aero_last_status = *status;
    if (*status == 401 || *status == 403) s->aero_rejected = true;   /* a wrong key: no more requests until it changes */
    return stage;
}

void aero_flights(Target &t, uint32_t now){
    DynamicJsonDocument filter(768);
    fw_aero_flights_filter(filter);
    char path[128], from[24], to[24];
    time_t unix_now = std::time(nullptr), a = unix_now - 20 * 3600, b = unix_now + 36 * 3600;
    struct tm tm;
    std::strftime(from, sizeof from, "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&a, &tm));
    std::strftime(to, sizeof to, "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&b, &tm));
    std::snprintf(path, sizeof path, "/aeroapi/flights/%s?ident_type=designator&start=%s&end=%s&max_pages=1", t.cs,
                  from, to);
    unsigned status;
    bool parsed;
    ++s->aero_flights_calls;
    int stage = aero_get(path, filter, &status, &parsed);
    FwAeroFlight fa;
    bool picked = parsed && fw_aero_pick(s->doc.as<JsonVariantConst>(), (uint32_t)unix_now, &fa);
    s->doc.clear();
    if (stage || status != 200 || !parsed) {
        t.sched_ms = now + AeroRetryMs;
    } else if (!picked) {
        t.has_fa = false;
        t.sched_ms = now + 12 * Hour;                           /* nothing in the next day and a half */
    } else {
        t.fa = fa;
        t.has_fa = true;
        if (fa.airborne) t.sched_ms = now + Hour;
        else if (fa.landed) t.sched_ms = now + 6 * Hour;
        else t.sched_ms = now + (fa.dep > (uint32_t)unix_now + 4 * 3600 ? until(fa.dep - 2 * 3600, (uint32_t)unix_now, 30 * Minute, 12 * Hour)
                                                                     : 30 * Minute);
        if (fa.airborne && t.slot < 0) t.next_ms = now;         /* look for it on adsb.fi now */
    }
    track_log("aero_flights", stage, status, picked);
}

/* FlightAware's own position, for a flight in the air adsb.fi cannot see. */
void aero_position(Target &t, uint32_t now){
    DynamicJsonDocument filter(256);
    FwFlight &f = s->shown[TrackShown];                         /* the work record */
    fw_aero_position_filter(filter);
    char path[112];
    std::snprintf(path, sizeof path, "/aeroapi/flights/%s/position", t.fa.fa_id);
    unsigned status;
    bool parsed;
    t.pos_ms = now + AeroPositionMs;
    ++s->aero_position_calls;
    int stage = aero_get(path, filter, &status, &parsed);
    double lat, lon, alt, gs, hd;
    bool found = parsed && fw_aero_position(s->doc.as<JsonVariantConst>(), &lat, &lon, &alt, &gs, &hd);
    s->doc.clear();
    if (found) {
        fw_flight_init(&f);
        std::snprintf(f.cs, sizeof f.cs, "%s", t.cs);
        std::snprintf(f.al_code, sizeof f.al_code, "%.3s", t.cs);
        f.alt_b = alt;
        f.vel = gs;
        f.trk = hd;
        f.gnd = false;
        if (!t.coords && t.route_tries < 3) { ++t.route_tries; t.coords = route_coords(t.cs, t.c, nullptr); }
        track_fill(t, f, lat, lon, (uint32_t)std::time(nullptr));
        track_show(t, f, now);
    }
    track_log("aero_position", stage, status, found);
}

/* Worth looking for on adsb.fi now. */
bool track_window(const Target &t, uint32_t unix_now){
    if (t.done) return false;
    if (!aero_on() || !t.has_fa) return true;                   /* no times: always */
    if (t.fa.airborne) return true;
    if (t.fa.landed) return false;
    return t.fa.dep && unix_now + 1800 >= t.fa.dep;
}

/* At most one request per call: a flight in the air that is due; then
 * FlightAware times that are due; then FlightAware's position for a flight
 * adsb.fi has lost; then the next flight waiting, once a minute. */
void track_step(uint32_t now){
    if (!s->target_count) return;
    if (s->aero_wanted && fw_aero_key_serial() != s->aero_key_serial) {
        s->aero_key_serial = fw_aero_key_serial();
        fw_aero_key_load(s->aero_key, sizeof s->aero_key);
        s->aero_rejected = false;
        for (int i = 0; i < s->target_count; ++i) s->targets[i].sched_ms = now;
    }
    uint32_t unix_now = (uint32_t)std::time(nullptr);
    for (int i = 0; i < s->target_count; ++i) {
        Target &t = s->targets[i];
        if (t.slot >= 0 && (int32_t)(now - t.next_ms) >= 0) { track_lookup(t, now); return; }
    }
    if (aero_on()) {
        for (int i = 0; i < s->target_count; ++i) {
            Target &t = s->targets[i];
            if (!t.done && (int32_t)(now - t.sched_ms) >= 0) { aero_flights(t, now); return; }
        }
        for (int i = 0; i < s->target_count; ++i) {
            Target &t = s->targets[i];
            bool lost = t.slot < 0 || now - t.seen_ms >= 3 * Minute;
            if (t.has_fa && t.fa.airborne && t.fa.fa_id[0] && lost && t.adsb_misses >= 3 &&
                (int32_t)(now - t.pos_ms) >= 0) {
                aero_position(t, now);
                return;
            }
        }
    }
    if ((int32_t)(now - s->next_probe_ms) < 0) return;
    s->next_probe_ms = now + TrackEveryMs;
    for (int k = 0; k < s->target_count; ++k) {
        int i = (s->probe_next + k) % s->target_count;
        Target &t = s->targets[i];
        if (t.slot >= 0 || !track_window(t, unix_now) || (t.rest_until_ms && (int32_t)(now - t.rest_until_ms) < 0)) continue;
        t.rest_until_ms = 0;
        s->probe_next = (i + 1) % s->target_count;
        track_lookup(t, now);
        return;
    }
}

/* With any tracked flight in sight the panel shows those, as the vendor feed
 * does with its tracked answer; otherwise the nearby flights. */
void track_publish(FwFeedFrame *frame, uint32_t now){
    int n = 0;
    for (int i = 0; i < s->target_count && n < TrackShown; ++i) {
        const Target &t = s->targets[i];
        bool fresh = now - t.seen_ms < LiveValidMs || (t.has_fa && t.fa.airborne && now - t.seen_ms < AeroPositionMs + Minute);
        if (t.slot >= 0 && fresh) frame->flights[n++] = s->shown[t.slot];
    }
    if (!n) return;
    frame->count = n;
    frame->tracked = true;
    frame->overflow = 0;
}

/* The tracked flights from request_config (as the request body reads them:
 * flight_number, show_metrics, show_distance_travelled, auto_remove); what is
 * known about flights tracked before stays. */
void track_configure(JsonVariantConst list){
    Target *old = s->targets;                                   /* what was known */
    int old_count = s->target_count;
    size_t wanted = list.is<JsonArrayConst>() ? list.size() : 0;
    if (wanted > (size_t)TrackMax) wanted = TrackMax;
    s->targets = wanted ? new (std::nothrow) Target[wanted] : nullptr;
    s->target_count = 0;
    if (s->targets && !s->shown) s->shown = new (std::nothrow) FwFlight[TrackShown + 1];
    if (!s->shown) { delete[] s->targets; s->targets = nullptr; }   /* no room: none tracked */
    if (s->targets) {
        for (JsonVariantConst e : list.as<JsonArrayConst>()) {
            const char *number = e["flight_number"].is<const char *>() ? e["flight_number"].as<const char *>() : nullptr;
            if (!number || !*number || std::strlen(number) > 11 || (size_t)s->target_count == wanted) continue;
            Target t{};
            t.slot = -1;
            for (int i = 0; i < old_count; ++i) if (!std::strcmp(old[i].number, number)) t = old[i];
            std::snprintf(t.number, sizeof t.number, "%s", number);
            fw_sched_callsign(number, t.cs, sizeof t.cs);
            t.metrics = e["show_metrics"].is<bool>() && e["show_metrics"].as<bool>();
            t.distance = e["show_distance_travelled"].is<bool>() && e["show_distance_travelled"].as<bool>();
            t.auto_remove = !e["auto_remove"].is<bool>() || e["auto_remove"].as<bool>();   /* default true, as stock */
            if (!t.auto_remove) t.done = false;
            s->targets[s->target_count++] = t;
        }
    }
    delete[] old;
    if (!s->targets) { delete[] s->shown; s->shown = nullptr; }
    s->probe_next = 0;
    s->next_probe_ms = now_ms();
}

} // namespace

extern "C" void fw_community_aero_status(FwAeroStatus *out){
    std::memset(out, 0, sizeof *out);
    if (!s) return;
    out->in_use = s->aero_wanted && s->aero_key[0];
    out->rejected = s->aero_rejected;
    out->flights_calls = s->aero_flights_calls;
    out->position_calls = s->aero_position_calls;
    out->last_status = s->aero_last_status;
}

extern "C" bool fw_community_live(const char *callsign, FwFlight *out){
    bool ok = false;
    if (!s_live_lock || !callsign) return false;
    xSemaphoreTake(s_live_lock, portMAX_DELAY);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_live_found && !std::strcmp(s_live_cs, callsign) && now - s_live_at < LiveValidMs) { *out = s_live_flight; ok = true; }
    xSemaphoreGive(s_live_lock);
    return ok;
}

extern "C" bool fw_community_configure(const char *settings_json, FwCommunityInfo *info){
    std::memset(info, 0, sizeof *info);
    if (!s_live_lock) s_live_lock = xSemaphoreCreateMutex();
    if (!s) {
        s = new (std::nothrow) State();
        if (!s) { fw_mem_report("community_no_memory", nullptr); return false; }
        std::memcpy(s->roots, owner_tls_gts_r4, sizeof owner_tls_gts_r4 - 1);
        std::memcpy(s->roots + sizeof owner_tls_gts_r4 - 1, owner_route_root, sizeof owner_route_root);
    }
    s->config = CommunityConfig{};
    s->cache.clear();
    reset(s->show, Provider::AdsbFi);
    s->positions = PollBudget{};
    s->next_zone = 0;
    s->good = false;
    bool ok = false;
    {
        /* Only request_config is kept from the (up to 4 KiB) document. */
        DynamicJsonDocument filter(96), doc(6144);
        filter["request_config"] = true;
        filter["owned_config"]["tracking_source"] = true;
        bool read = settings_json && !deserializeJson(doc, settings_json, DeserializationOption::Filter(filter),
                                                      DeserializationOption::NestingLimit(12));
        if (read) ok = community_config(doc["request_config"], s->config) == Result::Ok && s->config.count > 0;
        JsonVariantConst list;
        if (read) list = doc["request_config"]["tracked_flights"];
        track_configure(list);
        bool aero = read && doc["owned_config"]["tracking_source"] == "aeroapi";
        if (aero != s->aero_wanted) {
            s->aero_wanted = aero;
            s->aero_key_serial = fw_aero_key_serial() - 1;     /* (re)load the key on the next step */
            for (int i = 0; i < s->target_count; ++i) { s->targets[i].has_fa = false; s->targets[i].sched_ms = now_ms(); }
        }
        if (!aero) std::memset(s->aero_key, 0, sizeof s->aero_key);
    }
    if (!ok) s->config = CommunityConfig{};
    info->areas = (int)s->config.count;
    info->tracked = s->target_count;
    info->skipped_geo = (int)s->config.skipped_geo;
    return ok;
}

extern "C" uint32_t fw_community_step(FwFeedFrame *frame, bool *good){
    *good = s && s->good;
    if (!s || !s->config.count) return 10000;
    if (fw_games_active()) return 1000;                     /* the game has the block; flights are not shown */
    if (!fw_esp_wall_clock_valid()) return 2000;            /* certificates need the date */
    uint32_t now = now_ms();
    if (s->positions.ready(now)) poll_positions(now);
    for (int i = 0; i < RoutesPerStep; ++i) {
        Flight f;
        now = now_ms();
        if (s->last_route && (uint32_t)(now - s->last_route) < RouteGapMs) break;
        if (!s->cache.route_candidate(now, f)) break;
        s->last_route = now ? now : 1;
        lookup_route(f);
    }
    live_step(now_ms());                                    /* the Sync leg in the air, if any */
    track_step(now_ms());                                   /* the owner's tracked flights */
    s->cache.display(s->config, s_logos, now_ms(), s->show);
    fw_community_frame(s->show, s_logos, frame);
    track_publish(frame, now_ms());
    *good = s->good;
    now = now_ms();
    uint32_t wait = s->positions.next_ms() - now;
    if ((int32_t)wait < 500) wait = 500;
    if (wait > 1500 && s->cache.batch.count) wait = 1500;   /* routes still to look up keep it ticking */
    return wait;
}
