/* Pairing a browser with the wall; see fw_pair.h. */
#include "fw_pair.h"
#include <stdio.h>
#include <string.h>
#include "owner_sha256.h"

#define MAGIC 0x31415046u   /* "FPA1" */

static void sha(const uint8_t *in, size_t n, uint8_t out[32]){
    OwnerSha256 h;
    owner_sha256_init(&h);
    owner_sha256_update(&h, in, n);
    owner_sha256_final(&h, out);
}

void fw_pair_init(FwPair *p, const FwPairStore *loaded){
    memset(p, 0, sizeof *p);
    if (loaded && loaded->magic == MAGIC && loaded->count <= FW_PAIR_DEVICES) p->store = *loaded;
    p->store.magic = MAGIC;
}

const char *fw_pair_start(FwPair *p, uint32_t now, uint32_t random){
    snprintf(p->code, sizeof p->code, "%06u", (unsigned)(random % 1000000u));
    p->code_active = true;
    p->code_until = now + FW_PAIR_CODE_MS;
    p->tries = 0;
    return p->code;
}

const char *fw_pair_code(FwPair *p, uint32_t now, int *seconds_left){
    if (!p->code_active) return NULL;
    if ((int32_t)(p->code_until - now) <= 0) { fw_pair_cancel(p); return NULL; }
    if (seconds_left) *seconds_left = (int)((p->code_until - now + 999) / 1000);
    return p->code;
}

void fw_pair_cancel(FwPair *p){
    p->code_active = false;
    memset(p->code, 0, sizeof p->code);
}

static bool same_code(const char *a, const char *b){
    unsigned diff = 0;
    if (strlen(a) != 6 || strlen(b) != 6) return false;
    for (int i = 0; i < 6; ++i) diff |= (unsigned)(a[i] ^ b[i]);
    return !diff;
}

int fw_pair_finish(FwPair *p, const char *code, uint32_t now, const uint8_t key32[32], char key_hex[65]){
    static const char hex[] = "0123456789abcdef";
    int slot = 0;
    if (!fw_pair_code(p, now, NULL)) return FW_PAIR_NO_CODE;
    if (!code || !same_code(code, p->code)) {
        if (++p->tries >= FW_PAIR_TRIES) fw_pair_cancel(p);
        return FW_PAIR_WRONG;
    }
    fw_pair_cancel(p);
    for (int i = 0; i < 32; ++i) {
        key_hex[2 * i] = hex[key32[i] >> 4];
        key_hex[2 * i + 1] = hex[key32[i] & 15];
    }
    key_hex[64] = 0;
    if (p->store.count < FW_PAIR_DEVICES) {
        slot = (int)p->store.count++;
    } else {
        for (int i = 1; i < FW_PAIR_DEVICES; ++i)          /* replace the least recently used */
            if (p->store.used[i] < p->store.used[slot]) slot = i;
    }
    sha((const uint8_t *)key_hex, 64, p->store.hash[slot]);
    p->store.used[slot] = ++p->store.clock;
    return FW_PAIR_OK;
}

bool fw_pair_check(FwPair *p, const char *key_hex){
    uint8_t h[32];
    bool found = false;
    if (!key_hex || strlen(key_hex) != 64) return false;
    for (int i = 0; i < 64; ++i) {
        char c = key_hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    sha((const uint8_t *)key_hex, 64, h);
    for (uint32_t i = 0; i < p->store.count; ++i) {
        unsigned diff = 0;
        for (int k = 0; k < 32; ++k) diff |= (unsigned)(h[k] ^ p->store.hash[i][k]);
        if (!diff) { found = true; p->store.used[i] = ++p->store.clock; }
    }
    return found;
}

void fw_pair_forget_all(FwPair *p){
    memset(&p->store, 0, sizeof p->store);
    p->store.magic = MAGIC;
    fw_pair_cancel(p);
}
