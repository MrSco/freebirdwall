#ifndef FLIGHTWALL_HTTPS_GET_H
#define FLIGHTWALL_HTTPS_GET_H
/* One HTTPS GET on the board, shared by the community source (community.cpp)
 * and the app settings (app_settings.cpp). Both run on the feed task, the one
 * user of fw_tls_context. The shared block (big_buffer.h) is the TLS arena,
 * held only for TLS and the transfer; a logo waiting for it goes first. */
#include <cstdio>
#include <cstring>
#include "http_body.h"
#include "esp32_transport.h"
#include "tls_port.h"
#include "big_buffer.h"
#include "community.h"          /* fw_tls_context */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace fw_https {
constexpr uint32_t ConnectMs = 10000, RequestMs = 15000;
constexpr uint32_t BlockWaitMs = 3000;     /* a logo decode holds the block for tens of ms */
constexpr int LogoYieldSteps = 25;         /* up to 0.5 s for a waiting logo */

struct TlsReader {
    FwTlsClient *client;
    static int next(void *p, uint8_t *bytes, size_t n){ return fw_tls_read(static_cast<TlsReader *>(p)->client, bytes, n); }
};
} // namespace fw_https

/* GET https://host/path, trusting only roots (PEM, NUL included in
 * roots_len); headers (NULL or "Name: value\r\n" lines) are added to the
 * request and wiped from the stack after sending. consume reads the body
 * after the headers. Returns 0 when the request completed (any HTTP status),
 * else the failed stage: 1 connect, 2 TLS, 3 send, 4 headers, 5 body, 6 the
 * block stayed busy. */
template <class Consume>
int fw_https_get(const char *host, const char *path, const char *headers, const uint8_t *roots, size_t roots_len,
                 unsigned *status, uint32_t *retry_after, Consume consume){
    using namespace fw_https;
    *status = 0;
    *retry_after = 0;
    FwEspTransport socket;
    fw_esp_transport_init(&socket);
    int stage = 0;
    /* Connect first: DNS and TCP need no arena. */
    if (!fw_esp_transport_connect(&socket, host, "443", ConnectMs)) return 1;
    for (int i = 0; fw_big_wanted() && i < LogoYieldSteps; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t *arena = fw_big_take(BlockWaitMs);
    if (!arena) { fw_esp_transport_close(&socket); return 6; }
    FwTlsClient *client = fw_tls_init(fw_tls_context, fw_tls_context_bytes_reserved);
    if (!client) { fw_esp_transport_close(&socket); fw_big_give(); return 2; }
    FwTlsTransport io{};
    fw_esp_transport_bind(&socket, &io);
    /* From here the TLS client owns closing the socket, even on failure. */
    if (!fw_tls_open(client, &io, arena, fw_big_bytes, host, roots, roots_len, RequestMs)) {
        stage = 2;
    } else {
        char request[384];
        int n = std::snprintf(request, sizeof request,
                              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: FlightWall-Owned/1\r\n%s"
                              "Accept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                              path, host, headers ? headers : "");
        bool sent = n > 0 && (size_t)n < sizeof request &&
                    fw_tls_write_all(client, reinterpret_cast<const uint8_t *>(request), (size_t)n);
        std::memset(request, 0, sizeof request);
        if (!sent) {
            stage = 3;
        } else {
            TlsReader reader{client};
            flightwall::HttpBody body({&reader, TlsReader::next});
            if (!body.headers()) stage = 4;
            else {
                *status = body.status();
                *retry_after = body.retry_after();
                if (!consume(body)) stage = 5;
            }
        }
    }
    FwTlsStats stats{};
    fw_tls_close(client);
    fw_tls_stats(client, &stats);
    fw_tls_destroy(client);
    fw_esp_transport_close(&socket);
    fw_big_give();
    if (!stage && (stats.failure || stats.remaining_blocks)) stage = 2;
    return stage;
}

#endif
