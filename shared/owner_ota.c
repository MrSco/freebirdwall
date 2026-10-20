#include "owner_ota.h"

static int fail(OwnerOtaContext *c, int error) {
    if (c->begun) { c->ops.abort(c->ops.user); c->begun=0; }
    c->state=OO_FAILED; c->error=error;
    return error;
}
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static unsigned char ring(const OwnerOtaContext *c, unsigned int offset) {
    unsigned int i=c->descriptor_pos+offset;
    if (i>=OO_DESCRIPTOR_BYTES) i-=OO_DESCRIPTOR_BYTES;
    return c->descriptor[i];
}
static int scan(OwnerOtaContext *c, const unsigned char *data, size_t length) {
    static const unsigned char prefix[8]={'F','W','O','T','A','1',0,0};
    size_t i;
    for (i=0; i<length; ++i) {
        unsigned int j, different=0;
        c->descriptor[c->descriptor_pos++]=data[i];
        if (c->descriptor_pos==OO_DESCRIPTOR_BYTES) c->descriptor_pos=0;
        if (c->descriptor_used<OO_DESCRIPTOR_BYTES) ++c->descriptor_used;
        if (c->descriptor_used!=OO_DESCRIPTOR_BYTES) continue;
        for (j=0; j<8; ++j) if (ring(c,j)!=prefix[j]) break;
        if (j!=8 || ring(c,8)!=1 || ring(c,9)!=0 || ring(c,10)!=0 || ring(c,11)!=0) continue;
        if (++c->descriptors!=1) return OO_KEY;
        for (j=0; j<32; ++j) different|=(unsigned int)(ring(c,12u+j)^c->key[j]);
        if (different) return OO_KEY;
    }
    return OO_OK;
}
static int first_valid(const OwnerOtaContext *c) {
    const unsigned char *p=c->first;
    uint32_t segment=le32(p+28);
    return p[0]==0xe9 && p[1]>=1 && p[1]<=16 && p[12]==9 && p[13]==0 && p[23]==1 &&
           segment>=256u && segment<=c->expected_length-32u && le32(p+32)==0xabcd5432u;
}
int owner_ota_start(OwnerOtaContext *c, const OwnerOtaOps *ops, uint32_t length,
                    const unsigned char expected_sha256[32],
                    const unsigned char active_key[32], uint32_t partition_size) {
    size_t i;
    unsigned int nonzero=0;
    if (!c) return OO_INVALID;
    /* Explicit initialization also makes invalid starts safely cancelable. */
    for (i=0; i<sizeof(*c); ++i) ((unsigned char *)c)[i]=0;
    c->state=OO_FAILED; c->error=OO_INVALID;
    if (!ops || !ops->begin || !ops->write || !ops->end || !ops->abort || !ops->select ||
        !expected_sha256 || !active_key || length<OO_MIN_IMAGE || length>partition_size) return OO_INVALID;
    for (i=0; i<32; ++i) nonzero|=active_key[i];
    if (!nonzero) return OO_KEY;
    c->ops=*ops; c->expected_length=length;
    for (i=0; i<32; ++i) { c->expected_hash[i]=expected_sha256[i]; c->key[i]=active_key[i]; }
    owner_sha256_init(&c->hash); c->state=OO_RECEIVING; c->error=OO_OK;
    return OO_OK;
}
int owner_ota_feed(OwnerOtaContext *c, const unsigned char *data, size_t length) {
    size_t buffered=0, i;
    int result;
    if (!c || c->state!=OO_RECEIVING) return OO_STATE;
    if ((!data && length) || length>(size_t)(c->expected_length-c->received)) return fail(c,OO_INVALID);
    if (!length) return OO_OK;
    result=scan(c,data,length);
    if (result!=OO_OK) return fail(c,result);
    owner_sha256_update(&c->hash,data,length); c->received+=(uint32_t)length;
    if (c->first_used<OO_FIRST_BYTES) {
        buffered=OO_FIRST_BYTES-c->first_used;
        if (buffered>length) buffered=length;
        for (i=0; i<buffered; ++i) c->first[c->first_used+i]=data[i];
        c->first_used+=(unsigned int)buffered;
        if (c->first_used<OO_FIRST_BYTES) return OO_OK;
        if (!first_valid(c)) return fail(c,OO_IMAGE);
        /* begin may fail after allocating a handle: abort must be safe. */
        c->begun=1;
        if (c->ops.begin(c->ops.user,c->expected_length)!=0) return fail(c,OO_IO);
        if (c->ops.write(c->ops.user,c->first,OO_FIRST_BYTES)!=0) return fail(c,OO_IO);
    }
    if (length>buffered && c->ops.write(c->ops.user,data+buffered,length-buffered)!=0) return fail(c,OO_IO);
    return OO_OK;
}
int owner_ota_finish(OwnerOtaContext *c) {
    unsigned char digest[32];
    unsigned int i, different=0;
    if (!c || c->state!=OO_RECEIVING) return OO_STATE;
    if (c->received!=c->expected_length || !c->begun) return fail(c,OO_INVALID);
    if (c->descriptors!=1) return fail(c,OO_KEY);
    owner_sha256_final(&c->hash,digest);
    for (i=0; i<32; ++i) different|=(unsigned int)(digest[i]^c->expected_hash[i]);
    if (different) return fail(c,OO_HASH);
    if (c->ops.end(c->ops.user)!=0) return fail(c,OO_IMAGE);
    if (c->ops.select(c->ops.user)!=0) return fail(c,OO_IO);
    c->begun=0; c->state=OO_SELECTED; return OO_OK;
}
int owner_ota_cancel(OwnerOtaContext *c) {
    if (!c || c->state!=OO_RECEIVING) return OO_STATE;
    return fail(c,OO_STATE);
}
