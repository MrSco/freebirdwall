#ifndef FLIGHTWALL_ESP32_TRANSPORT_H
#define FLIGHTWALL_ESP32_TRANSPORT_H
/* Bounded socket transport for the source-built TLS component (tls_port.h).
 *
 * Supplies everything FwTlsTransport needs: DNS + non-blocking connect with a
 * deadline, select()-bounded send/receive slices, hardware entropy, a monotonic
 * millisecond clock, a yielding pause and idempotent close.
 *
 * Built for ESP-IDF (lwIP BSD sockets). The same source also compiles on a
 * desktop (POSIX or Winsock) solely so the loopback HTTPS acceptance suite can
 * exercise this exact connect/IO/close code; desktop builds are never shipped.
 *
 * Ownership: fw_esp_transport_connect() leaves an open socket in the handle.
 * Pass fw_esp_transport_bind() to fw_tls_open(); from then on the TLS client
 * owns closing it. If fw_tls_open() is never called, call
 * fw_esp_transport_close() yourself. Close is idempotent.
 *
 * One handle per connection; not thread safe. All waits are bounded. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tls_port.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Longest single blocking wait inside send/receive. The TLS client re-enters
 * with its remaining budget, so this only bounds how long one call can hold the
 * task and keeps the task watchdog and cooperative scheduling healthy. */
#define FW_ESP_IO_SLICE_MS 250u

/* DNS/connect failure reasons, for diagnostics only. */
enum {
    FW_ESP_OK = 0,
    FW_ESP_ERR_ARGS = 1,     /* bad host/port/handle */
    FW_ESP_ERR_ENTROPY = 2,  /* hardware RNG not yet backed by the radio */
    FW_ESP_ERR_DNS = 3,
    FW_ESP_ERR_SOCKET = 4,   /* socket()/non-blocking setup failed */
    FW_ESP_ERR_CONNECT = 5,  /* refused/unreachable on every address */
    FW_ESP_ERR_TIMEOUT = 6
};

typedef struct {
    int fd;                  /* -1 when closed */
    int last_error;          /* FW_ESP_* from connect */
    int last_errno;          /* platform errno/WSA error of the last failure */
    uint32_t closes;         /* how many times close actually released a socket */
    uint32_t again;          /* send/receive slices that timed out (Again) */
    uint32_t bytes_sent, bytes_received;
} FwEspTransport;

void fw_esp_transport_init(FwEspTransport *t);

/* Resolves host and connects to the first reachable address within
 * timeout_ms. DNS itself uses the platform resolver's own timeout, which is
 * not interruptible here; the deadline is checked before and after it. */
bool fw_esp_transport_connect(FwEspTransport *t, const char *host,
                              const char *port, uint32_t timeout_ms);

/* Fills the callback table for fw_tls_open(). */
void fw_esp_transport_bind(FwEspTransport *t, FwTlsTransport *out);

void fw_esp_transport_close(FwEspTransport *t);

/* The individual callbacks, exposed so tests can decorate them. */
int fw_esp_transport_send(void *user, const uint8_t *bytes, size_t n, uint32_t wait_ms);
int fw_esp_transport_receive(void *user, uint8_t *bytes, size_t n, uint32_t wait_ms);
bool fw_esp_transport_random(void *user, uint8_t *bytes, size_t n);
uint64_t fw_esp_transport_now_ms(void *user);
void fw_esp_transport_pause_ms(void *user, unsigned ms);
void fw_esp_transport_close_cb(void *user);

/* Entropy gate. On ESP32-S3 the hardware RNG only produces true random numbers
 * while the RF subsystem (Wi-Fi/BT) or the bootloader entropy source is
 * running; otherwise it is pseudo-random. The firmware must call
 * fw_esp_entropy_ready(true) after esp_wifi_start() succeeds; until then the
 * random callback refuses, and the TLS handshake fails closed. Desktop builds
 * start ready because the OS RNG is always trusted. */
void fw_esp_entropy_ready(bool ready);

/* Certificate validity checks need a correct wall clock. Returns true when the
 * system time is plausibly set (after SNTP), i.e. not near the 1970 epoch. */
bool fw_esp_wall_clock_valid(void);

#ifdef __cplusplus
}
#endif
#endif
