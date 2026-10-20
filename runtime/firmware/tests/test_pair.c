/* Desktop tests for components/fw_settings fw_pair (signing a browser in).
 * Run: py tests/run_host_tests.py */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_pair.h"

static int checks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)

static bool contains(const void *hay, size_t n, const char *needle, size_t m){
    const unsigned char *h = hay;
    for (size_t i = 0; i + m <= n; ++i)
        if (!memcmp(h + i, needle, m)) return true;
    return false;
}

static void key(uint8_t k[32], uint8_t seed){ for (int i = 0; i < 32; ++i) k[i] = (uint8_t)(seed * 31 + i); }

int main(void){
    FwPair p;
    uint8_t k[32];
    char hex[65], first[65];
    int left;
    const char *code;

    fw_pair_init(&p, NULL);
    CHECK(!fw_pair_code(&p, 0, NULL));
    CHECK(fw_pair_finish(&p, "123456", 0, k, hex) == FW_PAIR_NO_CODE);    /* nothing shown */

    code = fw_pair_start(&p, 1000, 4294967295u);
    CHECK(strlen(code) == 6 && !strcmp(code, "967295"));
    CHECK(!strcmp(fw_pair_start(&p, 1000, 42), "000042"));                 /* leading zeros */
    CHECK(fw_pair_code(&p, 1000, &left) && left == 120);
    CHECK(fw_pair_code(&p, 1000 + 119500, &left) && left == 1);
    CHECK(!fw_pair_code(&p, 1000 + 120000, NULL));                          /* expired */
    CHECK(fw_pair_finish(&p, "000042", 1000 + 120001, k, hex) == FW_PAIR_NO_CODE);

    /* wrong codes: the fifth cancels */
    fw_pair_start(&p, 0, 111111);
    for (int i = 0; i < 4; ++i) CHECK(fw_pair_finish(&p, "222222", 10, k, hex) == FW_PAIR_WRONG);
    CHECK(fw_pair_code(&p, 10, NULL));
    CHECK(fw_pair_finish(&p, "222222", 10, k, hex) == FW_PAIR_WRONG);
    CHECK(!fw_pair_code(&p, 10, NULL));
    CHECK(fw_pair_finish(&p, "111111", 10, k, hex) == FW_PAIR_NO_CODE);
    CHECK(fw_pair_finish(&p, NULL, 10, k, hex) == FW_PAIR_NO_CODE);

    /* the right code: a key, once */
    fw_pair_start(&p, 0, 333333);
    CHECK(fw_pair_finish(&p, "33333", 10, k, hex) == FW_PAIR_WRONG);       /* five digits */
    key(k, 1);
    CHECK(fw_pair_finish(&p, "333333", 10, k, hex) == FW_PAIR_OK);
    CHECK(strlen(hex) == 64 && p.store.count == 1);
    CHECK(fw_pair_finish(&p, "333333", 10, k, hex) == FW_PAIR_NO_CODE);    /* used up */
    strcpy(first, hex);
    CHECK(fw_pair_check(&p, first));
    {
        char other[65];
        strcpy(other, first);
        other[0] = other[0] == 'a' ? 'b' : 'a';
        CHECK(!fw_pair_check(&p, other));
        CHECK(!fw_pair_check(&p, "short"));
        CHECK(!fw_pair_check(&p, NULL));
        other[0] = 'G';
        CHECK(!fw_pair_check(&p, other));
    }
    /* the store keeps hashes, never the key */
    CHECK(!contains(&p.store, sizeof p.store, first, 64));

    /* survives a restart through the store */
    {
        FwPairStore saved = p.store;
        FwPair q;
        fw_pair_init(&q, &saved);
        CHECK(fw_pair_check(&q, first));
        saved.magic = 0;
        fw_pair_init(&q, &saved);
        CHECK(!fw_pair_check(&q, first) && q.store.count == 0);
    }

    /* four browsers; the fifth replaces the least recently used */
    {
        char keys[5][65];
        strcpy(keys[0], first);
        for (int i = 1; i < 4; ++i) {
            fw_pair_start(&p, 0, (uint32_t)i);
            key(k, (uint8_t)(i + 1));
            CHECK(fw_pair_finish(&p, code = p.code, 0, k, keys[i]) == FW_PAIR_OK);
        }
        CHECK(p.store.count == 4);
        CHECK(fw_pair_check(&p, keys[0]));          /* 0 used recently; 1 is now the oldest */
        fw_pair_start(&p, 0, 9);
        key(k, 9);
        CHECK(fw_pair_finish(&p, p.code, 0, k, keys[4]) == FW_PAIR_OK);
        CHECK(p.store.count == 4);
        CHECK(fw_pair_check(&p, keys[0]) && !fw_pair_check(&p, keys[1]) && fw_pair_check(&p, keys[2]) &&
              fw_pair_check(&p, keys[3]) && fw_pair_check(&p, keys[4]));
        fw_pair_forget_all(&p);
        for (int i = 0; i < 5; ++i) CHECK(!fw_pair_check(&p, keys[i]));
    }

    printf("PASS: %d pair checks\n", checks);
    return 0;
}
