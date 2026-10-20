/* Runs an image through the device's own Wi-Fi update validator
 * (shared/owner_ota.c), compiled for the desktop, exactly as the
 * board consumes an upload: streamed in chunks, transfer-SHA checked,
 * descriptor scanned across chunk boundaries, then end/select.
 *
 *   owner_ota_check <image.bin> <key-hex-64> <chunk-bytes>
 *
 * Exit 0 = the board's validator would accept and select this image.
 * Exit 1 = rejected; the reason code is printed. Exit 2 = usage/IO error.
 * The key is read from the command line only to compare; it is never printed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "owner_ota.h"
#include "owner_sha256.h"

#define SLOT_BYTES 0x2c0000u

typedef struct { uint32_t expected, written; unsigned begin, end, select, aborted; } Sink;

static int sink_begin(void *u, uint32_t n){ Sink *s=u; s->expected=n; ++s->begin; return 0; }
static int sink_write(void *u, const unsigned char *b, size_t n){
    Sink *s=u; (void)b;
    if (!s->begin || s->written+n>s->expected) return -1;
    s->written+=(uint32_t)n; return 0;
}
static int sink_end(void *u){ Sink *s=u; ++s->end; return s->written==s->expected ? 0 : -1; }
static void sink_abort(void *u){ Sink *s=u; ++s->aborted; }
static int sink_select(void *u){ Sink *s=u; ++s->select; return 0; }

static int hexval(char c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

static const char *reason(int r){
    switch (r) {
    case OO_OK: return "ok";
    case OO_INVALID: return "invalid";
    case OO_IO: return "io";
    case OO_HASH: return "transfer-hash";
    case OO_KEY: return "descriptor-missing-duplicate-or-wrong-key";
    case OO_STATE: return "state";
    case OO_IMAGE: return "image-header-or-sdk-verify";
    default: return "unknown";
    }
}

int main(int argc, char **argv){
    unsigned char key[32], digest[32];
    unsigned char *image;
    long size;
    size_t chunk, at;
    FILE *f;
    OwnerSha256 sha;
    OwnerOtaContext ctx;
    Sink sink={0};
    OwnerOtaOps ops={&sink,sink_begin,sink_write,sink_end,sink_abort,sink_select};
    int r;

    if (argc!=4 || strlen(argv[2])!=64) { fprintf(stderr,"usage: %s image key-hex chunk\n",argv[0]); return 2; }
    for (int i=0;i<32;++i) {
        int hi=hexval(argv[2][2*i]), lo=hexval(argv[2][2*i+1]);
        if (hi<0||lo<0) { fprintf(stderr,"bad key hex\n"); return 2; }
        key[i]=(unsigned char)(hi<<4|lo);
    }
    chunk=(size_t)strtoul(argv[3],NULL,10);
    if (!chunk) return 2;
    f=fopen(argv[1],"rb");
    if (!f) return 2;
    fseek(f,0,SEEK_END); size=ftell(f); fseek(f,0,SEEK_SET);
    if (size<=0 || (unsigned long)size>SLOT_BYTES) { fclose(f); fprintf(stderr,"size out of range\n"); return 2; }
    image=malloc((size_t)size);
    if (!image || fread(image,1,(size_t)size,f)!=(size_t)size) { fclose(f); return 2; }
    fclose(f);

    /* The uploader sends the SHA-256 of the exact file as X-Firmware-SHA256. */
    owner_sha256_init(&sha); owner_sha256_update(&sha,image,(size_t)size); owner_sha256_final(&sha,digest);

    r=owner_ota_start(&ctx,&ops,(uint32_t)size,digest,key,SLOT_BYTES);
    for (at=0; r==OO_OK && at<(size_t)size; at+=chunk) {
        size_t n=(size_t)size-at<chunk ? (size_t)size-at : chunk;
        r=owner_ota_feed(&ctx,image+at,n);
    }
    if (r==OO_OK) r=owner_ota_finish(&ctx);
    free(image);
    printf("{\"accepted\":%s,\"reason\":\"%s\",\"code\":%d,\"bytes\":%ld,\"chunk\":%u,"
           "\"written\":%u,\"selected\":%u,\"aborted\":%u}\n",
           r==OO_OK?"true":"false",reason(r),r,size,(unsigned)chunk,
           (unsigned)sink.written,sink.select,sink.aborted);
    return r==OO_OK ? 0 : 1;
}
