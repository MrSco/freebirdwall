#pragma once
#include "../src/flight_data.h"

namespace flightwall {
// ABI for stock image 0ab195d6... only: decoder 0x4200d594 copies the bytes into
// an inactive output owned by its flight worker. It accepts v:1 and five rows.
constexpr size_t StockMaxFlights = 5;
using StockDecoder = bool (*)(const uint8_t *, size_t, void *);

// No networking, flash writes or globals. Caller owns scratch memory and must
// not alias it with batch/output. Unknown/invalid metrics are omitted. The
// document is cleared before return; output length is zero on any failure.
Result encode_stock_frame(const Batch &, const LogoCatalog &, JsonDocument &,
                          uint8_t *, size_t capacity, size_t &length);
// Invokes the supplied stock decoder only after a complete bounded encoding.
// Use synchronously in the flight worker; never retain its output pointer.
bool deliver_stock_frame(const Batch &, const LogoCatalog &, JsonDocument &,
                         uint8_t *, size_t capacity, void *output, StockDecoder);
} // namespace flightwall
