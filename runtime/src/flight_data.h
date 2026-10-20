#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cstring>
#include <type_traits>
#include "../third_party/ArduinoJson/ArduinoJson-v6.21.5.h"

namespace flightwall {
#ifndef FLIGHTWALL_MAX_FLIGHTS
#define FLIGHTWALL_MAX_FLIGHTS 32
#endif
constexpr size_t MaxFlights = FLIGHTWALL_MAX_FLIGHTS, MaxResponseBytes = 128 * 1024;
static_assert(MaxFlights>=5&&MaxFlights<=32,"bounded flight capacity");
enum class Provider : uint8_t { Vendor, AdsbFi };
enum class Encoding : uint8_t { Json, MessagePack };
enum class Result : uint8_t { Ok, Invalid, TooLarge, NoMemory, Unsupported };
enum class Unit : uint8_t { Metres, MetresPerSecond, Degrees, VendorUnverified };
enum class RouteSource : uint8_t { Unknown, Vendor, CallsignDatabase };
enum class Category : uint8_t { Unknown, Helicopter, Light, SmallJet };
struct Metric { double value; Unit unit; bool valid; };
struct Airport { char iata[4], icao[5], name[64]; };
struct Airline {
    char icao[4], iata[3], name[64];
    // Preserve these verbatim as bounded text; operating/livery semantics TBD.
    char vendor_code[8], vendor_paint[8], vendor_operator[8];
};
struct Flight {
    Provider provider;
    char aircraft_id[8], callsign[16], registration[16], aircraft_type[12], aircraft_name[64];
    Airline airline;
    Airport origin, destination;
    RouteSource route_source;
    Category category;
    Metric altitude, speed, heading, vertical_rate;
    double latitude, longitude, position_age_seconds;
    bool has_position, has_position_age, on_ground, has_ground_state;
};
struct Batch {
    Flight flights[MaxFlights];
    size_t count, rejected, duplicates;
    bool overflow;
    Provider provider;
};
static_assert(std::is_trivially_copyable<Batch>::value,"Batch is cleared in place");
// Clears in place: `b=Batch{}` may build a whole temporary Batch on the stack,
// which a small embedded task stack cannot spare.
inline void reset(Batch &b,Provider provider){std::memset(static_cast<void *>(&b),0,sizeof b);b.provider=provider;}
struct ParseOptions {
    double max_position_age_seconds = 60;
    void *selection_user=nullptr;
    bool (*allow)(void *,const Flight &)=nullptr;
    double (*rank)(void *,const Flight &)=nullptr;
};

// Caller owns document and staging batch: no network, settings, flash or globals.
// On failure staging is cleared. Publish staging only on Ok; retain last-good data
// in a separate active batch. Input buffer is read-only; nesting and sizes bounded.
Result decode(Provider, Encoding, const uint8_t *, size_t, JsonDocument &,
              Batch &staging, const ParseOptions & = ParseOptions{});
// Shared normalization/deduplication for a streaming community reader. Invalid
// individual aircraft are counted/skipped, as with decode(); the caller must
// publish only after validating the complete enclosing response.
Result append_adsb_aircraft(JsonVariantConst, Batch &, const ParseOptions & = ParseOptions{});
bool json_object_envelope(const uint8_t *,size_t);
// Route lookups are lower-confidence enrichment, never live schedule/ETA data.
// Match the requested callsign; don't replace vendor routes or known endpoints.
bool enrich_route(JsonVariantConst response, const char *requested_callsign, Flight &);

struct AirlineCode { const char *icao, *iata; };
struct LogoCatalog {
    void *user;
    bool (*exists)(void *, const char *path);
    const AirlineCode *crosswalk;
    size_t crosswalk_count;
};
enum class LogoKind : uint8_t { Owner, Stock, Category, Text };
struct Logo { LogoKind kind; char path[64], airline_code[4]; bool inferred_from_callsign; };
Logo resolve_logo(const Flight &, const LogoCatalog &);
} // namespace flightwall
