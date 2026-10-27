/* Community flights as the screens' flight list; see fw_community_frame.h. */
#include "fw_community_frame.h"
#include "fw_airports.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

void copy(char *out, size_t cap, const char *in){ std::snprintf(out, cap, "%s", in ? in : ""); }

double value(const flightwall::Metric &m){ return m.valid ? m.value : NAN; }

bool ends_with(const char *s, size_t n, const char *tail){
    size_t t = std::strlen(tail);
    return n >= t && std::strncmp(s + n - t, tail, t) == 0;
}

const char *aircraft_class(flightwall::Category c){
    switch (c) {
    case flightwall::Category::Helicopter: return "Heli";
    case flightwall::Category::Light: return "Light";
    case flightwall::Category::SmallJet: return "Smalljet";
    default: return "";
    }
}

} // namespace

void fw_community_airport_name(const char *in, char *out, size_t cap){
    char b[80];
    size_t n;
    if (!cap) return;
    copy(b, sizeof b, in);
    n = std::strlen(b);
    while (n && b[n - 1] == ' ') b[--n] = 0;
    if (ends_with(b, n, " International Airport")) { n -= std::strlen(" International Airport"); std::strcpy(b + n, " Intl"); }
    else if (ends_with(b, n, " Intl Airport")) { n -= std::strlen(" Airport"); b[n] = 0; }
    else if (ends_with(b, n, " Airport")) { n -= std::strlen(" Airport"); b[n] = 0; }
    copy(out, cap, b);
}

void fw_community_aircraft_name(const char *in, char *out, size_t cap){
    size_t o = 0;
    if (!cap) return;
    if (!in) in = "";
    while (*in && o + 1 < cap) {
        const char *w = in;
        size_t len = 0;
        bool letters = true;
        while (w[len] && w[len] != ' ') { if (w[len] < 'A' || w[len] > 'Z') letters = false; ++len; }
        for (size_t i = 0; i < len && o + 1 < cap; ++i) {
            char c = w[i];
            bool word = letters && (len > 3 || (len == 2 && w[0] == 'D' && w[1] == 'E'));   /* DE HAVILLAND */
            if (word && i > 0) c = (char)(c - 'A' + 'a');
            out[o++] = c;
        }
        in = w + len;
        while (*in == ' ' && o + 1 < cap) { out[o++] = ' '; ++in; }
    }
    out[o] = 0;
}

void fw_community_flight(const flightwall::Flight &s, const flightwall::LogoCatalog &logos, FwFlight *f){
    fw_flight_init(f);
    copy(f->icao, sizeof f->icao, s.aircraft_id);
    copy(f->cs, sizeof f->cs, s.callsign);
    copy(f->reg, sizeof f->reg, s.registration);
    f->alt_b = value(s.altitude);
    f->vel = value(s.speed);
    f->trk = value(s.heading);
    f->vr = value(s.vertical_rate);
    f->gnd = s.has_ground_state && s.on_ground;
    copy(f->orig_icao, sizeof f->orig_icao, s.origin.icao);
    copy(f->orig_iata, sizeof f->orig_iata, s.origin.iata);
    copy(f->dest_icao, sizeof f->dest_icao, s.destination.icao);
    copy(f->dest_iata, sizeof f->dest_iata, s.destination.iata);
    fw_community_airport_name(s.origin.name, f->orig, sizeof f->orig);
    fw_community_airport_name(s.destination.name, f->dest, sizeof f->dest);
    flightwall::Logo logo = flightwall::resolve_logo(s, logos);
    copy(f->al_code, sizeof f->al_code, logo.airline_code);
    copy(f->al_name, sizeof f->al_name, s.airline.name);
    copy(f->ac_type, sizeof f->ac_type, s.aircraft_type);
    fw_community_aircraft_name(s.aircraft_name, f->ac_name, sizeof f->ac_name);
    copy(f->ac_class, sizeof f->ac_class, aircraft_class(s.category));
}

