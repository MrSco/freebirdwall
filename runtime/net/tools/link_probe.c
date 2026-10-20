/* Link-size probe: references the full SDK-facing TLS interface so
 * --gc-sections keeps exactly what a firmware integration would need.
 * Never executed; used only by measure_link.py. */
#include "tls_port.h"
static int tx(void *u,const uint8_t *b,size_t n,uint32_t w){(void)u;(void)b;(void)w;return (int)n;}
static int rx(void *u,uint8_t *b,size_t n,uint32_t w){(void)u;(void)b;(void)w;return (int)n;}
static bool rnd(void *u,uint8_t *b,size_t n){(void)u;(void)b;(void)n;return true;}
static uint64_t now(void *u){(void)u;return 0;}
static void pause_ms(void *u,unsigned m){(void)u;(void)m;}
static void cls(void *u){(void)u;}
int probe_entry(void *storage,size_t storage_bytes,uint8_t *arena,size_t arena_bytes){
    FwTlsTransport t={0,tx,rx,rnd,now,pause_ms,cls};
    FwTlsClient *c=fw_tls_init(storage,storage_bytes);
    uint8_t b[64]={0};FwTlsStats s;
    if(!c) return -1;
    if(fw_tls_open(c,&t,arena,arena_bytes,"api.adsb.fi",b,sizeof b,10000)){
        fw_tls_write_all(c,b,sizeof b);fw_tls_read(c,b,sizeof b);
    }
    fw_tls_close(c);fw_tls_stats(c,&s);fw_tls_destroy(c);
    return (int)fw_tls_context_bytes()+(int)fw_tls_context_alignment();
}
