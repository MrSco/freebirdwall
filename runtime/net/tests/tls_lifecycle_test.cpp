#include "tls_client.h"
#include "../../../shared/owner_route_root.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace flightwall::net;
struct Fake {uint64_t clock=0;unsigned closes=0;bool rng=true,disconnect=false,nested=false;Client *closing=nullptr;};
static uint8_t other_arena[65536];
static int send(void *,const uint8_t *,size_t n,uint32_t left){assert(left);return static_cast<int>(n);}
static int receive(void *p,uint8_t *,size_t,uint32_t left){assert(left);return static_cast<Fake *>(p)->disconnect?0:Again;}
static uint64_t clock_ms(void *p){return static_cast<Fake *>(p)->clock;}
static void pause(void *p,unsigned n){static_cast<Fake *>(p)->clock+=n;}
static void close(void *p){auto &f=*static_cast<Fake *>(p);++f.closes;if(f.closing)f.closing->close();}
static Transport ops(Fake &f);
static bool random(void *p,uint8_t *out,size_t n){auto &f=*static_cast<Fake *>(p);
    if(f.nested){f.nested=false;Client other;auto io=ops(f);unsigned previous=f.closes;
        assert(!other.open(io,other_arena,sizeof other_arena,"example.test",reinterpret_cast<const uint8_t *>(owner_route_root),sizeof owner_route_root,100));
        assert(other.stats().failure==Failure::Busy&&f.closes==previous);
    }
    std::memset(out,0x37,n);return f.rng;
}
static Transport ops(Fake &f){return {&f,send,receive,random,clock_ms,pause,close};}
int main(){
    uint8_t arena[65536];Client c;Fake f;auto io=ops(f);const auto *root=reinterpret_cast<const uint8_t *>(owner_route_root);
    assert(!c.open(io,arena,sizeof arena,"bad/host",root,sizeof owner_route_root,100));
    assert(c.stats().failure==Failure::Invalid&&!f.closes);
    assert(!c.open(io,arena,sizeof arena,"example.test",nullptr,0,100)&&!f.closes);
    Fake overflow;overflow.clock=UINT64_MAX-50;auto overflow_io=ops(overflow);
    assert(!c.open(overflow_io,arena,sizeof arena,"example.test",root,sizeof owner_route_root,100));
    assert(c.stats().failure==Failure::Invalid&&overflow.closes==1);
    uint8_t invalid[]={1,2,3,0};assert(!c.open(io,arena,sizeof arena,"example.test",invalid,sizeof invalid,100));
    assert(c.stats().failure==Failure::Verify&&c.stats().remaining_blocks==0&&f.closes==1);
    f.rng=false;assert(!c.open(io,arena,sizeof arena,"example.test",root,sizeof owner_route_root,100));
    assert(c.stats().failure==Failure::Entropy&&c.stats().remaining_blocks==0&&f.closes==2);
    f.rng=true;f.nested=true;
    assert(!c.open(io,arena,sizeof arena,"example.test",root,sizeof owner_route_root,100));
    assert(c.stats().failure==Failure::Timeout&&c.stats().remaining_blocks==0&&f.closes==3);
    assert(!c.open(io,arena,4096,"example.test",root,sizeof owner_route_root,100));
    assert(c.stats().remaining_blocks==0&&f.closes==4);
    f.disconnect=true;f.closing=&c;
    for(unsigned i=0;i<50;++i){
        assert(!c.open(io,arena,sizeof arena,"example.test",root,sizeof owner_route_root,100));
        assert(c.stats().remaining_blocks==0&&c.stats().remaining_payload_bytes==0);
    }
    unsigned closes=f.closes;c.close();c.close();assert(f.closes==closes);
    std::puts("TLS lifecycle: invalid input, trust/RNG failures, exclusive arena, timeout, OOM, disconnect/reconnect cleanup passed");
}
