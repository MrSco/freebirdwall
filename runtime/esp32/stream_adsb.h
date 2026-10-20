#pragma once
#include "../src/flight_data.h"
namespace flightwall {
// read returns an unsigned byte, -1 for a complete HTTP body, -2 on transport
// failure. The transport must bound time and deliver dechunked/decompressed data.
struct BodyReader { void *user;int (*read)(void *); };
// Incrementally validate the entire envelope, normalizing one aircraft at a
// time. No full response allocation. Scratch JSON and row buffer are reusable;
// 4096 bytes for each is sufficient for the captured API records, not a promise
// that arbitrarily large records fit. Any failure clears staging. Max total
// body 128 KiB, nesting 12 and retained flights 32, including when input is large.
Result decode_adsb_stream(BodyReader,JsonDocument &,char *row_buffer,size_t capacity,
                          Batch &staging,const ParseOptions & = ParseOptions{});
}
