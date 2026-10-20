#pragma once
#include "flight_data.h"

namespace flightwall {
struct Zone { double latitude, longitude, radius_km; };
struct Request {
    const char *host;
    const char *method;
    const char *accept;
    bool tls, needs_device_key;
    char path[160];
};
// Fixed endpoints only. Location/radius are checked before URL construction.
// HTTPS transport must verify peer identity; callbacks must not follow redirects
// or forward device credentials to a community/route/asset server.
bool make_request(Provider, const Zone &, Request &);
// Flatten only the known local request/filter fields used by the vendor wire
// serializer. Never send display settings, pairing material, or owner commands.
bool vendor_request_body(JsonVariantConst local_request_config, JsonDocument &out);

class PollBudget {
public:
    bool ready(uint32_t now_ms) const;
    // Reserve before issuing a request, across ALL zones for this source.
    bool reserve(uint32_t now_ms);
    void complete(uint32_t now_ms, unsigned http_status, uint32_t retry_after_seconds=0);
    uint32_t next_ms() const { return next_; }
private:
    uint32_t next_=0;
    unsigned failures_=0;
    bool scheduled_=false;
};

struct FrameState {
    bool has_good_frame=false;
    uint32_t updated_ms=0;
    Provider provider=Provider::Vendor;
    // Call on explicit source change before showing data from the next source.
    void select(Provider value, Batch &active);
    // Failure retains the previous frame/timestamp. No silent provider fallback.
    bool publish(Result, const Batch &staging, uint32_t now_ms, Batch &active);
    bool stale(uint32_t now_ms, uint32_t max_age_ms=60000) const;
};
} // namespace flightwall
