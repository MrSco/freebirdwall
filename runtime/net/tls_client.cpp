#include "tls_client.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/entropy.h"
#include <atomic>
#include <climits>
#include <cstring>

namespace {
uint64_t (*tls_clock)(void *)=nullptr;
void *tls_clock_user=nullptr;
}
extern "C" mbedtls_ms_time_t mbedtls_ms_time(void){
    return tls_clock?static_cast<mbedtls_ms_time_t>(tls_clock(tls_clock_user)):0;
}
namespace flightwall::net {
namespace {
std::atomic<unsigned> active{0};
static_assert(std::atomic<unsigned>::is_always_lock_free,"TLS ownership requires a lock-free word atomic");
bool host_valid(const char *s){
    if(!s)return false;
    size_t n=0;for(;s[n]&&n<254;++n){unsigned char c=static_cast<unsigned char>(s[n]);
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'))return false;
    }return n>0&&n<=253;
}
}
uint32_t Client::remaining() const{
    const uint64_t now=io_.now_ms(io_.user);
    return now>=deadline_?0:static_cast<uint32_t>(deadline_-now);
}
int Client::send_bytes(void *p,const unsigned char *bytes,size_t n){
    auto &c=*static_cast<Client *>(p);uint32_t left=c.remaining();
    if(!left)return MBEDTLS_ERR_SSL_TIMEOUT;
    int rc=c.io_.send(c.io_.user,bytes,n,left);
    if(rc==Again)return MBEDTLS_ERR_SSL_WANT_WRITE;
    if(rc<=0||static_cast<size_t>(rc)>n)return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return rc;
}
int Client::receive_bytes(void *p,unsigned char *bytes,size_t n){
    auto &c=*static_cast<Client *>(p);uint32_t left=c.remaining();
    if(!left)return MBEDTLS_ERR_SSL_TIMEOUT;
    int rc=c.io_.receive(c.io_.user,bytes,n,left);
    if(rc==Again)return MBEDTLS_ERR_SSL_WANT_READ;
    if(rc<0||static_cast<size_t>(rc)>n)return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return rc;
}
int Client::entropy(void *p,unsigned char *bytes,size_t n){
    auto &c=*static_cast<Client *>(p);
    return c.io_.random(c.io_.user,bytes,n)?0:MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}
bool Client::fail(Failure kind,int error){
    stats_.failure=kind;stats_.tls_error=error;
    if(handshake_)stats_.verify_flags=mbedtls_ssl_get_verify_result(&ssl_);
    if(stats_.verify_flags&&kind==Failure::Tls)stats_.failure=Failure::Verify;
    if(error==MBEDTLS_ERR_SSL_TIMEOUT)stats_.failure=Failure::Timeout;
    close();return false;
}
bool Client::open(const Transport &io,uint8_t *arena,size_t capacity,const char *host,
                  const uint8_t *ca,size_t ca_bytes,uint32_t timeout){
    if(owns_)return false; // Do not disturb a live request on an invalid reopen.
    stats_=Stats{};
    if(!io.send||!io.receive||!io.random||!io.now_ms||!io.pause_ms||!io.close||
       !arena||capacity<4096||capacity>128*1024||!host_valid(host)||!ca||!ca_bytes||ca_bytes>8192||
       timeout<100||timeout>60000){stats_.failure=Failure::Invalid;return false;}
    unsigned expected=0;
    if(!active.compare_exchange_strong(expected,1,std::memory_order_acquire)){stats_.failure=Failure::Busy;return false;}
    io_=io;owns_=true;stats_.arena_bytes=capacity;
    const uint64_t now=io_.now_ms(io_.user);
    if(now>static_cast<uint64_t>(INT64_MAX)-timeout){stats_.failure=Failure::Invalid;owns_=false;io_.close(io_.user);active.store(0,std::memory_order_release);return false;}
    deadline_=now+timeout;
    tls_clock=io_.now_ms;tls_clock_user=io_.user;
    mbedtls_memory_buffer_alloc_init(arena,capacity);
    mbedtls_ssl_init(&ssl_);mbedtls_ssl_config_init(&config_);
    mbedtls_ctr_drbg_init(&rng_);mbedtls_x509_crt_init(&ca_);initialized_=true;
    int rc=mbedtls_x509_crt_parse(&ca_,ca,ca_bytes);
    if(rc)return fail(Failure::Verify,rc);
    static const unsigned char personalization[]="FlightWall source TLS";
    rc=mbedtls_ctr_drbg_seed(&rng_,entropy,this,personalization,sizeof personalization-1);
    if(rc)return fail(Failure::Entropy,rc);
    rc=mbedtls_ssl_config_defaults(&config_,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
    if(rc)return fail(Failure::Tls,rc);
    static const int suites[]={MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,0};
    static const uint16_t groups[]={29,23,24,0}; // IANA: X25519, P-256, P-384.
    mbedtls_ssl_conf_ciphersuites(&config_,suites);mbedtls_ssl_conf_groups(&config_,groups);
    mbedtls_ssl_conf_min_tls_version(&config_,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&config_,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&config_,MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config_,&ca_,nullptr);
    mbedtls_ssl_conf_rng(&config_,mbedtls_ctr_drbg_random,&rng_);
    rc=mbedtls_ssl_setup(&ssl_,&config_);
    if(rc)return fail(Failure::Memory,rc);
    rc=mbedtls_ssl_set_hostname(&ssl_,host);
    if(rc)return fail(Failure::Memory,rc);
    mbedtls_ssl_set_bio(&ssl_,this,send_bytes,receive_bytes,nullptr);handshake_=true;
    do{
        if(!remaining())return fail(Failure::Timeout,MBEDTLS_ERR_SSL_TIMEOUT);
        rc=mbedtls_ssl_handshake(&ssl_);
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)io_.pause_ms(io_.user,1);
    }while(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE);
    if(rc)return fail(Failure::Tls,rc);
    if(!remaining())return fail(Failure::Timeout,MBEDTLS_ERR_SSL_TIMEOUT);
    if(mbedtls_ssl_get_verify_result(&ssl_))return fail(Failure::Verify,MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
    ready_=true;return true;
}
bool Client::write_all(const uint8_t *bytes,size_t n){
    if(!ready_||(!bytes&&n))return false;
    while(n){
        if(!remaining())return fail(Failure::Timeout,MBEDTLS_ERR_SSL_TIMEOUT);
        size_t take=n>FW_TLS_TX_BYTES?FW_TLS_TX_BYTES:n;
        int rc=mbedtls_ssl_write(&ssl_,bytes,take);
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE){io_.pause_ms(io_.user,1);continue;}
        if(rc<=0)return fail(Failure::Io,rc);
        bytes+=rc;n-=static_cast<size_t>(rc);
    }return true;
}
int Client::read(uint8_t *bytes,size_t n){
    if(!ready_||!bytes||!n||n>INT_MAX)return -1;
    int rc;
    do{
        if(!remaining()){fail(Failure::Timeout,MBEDTLS_ERR_SSL_TIMEOUT);return -1;}
        rc=mbedtls_ssl_read(&ssl_,bytes,n);
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)io_.pause_ms(io_.user,1);
    }while(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE);
    if(rc==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY||rc==0){close();return 0;}
    if(rc<0){fail(Failure::Io,rc);return -1;}
    return rc;
}
void Client::close(){
    if(!owns_)return;
    if(initialized_){
        mbedtls_ssl_free(&ssl_);mbedtls_ssl_config_free(&config_);
        mbedtls_x509_crt_free(&ca_);mbedtls_ctr_drbg_free(&rng_);
        mbedtls_memory_buffer_alloc_max_get(&stats_.peak_payload_bytes,&stats_.peak_blocks);
        mbedtls_memory_buffer_alloc_cur_get(&stats_.remaining_payload_bytes,&stats_.remaining_blocks);
        mbedtls_memory_buffer_alloc_free();
    }
    ready_=initialized_=handshake_=owns_=false;io_.close(io_.user);
    tls_clock=nullptr;tls_clock_user=nullptr;
    active.store(0,std::memory_order_release);
}
}
