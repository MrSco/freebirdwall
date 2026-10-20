/* Desktop checks of esp32_transport.c behaviour the TLS suite cannot reach:
 * argument validation, the entropy gate, DNS failure, refused connects, the
 * per-call wait slice, EOF, and idempotent close. Uses a local TCP listener
 * only; no Internet access. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Sock=SOCKET;
static void close_sock(Sock s){closesocket(s);}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using Sock=int;
static void close_sock(Sock s){close(s);}
#endif
#include "esp32_transport.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static int checks=0;
#define CHECK(cond) do{ if(!(cond)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);return 1;} ++checks; }while(0)

/* Listening socket on 127.0.0.1:<ephemeral>; returns the port. */
static Sock listen_local(unsigned short *port){
    Sock s=socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
    if(bind(s,reinterpret_cast<sockaddr *>(&a),sizeof a)||listen(s,4))return s;
    socklen_t n=sizeof a;getsockname(s,reinterpret_cast<sockaddr *>(&a),&n);
    *port=ntohs(a.sin_port);return s;
}

int main(){
#ifdef _WIN32
    WSADATA wsa;if(WSAStartup(MAKEWORD(2,2),&wsa))return 2;
#endif
    FwEspTransport t;
    uint8_t buf[64];

    /* --- argument validation never opens a socket --- */
    fw_esp_transport_init(&t);
    CHECK(t.fd==-1);
    CHECK(!fw_esp_transport_connect(&t,nullptr,"443",1000)&&t.last_error==FW_ESP_ERR_ARGS);
    CHECK(!fw_esp_transport_connect(&t,"","443",1000)&&t.last_error==FW_ESP_ERR_ARGS);
    CHECK(!fw_esp_transport_connect(&t,"bad host","443",1000)&&t.last_error==FW_ESP_ERR_ARGS);
    CHECK(!fw_esp_transport_connect(&t,"bad\r\nhost","443",1000)&&t.last_error==FW_ESP_ERR_ARGS);
    CHECK(!fw_esp_transport_connect(&t,"127.0.0.1","443443",1000)&&t.last_error==FW_ESP_ERR_ARGS);
    CHECK(!fw_esp_transport_connect(&t,"127.0.0.1","443",0)&&t.last_error==FW_ESP_ERR_ARGS);
    CHECK(!fw_esp_transport_connect(&t,std::string(254,'a').c_str(),"443",1000)&&t.last_error==FW_ESP_ERR_ARGS);
    CHECK(!fw_esp_transport_connect(nullptr,"127.0.0.1","443",1000));
    CHECK(t.fd==-1&&t.closes==0);

    /* IO on a closed handle fails, never "again" (that would spin to the deadline). */
    CHECK(fw_esp_transport_send(&t,buf,4,10)==-1);
    CHECK(fw_esp_transport_receive(&t,buf,4,10)==-1);
    CHECK(fw_esp_transport_send(nullptr,buf,4,10)==-1);

    /* --- entropy gate: no handshake is possible before the radio is up --- */
    fw_esp_entropy_ready(false);
    CHECK(!fw_esp_transport_random(nullptr,buf,sizeof buf));
    CHECK(!fw_esp_transport_connect(&t,"127.0.0.1","9",1000)&&t.last_error==FW_ESP_ERR_ENTROPY);
    fw_esp_entropy_ready(true);
    std::memset(buf,0,sizeof buf);
    CHECK(fw_esp_transport_random(nullptr,buf,sizeof buf));
    {bool nonzero=false;for(uint8_t b:buf)nonzero|=b!=0;CHECK(nonzero);}
    CHECK(fw_esp_transport_random(nullptr,buf,0));
    CHECK(!fw_esp_transport_random(nullptr,nullptr,4));

    /* --- DNS failure (".invalid" is reserved and never resolves) --- */
    CHECK(!fw_esp_transport_connect(&t,"flightwall.invalid","443",5000));
    CHECK(t.last_error==FW_ESP_ERR_DNS||t.last_error==FW_ESP_ERR_TIMEOUT);
    CHECK(t.fd==-1&&t.closes==0);

    /* --- refused connect: closed local port, bounded, no leaked socket --- */
    {
        unsigned short port=0;Sock l=listen_local(&port);close_sock(l);
        uint64_t start=fw_esp_transport_now_ms(nullptr);
        CHECK(!fw_esp_transport_connect(&t,"127.0.0.1",std::to_string(port).c_str(),3000));
        uint64_t took=fw_esp_transport_now_ms(nullptr)-start;
        CHECK(t.last_error==FW_ESP_ERR_CONNECT||t.last_error==FW_ESP_ERR_TIMEOUT);
        CHECK(took<=3600);
        CHECK(t.fd==-1);
        CHECK(t.closes==0); /* a failed attempt is not a released connection */
    }

    /* --- connected IO, the wait slice, EOF and close --- */
    {
        unsigned short port=0;Sock l=listen_local(&port);
        CHECK(fw_esp_transport_connect(&t,"127.0.0.1",std::to_string(port).c_str(),3000));
        CHECK(t.fd>=0&&t.last_error==FW_ESP_OK);
        CHECK(!fw_esp_transport_connect(&t,"127.0.0.1",std::to_string(port).c_str(),3000)); /* already open */
        CHECK(t.last_error==FW_ESP_ERR_ARGS&&t.fd>=0);
        Sock peer=accept(l,nullptr,nullptr);

        /* Nothing pending: a huge budget must still return within one slice. */
        uint64_t start=fw_esp_transport_now_ms(nullptr);
        CHECK(fw_esp_transport_receive(&t,buf,sizeof buf,60000)==FW_TLS_AGAIN);
        uint64_t took=fw_esp_transport_now_ms(nullptr)-start;
        CHECK(took+20>=FW_ESP_IO_SLICE_MS&&took<=FW_ESP_IO_SLICE_MS+400);
        CHECK(t.again==1);

        /* A zero budget polls without blocking. */
        start=fw_esp_transport_now_ms(nullptr);
        CHECK(fw_esp_transport_receive(&t,buf,sizeof buf,0)==FW_TLS_AGAIN);
        CHECK(fw_esp_transport_now_ms(nullptr)-start<=100);

        const char hello[]="hello";
        CHECK(send(peer,hello,5,0)==5);
        int got=0;
        for(int i=0;i<20&&got<5;++i){
            int rc=fw_esp_transport_receive(&t,buf+got,sizeof buf-static_cast<size_t>(got),500);
            if(rc>0)got+=rc;else CHECK(rc==FW_TLS_AGAIN);
        }
        CHECK(got==5&&std::memcmp(buf,hello,5)==0&&t.bytes_received==5);

        CHECK(fw_esp_transport_send(&t,reinterpret_cast<const uint8_t *>("ping"),4,1000)==4);
        CHECK(t.bytes_sent==4);
        char echo[8]{};int r=recv(peer,echo,sizeof echo,0);
        CHECK(r==4&&std::memcmp(echo,"ping",4)==0);
        CHECK(fw_esp_transport_send(&t,buf,0,1000)==0);

        /* Orderly peer close is EOF (0), distinct from Again and error. */
        close_sock(peer);
        int rc=FW_TLS_AGAIN;
        for(int i=0;i<20&&rc==FW_TLS_AGAIN;++i)rc=fw_esp_transport_receive(&t,buf,sizeof buf,500);
        CHECK(rc==0);

        /* Bind wires every callback; close via callback, then idempotent. */
        FwTlsTransport io{};
        fw_esp_transport_bind(&t,&io);
        CHECK(io.user==&t&&io.send&&io.receive&&io.random&&io.now_ms&&io.pause_ms&&io.close);
        io.close(io.user);
        CHECK(t.fd==-1&&t.closes==1);
        io.close(io.user);
        fw_esp_transport_close(&t);
        CHECK(t.closes==1);
        CHECK(fw_esp_transport_receive(&t,buf,sizeof buf,10)==-1);
        close_sock(l);
    }

    /* --- clock and pause --- */
    {
        uint64_t a=fw_esp_transport_now_ms(nullptr);
        fw_esp_transport_pause_ms(nullptr,30);
        uint64_t b=fw_esp_transport_now_ms(nullptr);
        CHECK(b>=a+25);
        a=fw_esp_transport_now_ms(nullptr);
        fw_esp_transport_pause_ms(nullptr,0); /* must still yield, not spin forever */
        CHECK(fw_esp_transport_now_ms(nullptr)>=a);
        CHECK(fw_esp_wall_clock_valid());
    }

#ifdef _WIN32
    WSACleanup();
#endif
    std::printf("PASS: %d ESP transport checks\n",checks);
    return 0;
}