void fw_community_frame(const flightwall::Batch &batch, const flightwall::LogoCatalog &logos, FwFeedFrame *out){
    std::memset(out, 0, sizeof *out);
    size_t n = batch.count < FW_FEED_MAX_FLIGHTS ? batch.count : FW_FEED_MAX_FLIGHTS;
    for (size_t i = 0; i < n; ++i) {
        const flightwall::Flight &s = batch.flights[i];
        FwFlight *f = &out->flights[i];
        double olat, olon, dlat, dlon;
        fw_community_flight(s, logos, f);
        /* owned: the progress bar (FLIGHT-SCREENS.md), the airports from the flash table */
        if (s.has_position && fw_airport_position(f->orig_icao, &olat, &olon) &&
            fw_airport_position(f->dest_icao, &dlat, &dlon))
            f->prog = fw_route_progress(olat, olon, dlat, dlon, s.latitude, s.longitude, f->gnd);
    }
    out->count = (int)n;
    out->overflow = batch.count > n ? (int)(batch.count - n) : 0;
}

double fw_community_distance(double lat1, double lon1, double lat2, double lon2){
    const double r = 6371008.8, k = 0.017453292519943295;
    double a = std::sin((lat2 - lat1) * k / 2), b = std::sin((lon2 - lon1) * k / 2);
    double h = a * a + std::cos(lat1 * k) * std::cos(lat2 * k) * b * b;
    return 2 * r * std::asin(std::sqrt(h > 1 ? 1 : h));
}

namespace {
/* "1h 05m" / "45m": a duration in whole minutes, for "Arriving in ..." */
void span(double seconds, char *out, size_t cap){
    unsigned m = seconds > 0 ? (unsigned)(seconds / 60) : 0;
    if (m >= 60) std::snprintf(out, cap, "%uh %02um", m / 60, m % 60);
    else std::snprintf(out, cap, "%um", m);
}
/* Relative to now, in the form stock's Sync rules read (as fw_sched_rel):
 * "35m ago", "2h 05m ago", "2d ago", or "in 20m" before the time. */
void rel(uint32_t t, uint32_t now, char *out, size_t cap){
    bool past = now > t;
    unsigned m = (past ? now - t : t - now) / 60;
    if (m >= 1440) std::snprintf(out, cap, past ? "%ud ago" : "in %ud", m / 1440);
    else if (m >= 60) std::snprintf(out, cap, past ? "%uh %02um ago" : "in %uh %02um", m / 60, m % 60);
    else std::snprintf(out, cap, past ? "%um ago" : "in %um", m);
}
}

void fw_community_track(FwFlight *f, double lat, double lon, bool coords, double olat, double olon, double dlat,
                        double dlon, uint32_t dep_at, uint32_t arr_at, uint32_t now){
    double rem = NAN;
    f->has_trk = true;
    f->prog = NAN;
    f->dist = NAN;
    f->trk_gnd = f->gnd;
    if (coords && lat == lat && lon == lon) {
        double flown = fw_community_distance(olat, olon, lat, lon);
        rem = fw_community_distance(lat, lon, dlat, dlon);
        f->dist = flown;
        f->prog = flown + rem > 0 ? 100.0 * flown / (flown + rem) : 0;
        if (f->gnd && rem < 10000) f->prog = 100;          /* down at the destination: landed */
        else if (f->gnd && flown < 10000) f->prog = 0;     /* still at the origin */
    } else if (dep_at && arr_at > dep_at) {
        f->prog = now <= dep_at ? 0 : now >= arr_at ? 100 : 100.0 * (now - dep_at) / (arr_at - dep_at);
    }
    if (dep_at) rel(dep_at, now, f->dep, sizeof f->dep);
    if (rem == rem && f->vel == f->vel && f->vel > 30 && !f->gnd) span(rem / f->vel, f->arr, sizeof f->arr);
    else if (arr_at) span((double)arr_at - (double)now, f->arr, sizeof f->arr);
}
