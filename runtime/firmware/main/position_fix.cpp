/* Owned (FLIGHT-SCREENS.md "Progress bar"): on the FlightWall feed, which
 * sends no positions, one aircraft's position by its ICAO address from
 * adsb.fi (/api/v2/hex/<hex>, GTS Root R4 as for the community source), for
 * the progress bar. Feed task only (https_get.h). The answer is scanned as it
 * arrives, byte by byte, for the first aircraft's lat, lon, alt_baro and
 * seen_pos: no JSON document, under 100 bytes of state. */
#include "position_fix.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "https_get.h"
#include "owner_tls_root.h"

namespace {

/* The keys of the first aircraft object ({"ac":[{...}]}, depth 3); nested
 * objects such as lastPosition are deeper and ignored. */
struct Scan {
    int depth = 0, aircraft = 0;
    bool in_str = false, esc = false, after_key = false, want_value = false;
    char key[12] = {0}, str[12] = {0}, num[24] = {0};
    size_t key_n = 0, str_n = 0, num_n = 0;
    bool have_lat = false, have_lon = false, ground = false;
    double lat = 0, lon = 0, seen_pos = 0;

    void value(const char *v, bool quoted){
        if (depth != 3 || aircraft != 1) return;
        if (!std::strcmp(key, "lat") && !quoted) { lat = std::strtod(v, nullptr); have_lat = true; }
        else if (!std::strcmp(key, "lon") && !quoted) { lon = std::strtod(v, nullptr); have_lon = true; }
        else if (!std::strcmp(key, "alt_baro")) ground = quoted && !std::strcmp(v, "ground");
        else if (!std::strcmp(key, "seen_pos") && !quoted) seen_pos = std::strtod(v, nullptr);
    }
    void end_number(){
        if (!num_n) return;
        num[num_n] = 0;
        value(num, false);
        num_n = 0;
        want_value = false;
    }
    void feed(int c){
        if (in_str) {
            if (esc) { esc = false; return; }
            if (c == '\\') { esc = true; return; }
            if (c == '"') {
                in_str = false;
                str[str_n] = 0;
                if (want_value) { value(str, true); want_value = false; }
                else { std::memcpy(key, str, sizeof key); after_key = true; }
                return;
            }
            if (str_n + 1 < sizeof str) str[str_n++] = (char)c;
            return;
        }
        if (num_n && !(c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' || (c >= '0' && c <= '9'))) end_number();
        switch (c) {
        case '"': in_str = true; str_n = 0; break;
        case ':': if (after_key) { want_value = true; after_key = false; } break;
        case '{': case '[':
            if (++depth == 3 && c == '{') ++aircraft;
            want_value = false;
            break;
        case '}': case ']': --depth; want_value = false; break;
        case ',': want_value = false; after_key = false; break;
        default:
            if (want_value && (c == '-' || (c >= '0' && c <= '9') || c == '.')) {
                if (num_n + 1 < sizeof num) num[num_n++] = (char)c;
            } else if (want_value && c > ' ') {
                want_value = false;      /* true, false, null */
            }
        }
    }
};

} // namespace

extern "C" int fw_position_fix(const char *hex, double *lat, double *lon, bool *ground, unsigned *status){
    char path[32];
    uint32_t retry;
    Scan scan;
    int stage;
    *status = 0;
    if (!hex || std::strlen(hex) != 6) return -1;
    std::snprintf(path, sizeof path, "/api/v2/hex/%s", hex);
    stage = fw_https_get("opendata.adsb.fi", path, nullptr, reinterpret_cast<const uint8_t *>(owner_tls_gts_r4),
                         sizeof owner_tls_gts_r4, status, &retry, [&](flightwall::HttpBody &body){
        int c;
        while ((c = body.read()) >= 0) if (*status == 200) scan.feed(c);
        return c == -1;
    });
    if (stage) return stage;
    if (*status != 200) return 10;
    if (!scan.have_lat || !scan.have_lon || scan.seen_pos > 60) return 11;     /* not seen, or no fresh position */
    *lat = scan.lat;
    *lon = scan.lon;
    *ground = scan.ground;
    return 0;
}
