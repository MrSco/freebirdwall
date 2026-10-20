#include "tls_port.h"
#include "tls_client.h"
#include <new>
using flightwall::net::Client;
static Client *client(FwTlsClient *p){return reinterpret_cast<Client *>(p);}
extern "C" {
size_t fw_tls_context_bytes(void){return sizeof(Client);}
size_t fw_tls_context_alignment(void){return alignof(Client);}
FwTlsClient *fw_tls_init(void *storage,size_t bytes){
    if(!storage||bytes<sizeof(Client)||reinterpret_cast<uintptr_t>(storage)%alignof(Client))return nullptr;
    return reinterpret_cast<FwTlsClient *>(new(storage)Client());
}
void fw_tls_destroy(FwTlsClient *p){if(p)client(p)->~Client();}
bool fw_tls_open(FwTlsClient *p,const FwTlsTransport *io,uint8_t *arena,size_t bytes,
                 const char *host,const uint8_t *ca,size_t ca_bytes,uint32_t timeout){
    if(!p||!io)return false;
    const flightwall::net::Transport transport{io->user,io->send,io->receive,io->random,io->now_ms,io->pause_ms,io->close};
    return client(p)->open(transport,arena,bytes,host,ca,ca_bytes,timeout);
}
bool fw_tls_write_all(FwTlsClient *p,const uint8_t *bytes,size_t n){return p&&client(p)->write_all(bytes,n);}
int fw_tls_read(FwTlsClient *p,uint8_t *bytes,size_t n){return p?client(p)->read(bytes,n):-1;}
void fw_tls_close(FwTlsClient *p){if(p)client(p)->close();}
bool fw_tls_stats(const FwTlsClient *p,FwTlsStats *out){
    if(!p||!out)return false;
    const auto &s=reinterpret_cast<const Client *>(p)->stats();
    *out={static_cast<uint32_t>(s.failure),s.tls_error,s.verify_flags,s.arena_bytes,s.peak_payload_bytes,
        s.peak_blocks,s.remaining_payload_bytes,s.remaining_blocks};return true;
}
}
