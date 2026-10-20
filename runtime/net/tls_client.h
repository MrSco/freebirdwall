#ifndef FLIGHTWALL_SOURCE_TLS_CLIENT_H
#define FLIGHTWALL_SOURCE_TLS_CLIENT_H
#include <cstddef>
#include <cstdint>
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

namespace flightwall::net {
enum class Failure { None, Invalid, Busy, Memory, Entropy, Verify, Tls, Io, Timeout };
struct Stats {
    Failure failure=Failure::None;
    int tls_error=0;
    uint32_t verify_flags=0;
    size_t arena_bytes=0,peak_payload_bytes=0,peak_blocks=0,remaining_payload_bytes=0,remaining_blocks=0;
};
/* A preconnected byte transport. Positive = bytes transferred, 0 = EOF,
 * Again = retry, other negative = failure. Every operation must honor its
 * supplied wait budget; DNS/connect timing belongs to the platform adapter.
 * random must fill all requested bytes from a trusted system/hardware RNG.
 * close is called exactly once after ownership is accepted, even on failure. */
constexpr int Again=-2;
struct Transport {
    void *user=nullptr;
    int (*send)(void *,const uint8_t *,size_t,uint32_t)=nullptr;
    int (*receive)(void *,uint8_t *,size_t,uint32_t)=nullptr;
    bool (*random)(void *,uint8_t *,size_t)=nullptr;
    uint64_t (*now_ms)(void *)=nullptr;
    void (*pause_ms)(void *,unsigned)=nullptr;
    void (*close)(void *)=nullptr;
};

/* Dedicated source-built library, one active connection at a time. Its arena
 * allocator is process-global inside this library, so it must never be linked
 * into the patched stock runtime under stock mbedtls symbol names. */
class Client {
public:
    Client()=default;
    ~Client(){close();}
    Client(const Client &)=delete;
    Client &operator=(const Client &)=delete;
    bool open(const Transport &,uint8_t *arena,size_t arena_bytes,
              const char *hostname,const uint8_t *ca,size_t ca_bytes,uint32_t total_timeout_ms);
    bool write_all(const uint8_t *,size_t);
    int read(uint8_t *,size_t);
    void close();
    const Stats &stats() const{return stats_;}
    bool ready() const{return ready_;}
private:
    mbedtls_ssl_context ssl_{};
    mbedtls_ssl_config config_{};
    mbedtls_ctr_drbg_context rng_{};
    mbedtls_x509_crt ca_{};
    Transport io_{};
    Stats stats_{};
    uint64_t deadline_=0;
    bool owns_=false,initialized_=false,handshake_=false,ready_=false;
    uint32_t remaining() const;
    bool fail(Failure,int);
    static int send_bytes(void *,const unsigned char *,size_t);
    static int receive_bytes(void *,unsigned char *,size_t);
    static int entropy(void *,unsigned char *,size_t);
};
}
#endif
