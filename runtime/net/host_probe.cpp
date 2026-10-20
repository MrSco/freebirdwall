/* Developer-only host adapter. Not a device server or runtime dependency. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
using Socket=SOCKET;
constexpr Socket BadSocket=INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <sys/random.h>
using Socket=int;
constexpr Socket BadSocket=-1;
#endif
#include "tls_client.h"
#include "../esp32/http_body.h"
#include "mbedtls/sha256.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>
using namespace flightwall::net;
namespace {
#ifdef FW_PROBE_ESP_TRANSPORT
/* Same CLI and fault options, but every socket/RNG/clock operation goes through
 * the ESP32 transport adapter (esp32_transport.c) compiled for the desktop.
 * The decorators below only inject faults; all real IO is the adapter's. */
}
#include "esp32_transport.h"
namespace {
uint64_t now(void *){return fw_esp_transport_now_ms(nullptr);}
void pause(void *,unsigned ms){fw_esp_transport_pause_ms(nullptr,ms);}
struct Host {FwEspTransport esp{};unsigned closes=0,operations=0,fail_after=0;size_t chunk=16384;bool bad_rng=false,stall=false;};
void close_host(void *p){auto &h=*static_cast<Host *>(p);fw_esp_transport_close(&h.esp);h.closes=h.esp.closes;}
bool connect_host(Host &h,const char *host,const char *port,uint32_t timeout){
    fw_esp_transport_init(&h.esp);
    bool ok=fw_esp_transport_connect(&h.esp,host,port,timeout);
    h.closes=h.esp.closes;
    return ok;
}
int send_host(void *p,const uint8_t *bytes,size_t n,uint32_t ms){
    auto &h=*static_cast<Host *>(p);
    if(h.fail_after&&++h.operations>=h.fail_after)return -1;
    return fw_esp_transport_send(&h.esp,bytes,std::min(n,h.chunk),ms);
}
int receive_host(void *p,uint8_t *bytes,size_t n,uint32_t ms){
    auto &h=*static_cast<Host *>(p);
    if(h.fail_after&&++h.operations>=h.fail_after)return -1;
    if(h.stall)return Again;
    return fw_esp_transport_receive(&h.esp,bytes,std::min(n,h.chunk),ms);
}
bool random_host(void *p,uint8_t *bytes,size_t n){
    if(static_cast<Host *>(p)->bad_rng)return false;
    return fw_esp_transport_random(nullptr,bytes,n);
}
static_assert(FW_TLS_AGAIN==Again,"adapter and client must agree on Again");
#else
uint64_t now(void *){return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void pause(void *,unsigned ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
struct Host {Socket fd=BadSocket;unsigned closes=0,operations=0,fail_after=0;size_t chunk=16384;bool bad_rng=false,stall=false;};
void close_host(void *p){auto &h=*static_cast<Host *>(p);if(h.fd!=BadSocket){
#ifdef _WIN32
    closesocket(h.fd);
#else
    ::close(h.fd);
#endif
    h.fd=BadSocket;++h.closes;
}}
int wait(Socket s,bool write,uint32_t ms){
    fd_set set;FD_ZERO(&set);FD_SET(s,&set);ms=std::min(ms,1000u);
    timeval tv{static_cast<long>(ms/1000),static_cast<long>((ms%1000)*1000)};
    return select(static_cast<int>(s+1),write?nullptr:&set,write?&set:nullptr,nullptr,&tv);
}
bool would_block(){
#ifdef _WIN32
    int error=WSAGetLastError();return error==WSAEWOULDBLOCK||error==WSAEINPROGRESS;
#else
    return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINPROGRESS;
#endif
}
bool connect_host(Host &h,const char *host,const char *port,uint32_t timeout){
    addrinfo hints{},*addresses=nullptr;hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_UNSPEC;
    // OS DNS resolution is outside the TLS engine's request deadline.
    if(getaddrinfo(host,port,&hints,&addresses))return false;
    uint64_t end=now(nullptr)+timeout;
    for(auto *a=addresses;a&&now(nullptr)<end;a=a->ai_next){
        h.fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);if(h.fd==BadSocket)continue;
#ifdef _WIN32
        u_long nonblocking=1;bool configured=ioctlsocket(h.fd,FIONBIO,&nonblocking)==0;
#else
        bool configured=fcntl(h.fd,F_SETFL,O_NONBLOCK)==0;
#endif
        int rc=configured?connect(h.fd,a->ai_addr,static_cast<int>(a->ai_addrlen)):-1;
        if(configured&&(rc==0||would_block())){
            while(rc!=0&&now(nullptr)<end){
                int ready=wait(h.fd,true,static_cast<uint32_t>(end-now(nullptr)));
                if(ready<0)break;
                if(!ready)continue;
                int error=0;
#ifdef _WIN32
                int size=sizeof error;
                rc=getsockopt(h.fd,SOL_SOCKET,SO_ERROR,reinterpret_cast<char *>(&error),&size);
#else
                socklen_t size=sizeof error;rc=getsockopt(h.fd,SOL_SOCKET,SO_ERROR,&error,&size);
#endif
                if(rc==0&&error==0){freeaddrinfo(addresses);return true;}rc=-1;break;
            }
            if(rc==0){freeaddrinfo(addresses);return true;}
        }
        close_host(&h);h.closes=0;
    }
    freeaddrinfo(addresses);return false;
}
int send_host(void *p,const uint8_t *bytes,size_t n,uint32_t ms){
    auto &h=*static_cast<Host *>(p);
    if(h.fail_after&&++h.operations>=h.fail_after)return -1;
    int ready=wait(h.fd,true,ms);if(!ready)return Again;if(ready<0)return -1;
    int rc=static_cast<int>(send(h.fd,reinterpret_cast<const char *>(bytes),static_cast<int>(std::min(n,h.chunk)),0));
    return rc<0&&would_block()?Again:rc;
}
int receive_host(void *p,uint8_t *bytes,size_t n,uint32_t ms){
    auto &h=*static_cast<Host *>(p);
    if(h.fail_after&&++h.operations>=h.fail_after)return -1;
    if(h.stall)return Again;
    int ready=wait(h.fd,false,ms);if(!ready)return Again;if(ready<0)return -1;
    int rc=static_cast<int>(recv(h.fd,reinterpret_cast<char *>(bytes),static_cast<int>(std::min(n,h.chunk)),0));
    return rc<0&&would_block()?Again:rc;
}
bool random_host(void *p,uint8_t *bytes,size_t n){
    if(static_cast<Host *>(p)->bad_rng)return false;
#ifdef _WIN32
    return BCryptGenRandom(nullptr,bytes,static_cast<ULONG>(n),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0;
#else
    size_t at=0;while(at<n){ssize_t got=getrandom(bytes+at,n-at,0);if(got<0&&errno==EINTR)continue;if(got<=0)return false;at+=static_cast<size_t>(got);}return true;
#endif
}
#endif
struct Reader {
    Client *client;
    static int next(void *p,uint8_t *bytes,size_t n){return static_cast<Reader *>(p)->client->read(bytes,n);}
};
}
int main(int argc,char **argv){
#ifdef _WIN32
    WSADATA wsa;if(WSAStartup(MAKEWORD(2,2),&wsa))return 2;
#endif
    std::string host="api.adsbdb.com",port="443",path="/v0/callsign/DAL42",ca_path,server_name;
    unsigned rounds=1,arena_bytes=65536,timeout=15000;Host options;
    for(int i=1;i<argc;++i){std::string a=argv[i];
        if(a=="--bad-rng"){options.bad_rng=true;continue;}
        if(a=="--stall"){options.stall=true;continue;}
        if(i+1>=argc)return 2;
        const char *value=argv[++i];
        if(a=="--host")host=value;else if(a=="--port")port=value;else if(a=="--path")path=value;
        else if(a=="--ca")ca_path=value;else if(a=="--server-name")server_name=value;
        else if(a=="--rounds")rounds=static_cast<unsigned>(std::strtoul(value,nullptr,10));
        else if(a=="--arena-bytes")arena_bytes=static_cast<unsigned>(std::strtoul(value,nullptr,10));
        else if(a=="--timeout-ms")timeout=static_cast<unsigned>(std::strtoul(value,nullptr,10));
        else if(a=="--io-chunk")options.chunk=std::strtoul(value,nullptr,10);
        else if(a=="--fail-after")options.fail_after=static_cast<unsigned>(std::strtoul(value,nullptr,10));
        else return 2;
    }
    if(ca_path.empty()||!rounds||rounds>100||arena_bytes<4096||arena_bytes>128*1024||
       !options.chunk||options.chunk>16384||host.find_first_of("\r\n ")!=std::string::npos||path.empty()||path.front()!='/'||path.size()>512||path.find_first_of("\r\n ")!=std::string::npos)return 2;
    std::ifstream file(ca_path,std::ios::binary);std::vector<uint8_t> ca((std::istreambuf_iterator<char>(file)),{});
    if(ca.empty()||ca.size()>8191)return 2;ca.push_back(0);
    std::vector<uint8_t> arena(arena_bytes);bool all=true;
    for(unsigned round=0;round<rounds;++round){
        Host io=options;Client client;unsigned status=0;size_t body_bytes=0;bool ok=false;
        unsigned char digest[32]{};mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);mbedtls_sha256_starts(&hash,0);
        if(connect_host(io,host.c_str(),port.c_str(),timeout)){
            Transport transport{&io,send_host,receive_host,random_host,now,pause,close_host};
            if(client.open(transport,arena.data(),arena.size(),server_name.empty()?host.c_str():server_name.c_str(),ca.data(),ca.size(),timeout)){
                const std::string request="GET "+path+" HTTP/1.1\r\nHost: "+host+"\r\nUser-Agent: FlightWall-Source/0.1\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
                if(client.write_all(reinterpret_cast<const uint8_t *>(request.data()),request.size())){
                    Reader reader{&client};flightwall::HttpBody body({&reader,Reader::next});
                    if(body.headers()){
                        status=body.status();int c;
                        while((c=body.read())>=0){uint8_t byte=static_cast<uint8_t>(c);mbedtls_sha256_update(&hash,&byte,1);++body_bytes;}
                        ok=c==-1&&client.stats().failure==Failure::None;
                    }
                }
            }
        }
        client.close();close_host(&io);mbedtls_sha256_finish(&hash,digest);mbedtls_sha256_free(&hash);
        const auto &s=client.stats();all=all&&ok&&s.remaining_blocks==0;
        std::printf("{\"ok\":%s,\"round\":%u,\"status\":%u,\"body_bytes\":%zu,\"failure\":%u,\"tls_error\":%d,\"verify_flags\":%u,\"arena_bytes\":%zu,\"peak_payload_bytes\":%zu,\"peak_blocks\":%zu,\"remaining_blocks\":%zu,\"remaining_payload_bytes\":%zu,\"close_count\":%u,\"client_bytes\":%zu,\"reader_bytes\":%zu,\"tx_bytes\":%u,\"rx_bytes\":%u,\"body_sha256\":\"",
            ok?"true":"false",round,status,body_bytes,static_cast<unsigned>(s.failure),s.tls_error,s.verify_flags,s.arena_bytes,s.peak_payload_bytes,s.peak_blocks,s.remaining_blocks,s.remaining_payload_bytes,io.closes,sizeof(Client),sizeof(flightwall::HttpBody),FW_TLS_TX_BYTES,MBEDTLS_SSL_IN_CONTENT_LEN);
        for(unsigned char byte:digest)std::printf("%02x",byte);std::puts("\"}");
        std::fflush(stdout);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return all?0:1;
}
