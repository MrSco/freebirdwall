#include "tls_port.h"
#include "../../../shared/owner_route_root.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#ifdef MBEDTLS_CONFIG_FILE
#error The plain C interface must not impose its TLS configuration on an SDK
#endif
static uint64_t ticks;static unsigned closes;
static int send_bytes(void *p,const uint8_t *bytes,size_t n,uint32_t ms){(void)p;(void)bytes;assert(ms);return (int)n;}
static int receive_bytes(void *p,uint8_t *bytes,size_t n,uint32_t ms){(void)p;(void)bytes;(void)n;assert(ms);return FW_TLS_AGAIN;}
static bool rng(void *p,uint8_t *bytes,size_t n){(void)p;memset(bytes,0x37,n);return false;}
static uint64_t now(void *p){(void)p;return ticks;}
static void pause_ms(void *p,unsigned n){(void)p;ticks+=n;}
static void close_io(void *p){(void)p;++closes;}
int main(void){
    void *storage=malloc(fw_tls_context_bytes()+fw_tls_context_alignment());uint8_t arena[65536];FwTlsStats stats;
    FwTlsTransport io={0,send_bytes,receive_bytes,rng,now,pause_ms,close_io};FwTlsClient *c;
    assert(storage&&!fw_tls_init(storage,1));
    assert(!fw_tls_init((uint8_t *)storage+1,fw_tls_context_bytes()));
    c=fw_tls_init(storage,fw_tls_context_bytes());assert(c);
    assert(!fw_tls_open(c,&io,arena,sizeof arena,"flightwall.test",(const uint8_t *)owner_route_root,sizeof owner_route_root,100));
    assert(fw_tls_stats(c,&stats)&&stats.failure==FW_TLS_ENTROPY&&!stats.remaining_blocks&&closes==1);
    fw_tls_close(c);fw_tls_destroy(c);assert(closes==1);free(storage);return 0;
}
