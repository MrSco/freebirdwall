#ifndef FLIGHTWALL_TLS_PORT_H
#define FLIGHTWALL_TLS_PORT_H
/* SDK-facing interface: no Mbed TLS headers, configuration or exposed layouts.
 * Caller supplies aligned context storage, an arena and a connected transport.
 * Handles are valid only between successful init and destroy. Serialize calls
 * on each handle; transport callbacks must not reenter its protocol operations.
 * PEM ca_bytes includes its terminating NUL; DER uses its exact byte length.
 * close ownership begins only when open accepts a valid, non-busy request. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct FwTlsClient FwTlsClient;
enum {FW_TLS_AGAIN=-2};
enum {FW_TLS_OK=0,FW_TLS_INVALID=1,FW_TLS_BUSY=2,FW_TLS_MEMORY=3,FW_TLS_ENTROPY=4,
      FW_TLS_VERIFY=5,FW_TLS_PROTOCOL=6,FW_TLS_IO=7,FW_TLS_TIMEOUT=8};
typedef struct {
    void *user;
    int (*send)(void *,const uint8_t *,size_t,uint32_t);
    int (*receive)(void *,uint8_t *,size_t,uint32_t);
    bool (*random)(void *,uint8_t *,size_t);
    uint64_t (*now_ms)(void *);
    void (*pause_ms)(void *,unsigned);
    void (*close)(void *);
} FwTlsTransport;
typedef struct {
    uint32_t failure;int tls_error;uint32_t verify_flags;
    size_t arena_bytes,peak_payload_bytes,peak_blocks,remaining_payload_bytes,remaining_blocks;
} FwTlsStats;
size_t fw_tls_context_bytes(void);
size_t fw_tls_context_alignment(void);
FwTlsClient *fw_tls_init(void *storage,size_t bytes);
void fw_tls_destroy(FwTlsClient *);
bool fw_tls_open(FwTlsClient *,const FwTlsTransport *,uint8_t *arena,size_t arena_bytes,
                 const char *host,const uint8_t *ca,size_t ca_bytes,uint32_t total_timeout_ms);
bool fw_tls_write_all(FwTlsClient *,const uint8_t *,size_t);
int fw_tls_read(FwTlsClient *,uint8_t *,size_t);
void fw_tls_close(FwTlsClient *);
bool fw_tls_stats(const FwTlsClient *,FwTlsStats *);
#ifdef __cplusplus
}
#endif
#endif
