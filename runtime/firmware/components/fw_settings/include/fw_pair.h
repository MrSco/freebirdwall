#ifndef FW_PAIR_H
#define FW_PAIR_H
/* Pairing a browser with the wall (SETTINGS.md "Signing in"): the dashboard
 * asks for a code, the wall shows six digits for two minutes, the person types
 * them in, and that browser gets a random key. The wall keeps only SHA-256
 * hashes of the keys (up to four browsers, the least recently used replaced).
 * Five wrong tries cancel a code. Portable C; the caller supplies randomness,
 * time and storage. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FW_PAIR_DEVICES 4
#define FW_PAIR_CODE_MS 120000
#define FW_PAIR_TRIES 5

/* What is stored (NVS fw_owned/pair). */
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint8_t hash[FW_PAIR_DEVICES][32];
    uint32_t used[FW_PAIR_DEVICES];      /* a counter, higher = more recent */
    uint32_t clock;
} FwPairStore;

typedef struct {
    FwPairStore store;
    char code[7];
    bool code_active;
    uint32_t code_until;
    int tries;
} FwPair;

enum { FW_PAIR_OK = 0, FW_PAIR_WRONG = -1, FW_PAIR_NO_CODE = -2 };

/* loaded may be NULL or invalid: then nobody is paired. */
void fw_pair_init(FwPair *p, const FwPairStore *loaded);

/* A new code from random (any 32 bits); returns it (6 digits). */
const char *fw_pair_start(FwPair *p, uint32_t now_ms, uint32_t random);

/* The code currently shown, or NULL; *seconds_left set when shown. */
const char *fw_pair_code(FwPair *p, uint32_t now_ms, int *seconds_left);

void fw_pair_cancel(FwPair *p);

/* Checks a typed code. On FW_PAIR_OK the new key (hex) is written to
 * key_hex[65] from key32 (32 random bytes) and the store changed: the caller
 * saves it. A wrong code counts a try; the fifth cancels the code. */
int fw_pair_finish(FwPair *p, const char *code, uint32_t now_ms, const uint8_t key32[32], char key_hex[65]);

/* True when key_hex belongs to a paired browser (marks it used: the caller
 * may save the store now and then). */
bool fw_pair_check(FwPair *p, const char *key_hex);

/* Forget every browser. */
void fw_pair_forget_all(FwPair *p);

#ifdef __cplusplus
}
#endif
#endif
