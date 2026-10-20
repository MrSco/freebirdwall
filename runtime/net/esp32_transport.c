/* Bounded socket transport for the source-built TLS component.
 * See esp32_transport.h for the contract. */
#include "esp32_transport.h"
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- platform */
#if defined(ESP_PLATFORM)
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <fcntl.h>
typedef int fw_sock_t;
#define FW_BAD_SOCK (-1)
#define FW_ADDRLEN(n) ((socklen_t)(n))
static int sock_errno(void){return errno;}
static bool sock_would_block(int e){return e==EAGAIN||e==EWOULDBLOCK||e==EINPROGRESS;}
static bool sock_nonblocking(fw_sock_t s){int f=fcntl(s,F_GETFL,0);return f>=0&&fcntl(s,F_SETFL,f|O_NONBLOCK)==0;}
static void sock_close(fw_sock_t s){close(s);}
static bool entropy_ready_flag=false;
static bool platform_random(uint8_t *bytes,size_t n){esp_fill_random(bytes,n);return true;}
static uint64_t platform_now_ms(void){return (uint64_t)esp_timer_get_time()/1000u;}
static void platform_pause_ms(unsigned ms){
    /* pdMS_TO_TICKS(1) is 0 at a 100 Hz tick; a zero delay would turn the TLS
     * client's 1 ms retry pause into a busy spin that starves lower tasks. */
    TickType_t ticks=pdMS_TO_TICKS(ms);
    vTaskDelay(ticks?ticks:1);
}
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
typedef SOCKET fw_sock_t;
#define FW_BAD_SOCK INVALID_SOCKET
#define FW_ADDRLEN(n) ((int)(n))
static int sock_errno(void){return WSAGetLastError();}
static bool sock_would_block(int e){return e==WSAEWOULDBLOCK||e==WSAEINPROGRESS;}
static bool sock_nonblocking(fw_sock_t s){u_long on=1;return ioctlsocket(s,FIONBIO,&on)==0;}
static void sock_close(fw_sock_t s){closesocket(s);}
static bool entropy_ready_flag=true;
static bool platform_random(uint8_t *bytes,size_t n){
    return BCryptGenRandom(NULL,bytes,(ULONG)n,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0;
}
static uint64_t platform_now_ms(void){return (uint64_t)GetTickCount64();}
static void platform_pause_ms(unsigned ms){Sleep(ms?ms:1);}
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/random.h>
typedef int fw_sock_t;
#define FW_BAD_SOCK (-1)
#define FW_ADDRLEN(n) ((socklen_t)(n))
static int sock_errno(void){return errno;}
static bool sock_would_block(int e){return e==EAGAIN||e==EWOULDBLOCK||e==EINPROGRESS;}
static bool sock_nonblocking(fw_sock_t s){int f=fcntl(s,F_GETFL,0);return f>=0&&fcntl(s,F_SETFL,f|O_NONBLOCK)==0;}
static void sock_close(fw_sock_t s){close(s);}
static bool entropy_ready_flag=true;
static bool platform_random(uint8_t *bytes,size_t n){
    size_t at=0;
    while(at<n){ssize_t got=getrandom(bytes+at,n-at,0);if(got<0&&errno==EINTR)continue;if(got<=0)return false;at+=(size_t)got;}
    return true;
}
static uint64_t platform_now_ms(void){
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000u+(uint64_t)ts.tv_nsec/1000000u;
}
static void platform_pause_ms(unsigned ms){struct timespec ts={ms/1000u,(long)(ms%1000u)*1000000L};nanosleep(&ts,NULL);}
#endif

/* The handle stores an int; Winsock SOCKETs are pointer-sized but in practice
 * small kernel handles. Refuse any socket value that does not round-trip. */
static fw_sock_t handle_sock(const FwEspTransport *t){return (fw_sock_t)t->fd;}
static bool store_sock(FwEspTransport *t,fw_sock_t s){
    t->fd=(int)s;
    return (fw_sock_t)t->fd==s&&t->fd>=0;
}

/* Waits up to wait_ms (capped to one slice) for readiness.
 * Returns 1 ready, 0 timed out, -1 error. */
static int wait_ready_checked(fw_sock_t s,bool for_write,uint32_t wait_ms){
    fd_set set,err;
    struct timeval tv;
    uint32_t ms=wait_ms<FW_ESP_IO_SLICE_MS?wait_ms:FW_ESP_IO_SLICE_MS;
    int rc;
    FD_ZERO(&set);FD_ZERO(&err);
    FD_SET(s,&set);FD_SET(s,&err);
    tv.tv_sec=(long)(ms/1000u);
    tv.tv_usec=(long)((ms%1000u)*1000u);
    rc=select((int)(s+1),for_write?NULL:&set,for_write?&set:NULL,&err,&tv);
    if(rc<0)return -1;
    if(rc==0)return 0;
    if(FD_ISSET(s,&set))return 1;
    return -1; /* only the exception set fired */
}

/* ------------------------------------------------------------------- API */
void fw_esp_entropy_ready(bool ready){entropy_ready_flag=ready;}

bool fw_esp_wall_clock_valid(void){
    /* 2024-01-01T00:00:00Z. Anything earlier means SNTP has not run yet, and
     * every certificate would look "not yet valid". */
    return time(NULL)>=(time_t)1704067200;
}

void fw_esp_transport_init(FwEspTransport *t){
    if(!t)return;
    memset(t,0,sizeof *t);
    t->fd=-1;
}

void fw_esp_transport_close(FwEspTransport *t){
    if(!t||t->fd<0)return;
    sock_close(handle_sock(t));
    t->fd=-1;
    ++t->closes;
}

static bool host_valid(const char *s,size_t max){
    size_t n=0;
    if(!s||!*s)return false;
    for(;s[n];++n){
        unsigned char c=(unsigned char)s[n];
        if(n>=max||c<=' '||c>=0x7f)return false;
    }
    return true;
}

/* Releases a socket from a failed connect attempt. Unlike close, this is not
 * a connection the caller ever owned, so it is not counted in closes. */
static void drop_socket(FwEspTransport *t){
    if(t->fd>=0){sock_close(handle_sock(t));t->fd=-1;}
}

static bool fail_connect(FwEspTransport *t,int reason){
    t->last_error=reason;
    t->last_errno=sock_errno();
    drop_socket(t);
    return false;
}

bool fw_esp_transport_connect(FwEspTransport *t,const char *host,const char *port,uint32_t timeout_ms){
    struct addrinfo hints,*list=NULL,*a;
    uint64_t start,end;
    int rc;
    if(!t)return false;
    if(t->fd>=0||!host_valid(host,253)||!host_valid(port,5)||!timeout_ms){
        t->last_error=FW_ESP_ERR_ARGS;return false;
    }
    if(!entropy_ready_flag){t->last_error=FW_ESP_ERR_ENTROPY;return false;}
    start=platform_now_ms();
    end=start+timeout_ms;

    memset(&hints,0,sizeof hints);
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;
    rc=getaddrinfo(host,port,&hints,&list);
    if(rc!=0||!list){
        if(list)freeaddrinfo(list);
        t->last_error=FW_ESP_ERR_DNS;t->last_errno=rc;return false;
    }
    if(platform_now_ms()>=end){freeaddrinfo(list);t->last_error=FW_ESP_ERR_TIMEOUT;return false;}

    t->last_error=FW_ESP_ERR_CONNECT;
    for(a=list;a;a=a->ai_next){
        fw_sock_t s;
        uint64_t now=platform_now_ms();
        if(now>=end){t->last_error=FW_ESP_ERR_TIMEOUT;break;}
        s=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if(s==FW_BAD_SOCK){t->last_error=FW_ESP_ERR_SOCKET;t->last_errno=sock_errno();continue;}
        if(!store_sock(t,s)){sock_close(s);t->fd=-1;t->last_error=FW_ESP_ERR_SOCKET;continue;}
        if(!sock_nonblocking(s)){fail_connect(t,FW_ESP_ERR_SOCKET);continue;}
        rc=connect(s,a->ai_addr,FW_ADDRLEN(a->ai_addrlen));
        if(rc!=0&&!sock_would_block(sock_errno())){fail_connect(t,FW_ESP_ERR_CONNECT);continue;}
        while(rc!=0){
            int ready,error=0;
            now=platform_now_ms();
            if(now>=end){fail_connect(t,FW_ESP_ERR_TIMEOUT);break;}
            ready=wait_ready_checked(s,true,(uint32_t)(end-now));
            if(ready==0)continue;
            if(ready<0){fail_connect(t,FW_ESP_ERR_CONNECT);break;}
            {
#if defined(_WIN32)
                int size=sizeof error;
                rc=getsockopt(s,SOL_SOCKET,SO_ERROR,(char *)&error,&size);
#else
                socklen_t size=sizeof error;
                rc=getsockopt(s,SOL_SOCKET,SO_ERROR,&error,&size);
#endif
            }
            if(rc==0&&error==0){rc=0;break;}
            t->last_errno=error;drop_socket(t);rc=-1;t->last_error=FW_ESP_ERR_CONNECT;break;
        }
        if(rc==0&&t->fd>=0){
            freeaddrinfo(list);
            t->last_error=FW_ESP_OK;t->last_errno=0;
            return true;
        }
        if(t->last_error==FW_ESP_ERR_TIMEOUT)break;
    }
    freeaddrinfo(list);
    drop_socket(t);
    return false;
}

int fw_esp_transport_send(void *user,const uint8_t *bytes,size_t n,uint32_t wait_ms){
    FwEspTransport *t=(FwEspTransport *)user;
    fw_sock_t s;
    int ready,rc;
    if(!t||t->fd<0)return -1;
    if(!n)return 0;
    s=handle_sock(t);
    ready=wait_ready_checked(s,true,wait_ms);
    if(ready==0){++t->again;return FW_TLS_AGAIN;}
    if(ready<0){t->last_errno=sock_errno();return -1;}
    if(n>0x7fffffffu)n=0x7fffffffu;
    rc=(int)send(s,(const char *)bytes,(int)n,0);
    if(rc<0){
        int e=sock_errno();
        if(sock_would_block(e)){++t->again;return FW_TLS_AGAIN;}
        t->last_errno=e;return -1;
    }
    if(rc==0){++t->again;return FW_TLS_AGAIN;} /* send never legitimately means EOF */
    t->bytes_sent+=(uint32_t)rc;
    return rc;
}

int fw_esp_transport_receive(void *user,uint8_t *bytes,size_t n,uint32_t wait_ms){
    FwEspTransport *t=(FwEspTransport *)user;
    fw_sock_t s;
    int ready,rc;
    if(!t||t->fd<0)return -1;
    if(!n)return FW_TLS_AGAIN;
    s=handle_sock(t);
    ready=wait_ready_checked(s,false,wait_ms);
    if(ready==0){++t->again;return FW_TLS_AGAIN;}
    if(ready<0){t->last_errno=sock_errno();return -1;}
    if(n>0x7fffffffu)n=0x7fffffffu;
    rc=(int)recv(s,(char *)bytes,(int)n,0);
    if(rc<0){
        int e=sock_errno();
        if(sock_would_block(e)){++t->again;return FW_TLS_AGAIN;}
        t->last_errno=e;return -1;
    }
    t->bytes_received+=(uint32_t)rc;
    return rc; /* 0 = orderly EOF */
}

bool fw_esp_transport_random(void *user,uint8_t *bytes,size_t n){
    (void)user;
    if(!entropy_ready_flag||(!bytes&&n))return false;
    return platform_random(bytes,n);
}

uint64_t fw_esp_transport_now_ms(void *user){(void)user;return platform_now_ms();}

void fw_esp_transport_pause_ms(void *user,unsigned ms){(void)user;platform_pause_ms(ms);}

void fw_esp_transport_close_cb(void *user){fw_esp_transport_close((FwEspTransport *)user);}

void fw_esp_transport_bind(FwEspTransport *t,FwTlsTransport *out){
    if(!out)return;
    out->user=t;
    out->send=fw_esp_transport_send;
    out->receive=fw_esp_transport_receive;
    out->random=fw_esp_transport_random;
    out->now_ms=fw_esp_transport_now_ms;
    out->pause_ms=fw_esp_transport_pause_ms;
    out->close=fw_esp_transport_close_cb;
}
