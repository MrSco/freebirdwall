#include "owner_sha256.h"

static uint32_t rotate(uint32_t x, unsigned int n) { return (x >> n) | (x << (32u - n)); }
static uint32_t read_be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void transform(OwnerSha256 *s, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64], a,b,c,d,e,f,g,h,t1,t2;
    unsigned int i;
    for (i=0; i<16; ++i) w[i]=read_be(p+4u*i);
    for (i=16; i<64; ++i) {
        uint32_t x=w[i-15], y=w[i-2];
        w[i]=w[i-16]+(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-7]+(rotate(y,17)^rotate(y,19)^(y>>10));
    }
    a=s->state[0]; b=s->state[1]; c=s->state[2]; d=s->state[3];
    e=s->state[4]; f=s->state[5]; g=s->state[6]; h=s->state[7];
    for (i=0; i<64; ++i) {
        t1=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^((~e)&g))+k[i]+w[i];
        t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->state[0]+=a; s->state[1]+=b; s->state[2]+=c; s->state[3]+=d;
    s->state[4]+=e; s->state[5]+=f; s->state[6]+=g; s->state[7]+=h;
}
void owner_sha256_init(OwnerSha256 *s) {
    s->state[0]=0x6a09e667u; s->state[1]=0xbb67ae85u;
    s->state[2]=0x3c6ef372u; s->state[3]=0xa54ff53au;
    s->state[4]=0x510e527fu; s->state[5]=0x9b05688cu;
    s->state[6]=0x1f83d9abu; s->state[7]=0x5be0cd19u;
    s->bytes=0; s->used=0;
}
void owner_sha256_update(OwnerSha256 *s, const void *data, size_t length) {
    const unsigned char *p=(const unsigned char *)data;
    s->bytes+=(uint64_t)length;
    while (length) {
        size_t n=64u-s->used, i;
        if (n>length) n=length;
        for (i=0; i<n; ++i) s->block[s->used+i]=p[i];
        s->used+=(unsigned int)n; p+=n; length-=n;
        if (s->used==64u) { transform(s,s->block); s->used=0; }
    }
}
void owner_sha256_final(OwnerSha256 *s, unsigned char digest[32]) {
    uint64_t bits=s->bytes*8u;
    unsigned int i;
    s->block[s->used++]=0x80;
    if (s->used>56u) {
        while (s->used<64u) s->block[s->used++]=0;
        transform(s,s->block); s->used=0;
    }
    while (s->used<56u) s->block[s->used++]=0;
    for (i=0; i<8; ++i) s->block[63u-i]=(unsigned char)(bits>>(8u*i));
    transform(s,s->block); s->used=0;
    for (i=0; i<32; ++i) digest[i]=(unsigned char)(s->state[i/4u]>>(24u-8u*(i%4u)));
}
