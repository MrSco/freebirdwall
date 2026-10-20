/* For tests/test_community.py: community records -> the screens' list.
 *   community_dump frame <positions.json> [<route.json> <callsign>]
 *     decodes adsb.fi JSON (the shared reader, 8 records), optionally adds an
 *     adsbdb route to the flight with that callsign, prints the FwFeedFrame
 *   community_dump airport <text>   /   community_dump aircraft <text> */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "fw_community_frame.h"
#include "fw_aero.h"

using namespace flightwall;

static std::vector<uint8_t> slurp(const char *path){
    std::vector<uint8_t> b;
    FILE *f = std::fopen(path, "rb");
    if (!f) return b;
    int c;
    while ((c = std::fgetc(f)) != EOF) b.push_back((uint8_t)c);
    std::fclose(f);
    return b;
}

static void js(const char *s){
    std::putchar('"');
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') std::printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20) std::printf("\\u%04x", *s);
        else std::putchar(*s);
    }
    std::putchar('"');
}
static void num(double v){ if (v != v) std::printf("null"); else std::printf("%.4f", v); }

static bool exists(void *, const char *path){
    /* a stand-in asset partition: DAL and BAW logos and the category icons */
    static const char *const have[] = {"/images/airlines/square/DAL.png", "/images/airlines/square/BAW.png",
                                       "/images/airlines/square/heli.png", "/images/airlines/square/default.png"};
    for (const char *h : have) if (!std::strcmp(h, path)) return true;
    return false;
}

