/* Bounded route request through the owned network stack. Emits counts and
 * memory statistics only; never prints response bodies or credentials. */
#include "route_probe.h"
#include "memory_probe.h"
#include "esp32_transport.h"
#include "tls_port.h"
#include "http_body.h"
#include "owner_route_root.h"   /* ISRG Root X2, pinned by SHA in its header */
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t ConnectTimeoutMs=10000;
constexpr uint32_t RequestTimeoutMs=15000;
constexpr unsigned RoundSpacingMs=5000;   /* at least 5 s between requests */

struct Reader {
    FwTlsClient *client;
    static int next(void *p,uint8_t *bytes,size_t n){
        return fw_tls_read(static_cast<Reader *>(p)->client,bytes,n);
    }
};

bool one_round(unsigned round,uint8_t *arena,size_t arena_bytes,void *storage,size_t storage_bytes){
    char extra[320];
    FwEspTransport socket;
    fw_esp_transport_init(&socket);

    if(!fw_esp_transport_connect(&socket,CONFIG_FW_PROBE_HOST,"443",ConnectTimeoutMs)){
        std::snprintf(extra,sizeof extra,"\"round\":%u,\"ok\":false,\"connect_error\":%d,\"errno\":%d",
                      round,socket.last_error,socket.last_errno);
        fw_mem_report("route_connect_failed",extra);
        return false;
    }

    FwTlsClient *client=fw_tls_init(storage,storage_bytes);
    if(!client){fw_esp_transport_close(&socket);fw_mem_report("route_init_failed",nullptr);return false;}

    FwTlsTransport io{};
    fw_esp_transport_bind(&socket,&io);
    /* From here the TLS client owns closing the socket, even on failure. PEM
     * length includes the terminating NUL, as tls_port.h requires. */
    bool ok=fw_tls_open(client,&io,arena,arena_bytes,CONFIG_FW_PROBE_HOST,
                        reinterpret_cast<const uint8_t *>(owner_route_root),sizeof owner_route_root,
                        RequestTimeoutMs);
    unsigned status=0;
    size_t body_bytes=0;
    if(ok){
        std::snprintf(extra,sizeof extra,"\"round\":%u",round);
        fw_mem_report("route_handshake",extra);   /* the peak-pressure moment */
        static const char request[]=
            "GET " CONFIG_FW_PROBE_PATH " HTTP/1.1\r\n"
            "Host: " CONFIG_FW_PROBE_HOST "\r\n"
            "User-Agent: FlightWall-Source/0.1\r\n"
            "Accept: application/json\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n";
        ok=fw_tls_write_all(client,reinterpret_cast<const uint8_t *>(request),sizeof request-1);
        if(ok){
            Reader reader{client};
            flightwall::HttpBody body({&reader,Reader::next});
            ok=body.headers();
            if(ok){
                status=body.status();
                int c;
                while((c=body.read())>=0)++body_bytes;
                ok=(c==-1)&&status==200;
            }
        }
    }

    FwTlsStats stats{};
    fw_tls_close(client);
    fw_tls_stats(client,&stats);
    fw_tls_destroy(client);
    fw_esp_transport_close(&socket);   /* idempotent; the client already closed it */

    ok=ok&&stats.failure==0&&stats.remaining_blocks==0;
    std::snprintf(extra,sizeof extra,
        "\"round\":%u,\"ok\":%s,\"status\":%u,\"body_bytes\":%u,\"failure\":%u,\"tls_error\":%d,"
        "\"verify_flags\":%u,\"arena_bytes\":%u,\"peak_payload\":%u,\"peak_blocks\":%u,"
        "\"remaining_blocks\":%u,\"closes\":%u,\"io_again\":%u,\"tx\":%u,\"rx\":%u",
        round,ok?"true":"false",status,(unsigned)body_bytes,(unsigned)stats.failure,stats.tls_error,
        (unsigned)stats.verify_flags,(unsigned)stats.arena_bytes,(unsigned)stats.peak_payload_bytes,
        (unsigned)stats.peak_blocks,(unsigned)stats.remaining_blocks,(unsigned)socket.closes,
        (unsigned)socket.again,(unsigned)socket.bytes_sent,(unsigned)socket.bytes_received);
    fw_mem_report("route_done",extra);
    return ok;
}
}

extern "C" bool fw_route_probe_run(uint8_t *arena,size_t arena_bytes,void *storage,size_t storage_bytes){
    if(!fw_esp_wall_clock_valid()){fw_mem_report("route_skipped_no_clock",nullptr);return false;}
    if(storage_bytes<fw_tls_context_bytes()){
        char extra[96];
        std::snprintf(extra,sizeof extra,"\"need\":%u,\"have\":%u",
                      (unsigned)fw_tls_context_bytes(),(unsigned)storage_bytes);
        fw_mem_report("route_context_too_small",extra);
        return false;
    }
    bool all=true;
    for(unsigned round=0;round<CONFIG_FW_PROBE_ROUNDS;++round){
        if(round)vTaskDelay(pdMS_TO_TICKS(RoundSpacingMs));
        all=one_round(round,arena,arena_bytes,storage,storage_bytes)&&all;
    }
    return all;
}
