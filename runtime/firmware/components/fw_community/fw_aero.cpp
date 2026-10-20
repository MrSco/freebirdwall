/* FlightAware AeroAPI answers; see fw_aero.h. */
#include "fw_aero.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const char *const TIMES[] = {"scheduled_out", "scheduled_off", "estimated_off", "actual_off",
                             "scheduled_on", "scheduled_in", "estimated_on", "actual_on"};
const char *const AIRPORT[] = {"code_iata", "code_icao", "name"};

void copy(char *out, size_t cap, JsonVariantConst v){
    std::snprintf(out, cap, "%s", v.is<const char *>() ? v.as<const char *>() : "");
}

uint32_t t(JsonVariantConst f, const char *key){ return fw_aero_time(f[key].as<const char *>()); }

/* The best known take-off and landing. */
uint32_t take_off(JsonVariantConst f){
    uint32_t v = t(f, "actual_off");
    if (!v) v = t(f, "estimated_off");
    if (!v) v = t(f, "scheduled_off");
    if (!v) v = t(f, "scheduled_out");
    return v;
}
uint32_t landing(JsonVariantConst f){
    uint32_t v = t(f, "actual_on");
    if (!v) v = t(f, "estimated_on");
    if (!v) v = t(f, "scheduled_on");
    if (!v) v = t(f, "scheduled_in");
    return v;
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d){
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

} // namespace

uint32_t fw_aero_time(const char *s){
    int y, mo, d, h, mi, sec, n = 0;
    if (!s || std::sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) != 6 || n < 19) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return 0;
    const char *p = s + n;
    if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') ++p; }
    int64_t off = 0;
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if (std::sscanf(p + 1, "%2d:%2d", &oh, &om) < 1) return 0;
        off = (oh * 60 + om) * 60 * (*p == '-' ? -1 : 1);
    } else if (*p != 'Z' && *p) {
        return 0;
    }
    int64_t v = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + sec - off;
    return v > 0 && v < 0xffffffffLL ? (uint32_t)v : 0;
}

void fw_aero_flights_filter(JsonDocument &filter){
    JsonObject f = filter["flights"].createNestedObject();
    for (const char *k : TIMES) f[k] = true;
    for (const char *k : {"fa_flight_id", "cancelled", "progress_percent", "aircraft_type", "registration"}) f[k] = true;
    for (const char *side : {"origin", "destination"})
        for (const char *k : AIRPORT) f[side][k] = true;
}

void fw_aero_position_filter(JsonDocument &filter){
    for (const char *k : {"latitude", "longitude", "altitude", "groundspeed", "heading"}) filter["last_position"][k] = true;
}

bool fw_aero_pick(JsonVariantConst answer, uint32_t now, FwAeroFlight *out){
    JsonVariantConst air, landed, next;
    uint32_t air_dep = 0, landed_on = 0, next_dep = 0;
    for (JsonVariantConst f : answer["flights"].as<JsonArrayConst>()) {
        if (f["cancelled"].as<bool>()) continue;
        uint32_t off = t(f, "actual_off"), on = t(f, "actual_on"), dep = take_off(f);
        if (off && !on) {
            if (air.isNull() || off > air_dep) { air = f; air_dep = off; }
        } else if (on) {
            if (on <= now + 60 && now - on <= 1800 && (landed.isNull() || on > landed_on)) { landed = f; landed_on = on; }
        } else if (dep && (uint64_t)dep + 3 * 3600 >= now && (next.isNull() || dep < next_dep)) {
            next = f;
            next_dep = dep;
        }
    }
    JsonVariantConst f = !air.isNull() ? air : !landed.isNull() ? landed : next;
    std::memset(out, 0, sizeof *out);
    out->progress = NAN;
    if (f.isNull()) return false;
    out->airborne = !air.isNull();
    out->landed = air.isNull() && !landed.isNull();
    out->dep = take_off(f);
    out->arr = landing(f);
    if (f["progress_percent"].is<double>()) out->progress = f["progress_percent"].as<double>();
    copy(out->fa_id, sizeof out->fa_id, f["fa_flight_id"]);
    copy(out->orig_iata, sizeof out->orig_iata, f["origin"]["code_iata"]);
    copy(out->orig_icao, sizeof out->orig_icao, f["origin"]["code_icao"]);
    copy(out->orig, sizeof out->orig, f["origin"]["name"]);
    copy(out->dest_iata, sizeof out->dest_iata, f["destination"]["code_iata"]);
    copy(out->dest_icao, sizeof out->dest_icao, f["destination"]["code_icao"]);
    copy(out->dest, sizeof out->dest, f["destination"]["name"]);
    copy(out->ac_type, sizeof out->ac_type, f["aircraft_type"]);
    copy(out->reg, sizeof out->reg, f["registration"]);
    return true;
}

bool fw_aero_position(JsonVariantConst answer, double *lat, double *lon, double *alt_m, double *gs_mps,
                      double *heading){
    JsonVariantConst p = answer["last_position"];
    if (!p["latitude"].is<double>() || !p["longitude"].is<double>()) return false;
    *lat = p["latitude"].as<double>();
    *lon = p["longitude"].as<double>();
    *alt_m = p["altitude"].is<double>() ? p["altitude"].as<double>() * 100 * 0.3048 : NAN;   /* hundreds of feet */
    *gs_mps = p["groundspeed"].is<double>() ? p["groundspeed"].as<double>() * 0.514444 : NAN; /* knots */
    *heading = p["heading"].is<double>() ? p["heading"].as<double>() : NAN;
    return true;
}