int main(int argc, char **argv){
    char out[128];
    if (argc == 3 && !std::strcmp(argv[1], "airport")) { fw_community_airport_name(argv[2], out, 32); js(out); std::puts(""); return 0; }
    if (argc == 3 && !std::strcmp(argv[1], "aircraft")) { fw_community_aircraft_name(argv[2], out, 32); js(out); std::puts(""); return 0; }
    if (argc == 13 && !std::strcmp(argv[1], "track")) {
        /* track gnd vel lat lon coords olat olon dlat dlon dep_at arr_at  (now = 1792500000) */
        static FwFlight f;
        fw_flight_init(&f);
        f.gnd = std::atoi(argv[2]) != 0;
        f.vel = std::atof(argv[3]);
        fw_community_track(&f, std::atof(argv[4]), std::atof(argv[5]), std::atoi(argv[6]) != 0, std::atof(argv[7]),
                           std::atof(argv[8]), std::atof(argv[9]), std::atof(argv[10]), (uint32_t)std::atol(argv[11]),
                           (uint32_t)std::atol(argv[12]), 1792500000u);
        std::printf("{\"has_trk\":%s,\"prog\":", f.has_trk ? "true" : "false"); num(f.prog);
        std::printf(",\"dist\":"); num(f.dist);
        std::printf(",\"dep\":"); js(f.dep);
        std::printf(",\"arr\":"); js(f.arr);
        std::printf(",\"trk_gnd\":%s}\n", f.trk_gnd ? "true" : "false");
        return 0;
    }
    if (argc == 3 && !std::strcmp(argv[1], "aerotime")) { std::printf("%u\n", (unsigned)fw_aero_time(argv[2])); return 0; }
    if (argc == 4 && !std::strcmp(argv[1], "aero")) {
        /* aero <flights answer> <now>: read through the filter, as on the board */
        std::vector<uint8_t> body = slurp(argv[2]);
        static StaticJsonDocument<4096> doc;
        static StaticJsonDocument<768> filter;
        fw_aero_flights_filter(filter);
        if (deserializeJson(doc, body.data(), body.size(), DeserializationOption::Filter(filter))) return 3;
        FwAeroFlight a;
        bool ok = fw_aero_pick(doc.as<JsonVariantConst>(), (uint32_t)std::strtoul(argv[3], nullptr, 10), &a);
        std::printf("{\"ok\":%s,\"airborne\":%s,\"landed\":%s,\"dep\":%u,\"arr\":%u,\"progress\":", ok ? "true" : "false",
                    a.airborne ? "true" : "false", a.landed ? "true" : "false", (unsigned)a.dep, (unsigned)a.arr);
        num(a.progress);
        std::printf(",\"fa_id\":"); js(a.fa_id);
        std::printf(",\"orig_iata\":"); js(a.orig_iata);
        std::printf(",\"orig_icao\":"); js(a.orig_icao);
        std::printf(",\"orig\":"); js(a.orig);
        std::printf(",\"dest_iata\":"); js(a.dest_iata);
        std::printf(",\"dest\":"); js(a.dest);
        std::printf(",\"ac_type\":"); js(a.ac_type);
        std::printf(",\"reg\":"); js(a.reg);
        std::printf(",\"doc_bytes\":%u}\n", (unsigned)doc.memoryUsage());
        return 0;
    }
    if (argc == 3 && !std::strcmp(argv[1], "aeropos")) {
        std::vector<uint8_t> body = slurp(argv[2]);
        static StaticJsonDocument<1024> doc;
        static StaticJsonDocument<256> filter;
        fw_aero_position_filter(filter);
        if (deserializeJson(doc, body.data(), body.size(), DeserializationOption::Filter(filter))) return 3;
        double lat, lon, alt, gs, hd;
        bool ok = fw_aero_position(doc.as<JsonVariantConst>(), &lat, &lon, &alt, &gs, &hd);
        std::printf("{\"ok\":%s,\"lat\":", ok ? "true" : "false");
        if (ok) { num(lat); std::printf(",\"lon\":"); num(lon); std::printf(",\"alt\":"); num(alt);
                  std::printf(",\"gs\":"); num(gs); std::printf(",\"heading\":"); num(hd); }
        else std::printf("null");
        std::printf(",\"doc_bytes\":%u}\n", (unsigned)doc.memoryUsage());
        return 0;
    }
    if ((argc != 3 && argc != 5) || std::strcmp(argv[1], "frame")) return 2;
    std::vector<uint8_t> pos = slurp(argv[2]);
    static StaticJsonDocument<16384> doc;
    static Batch batch;
    ParseOptions opts;
    opts.max_position_age_seconds = 1e9;
    Result r = decode(Provider::AdsbFi, Encoding::Json, pos.data(), pos.size(), doc, batch, opts);
    if (r != Result::Ok) { std::printf("{\"result\":%d}\n", (int)r); return 0; }
    if (argc == 5) {
        std::vector<uint8_t> route = slurp(argv[3]);
        static StaticJsonDocument<4096> rdoc;
        if (deserializeJson(rdoc, route.data(), route.size())) return 3;
        for (size_t i = 0; i < batch.count; ++i)
            if (!std::strcmp(batch.flights[i].callsign, argv[4])) enrich_route(rdoc.as<JsonVariantConst>(), argv[4], batch.flights[i]);
    }
    LogoCatalog logos{nullptr, exists, nullptr, 0};
    static FwFeedFrame frame;
    std::memset(&frame, 0xA5, sizeof frame);                 /* everything must be overwritten */
    fw_community_frame(batch, logos, &frame);
    std::printf("{\"result\":0,\"count\":%d,\"overflow\":%d,\"tracked\":%s,\"flights\":[", frame.count, frame.overflow,
                frame.tracked ? "true" : "false");
    for (int i = 0; i < frame.count; ++i) {
        const FwFlight &f = frame.flights[i];
        if (i) std::putchar(',');
        std::printf("{\"icao\":"); js(f.icao);
        std::printf(",\"cs\":"); js(f.cs);
        std::printf(",\"reg\":"); js(f.reg);
        std::printf(",\"alt_b\":"); num(f.alt_b);
        std::printf(",\"alt_g\":"); num(f.alt_g);
        std::printf(",\"vel\":"); num(f.vel);
        std::printf(",\"trk\":"); num(f.trk);
        std::printf(",\"vr\":"); num(f.vr);
        std::printf(",\"gnd\":%s", f.gnd ? "true" : "false");
        std::printf(",\"orig\":"); js(f.orig);
        std::printf(",\"dest\":"); js(f.dest);
        std::printf(",\"orig_iata\":"); js(f.orig_iata);
        std::printf(",\"orig_icao\":"); js(f.orig_icao);
        std::printf(",\"dest_iata\":"); js(f.dest_iata);
        std::printf(",\"dest_icao\":"); js(f.dest_icao);
        std::printf(",\"al_code\":"); js(f.al_code);
        std::printf(",\"al_name\":"); js(f.al_name);
        std::printf(",\"ac_type\":"); js(f.ac_type);
        std::printf(",\"ac_name\":"); js(f.ac_name);
        std::printf(",\"ac_class\":"); js(f.ac_class);
        std::printf(",\"has_trk\":%s,\"prog\":", f.has_trk ? "true" : "false"); num(f.prog);
        std::printf(",\"sqk\":"); js(f.sqk);
        std::printf("}");
    }
    std::printf("]}\n");
    return 0;
}
