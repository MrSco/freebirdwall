/* Desktop tests for components/fw_wifi (stock's saved network list).
 * Run: py tests/run_host_tests.py
 * Expected values are worked by hand from FUN_4202d6c4 / FUN_4202dbec /
 * FUN_4202d264 and ArduinoJson's TextFormatter (SETUP.md "Saved networks"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_nets.h"

static int checks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); \
    if (strcmp(a_, b_)) { fprintf(stderr, "FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, a_, b_); exit(1); } ++checks; } while (0)

static char store[2048];

static int parse(const char *json, FwNet out[FW_NETS_MAX]){
    return fw_nets_parse(json, out, store, sizeof store);
}

static void test_usable(void){
    CHECK(fw_nets_usable("a", "b", false));
    CHECK(fw_nets_usable("a", "", true));
    CHECK(!fw_nets_usable("a", "", false));
    CHECK(!fw_nets_usable("", "b", false));
    CHECK(!fw_nets_usable("", "", true));
    CHECK(!fw_nets_usable(NULL, "b", true));
    CHECK(fw_nets_usable("a", NULL, true));
}

static void test_json(void){
    char buf[1100];
    FwNet one[1] = {{"Home", "secret12", false}};
    FwNet two[2] = {{"Cafe", "", true}, {"Home", "secret12", false}};
    FwNet esc[1] = {{"a\"b\\c/d'e", "\b\f\n\r\t\x01\x7f\xc3\xa9", false}};
    CHECK(fw_nets_json(one, 1, buf, sizeof buf) == strlen(buf));
    CHECK_STR(buf, "[{\"ssid\":\"Home\",\"pass\":\"secret12\",\"open\":false}]");
    fw_nets_json(two, 2, buf, sizeof buf);
    CHECK_STR(buf, "[{\"ssid\":\"Cafe\",\"pass\":\"\",\"open\":true},{\"ssid\":\"Home\",\"pass\":\"secret12\",\"open\":false}]");
    fw_nets_json(esc, 1, buf, sizeof buf);
    /* '/' and '\'' stay as they are; control bytes other than the five named
     * escapes and all bytes >= 0x7f are written raw. */
    CHECK_STR(buf, "[{\"ssid\":\"a\\\"b\\\\c/d'e\",\"pass\":\"\\b\\f\\n\\r\\t\x01\x7f\xc3\xa9\",\"open\":false}]");
    CHECK(fw_nets_json(NULL, 0, buf, sizeof buf) == 2);
    CHECK_STR(buf, "[]");

    /* truncation: snprintf-style length, always terminated */
    {
        size_t full = fw_nets_json(one, 1, buf, sizeof buf);
        char small[10];
        CHECK(fw_nets_json(one, 1, small, sizeof small) == full);
        CHECK(strlen(small) == sizeof small - 1);
        CHECK(memcmp(small, buf, sizeof small - 1) == 0);
        CHECK(fw_nets_json(one, 1, small, 1) == full && small[0] == '\0');
        CHECK(fw_nets_json(one, 1, NULL, 0) == full);
    }

    /* the largest list stock would accept: 5 x (32-byte name, 63-byte password) */
    {
        char ssid[5][33], pass[5][64];
        FwNet five[5];
        for (int i = 0; i < 5; ++i) {
            memset(ssid[i], 'a' + i, 32); ssid[i][32] = 0;
            memset(pass[i], 'p', 63); pass[i][63] = 0;
            five[i].ssid = ssid[i]; five[i].pass = pass[i]; five[i].open = false;
        }
        /* each entry: {"ssid":"<32>","pass":"<63>","open":false} = 9+32+10+63+15 = 129 */
        CHECK(fw_nets_json(five, 5, buf, sizeof buf) == 2 + 5 * 129 + 4);
        CHECK(strlen(buf) <= FW_NETS_JSON_MAX);
        /* ... but a name of 32 double quotes doubles: still under 1024 */
        memset(ssid[0], '"', 32);
        CHECK(fw_nets_json(five, 5, buf, sizeof buf) == 2 + 5 * 129 + 4 + 32);
    }
}

static void test_parse(void){
    FwNet n[FW_NETS_MAX];
    char buf[1100];
    CHECK(parse("[{\"ssid\":\"Home\",\"pass\":\"secret12\",\"open\":false}]", n) == 1);
    CHECK_STR(n[0].ssid, "Home");
    CHECK_STR(n[0].pass, "secret12");
    CHECK(!n[0].open);

    /* open only counts as a boolean; strings only as strings */
    CHECK(parse("[{\"ssid\":\"A\",\"open\":true},{\"ssid\":\"B\",\"open\":1},{\"ssid\":\"C\",\"pass\":5,\"open\":true},"
                 "{\"ssid\":7,\"pass\":\"x\"},{\"pass\":\"x\"},{\"ssid\":\"\",\"pass\":\"x\"},{\"ssid\":\"D\",\"pass\":\"y\",\"open\":\"true\"}]", n) == 3);
    CHECK_STR(n[0].ssid, "A"); CHECK_STR(n[0].pass, ""); CHECK(n[0].open);
    CHECK_STR(n[1].ssid, "C"); CHECK_STR(n[1].pass, ""); CHECK(n[1].open);
    CHECK_STR(n[2].ssid, "D"); CHECK_STR(n[2].pass, "y"); CHECK(!n[2].open);

    /* not an array, broken, empty, non-object members */
    CHECK(parse("{\"ssid\":\"A\",\"pass\":\"b\"}", n) == 0);
    CHECK(parse("[{\"ssid\":\"A\",\"pass\":\"b\"}", n) == 0);
    CHECK(parse("", n) == 0);
    CHECK(parse("[]", n) == 0);
    CHECK(parse("[1,\"x\",null,[],{\"ssid\":\"A\",\"pass\":\"b\"}]", n) == 1);
    CHECK(parse(NULL, n) == 0);

    /* at most five usable entries; unusable ones do not count */
    CHECK(parse("[{\"ssid\":\"x\"},{\"ssid\":\"1\",\"pass\":\"p\"},{\"ssid\":\"2\",\"pass\":\"p\"},{\"ssid\":\"3\",\"pass\":\"p\"},"
                "{\"ssid\":\"4\",\"pass\":\"p\"},{\"ssid\":\"5\",\"pass\":\"p\"},{\"ssid\":\"6\",\"pass\":\"p\"}]", n) == 5);
    CHECK_STR(n[0].ssid, "1");
    CHECK_STR(n[4].ssid, "5");

    /* escapes round-trip */
    {
        FwNet e = {"a\"b\\c/d'e\t", "p\n\xc3\xa9", true};
        fw_nets_json(&e, 1, buf, sizeof buf);
        CHECK(parse(buf, n) == 1);
        CHECK_STR(n[0].ssid, e.ssid);
        CHECK_STR(n[0].pass, e.pass);
        CHECK(n[0].open);
    }
    /* keys are case-sensitive (duplicate keys never occur in what stock writes) */
    CHECK(parse("[{\"SSID\":\"Z\",\"ssid\":\"A\",\"pass\":\"p\"}]", n) == 1);
    CHECK_STR(n[0].ssid, "A");

    /* the store needs strlen(json)+1 at most */
    {
        const char *j = "[{\"ssid\":\"abc\",\"pass\":\"def\",\"open\":false}]";
        char tight[64];
        CHECK(strlen(j) + 1 <= sizeof tight);
        CHECK(fw_nets_parse(j, n, tight, strlen(j) + 1) == 1);
        CHECK_STR(n[0].ssid, "abc");
        CHECK_STR(n[0].pass, "def");
    }
}

static void test_legacy(void){
    FwNet n;
    CHECK(fw_nets_legacy("Home", "secret12", false, false, &n) == 1 && !n.open);
    CHECK_STR(n.ssid, "Home");
    CHECK_STR(n.pass, "secret12");
    CHECK(fw_nets_legacy("Home", "secret12", true, true, &n) == 1 && n.open);
    CHECK(fw_nets_legacy("Home", "secret12", true, false, &n) == 1 && !n.open);
    CHECK(fw_nets_legacy("Cafe", "", false, false, &n) == 1 && n.open);   /* no "open" key: open */
    CHECK(fw_nets_legacy("Cafe", "", true, true, &n) == 1 && n.open);
    CHECK(fw_nets_legacy("Cafe", "", true, false, &n) == 0);
    CHECK(fw_nets_legacy("", "x", true, true, &n) == 0);
    CHECK(fw_nets_legacy(NULL, "x", true, true, &n) == 0);
    CHECK(fw_nets_legacy("Cafe", NULL, false, false, &n) == 1 && n.open && !*n.pass);
}

static void test_add(void){
    FwNet list[5] = {{"A", "1", false}, {"B", "2", false}, {"C", "", true}, {"D", "4", false}, {"E", "5", false}};
    FwNet out[FW_NETS_MAX];
    FwNet x = {"X", "9", false}, c = {"C", "new", false}, bad = {"Y", "", false};
    char buf[1100];

    CHECK(fw_nets_add(NULL, 0, &x, out) == 1);
    CHECK_STR(out[0].ssid, "X");

    /* new first; the oldest drops off the end */
    CHECK(fw_nets_add(list, 5, &x, out) == 5);
    CHECK_STR(out[0].ssid, "X"); CHECK_STR(out[1].ssid, "A"); CHECK_STR(out[4].ssid, "D");

    /* an existing name moves to the front with the new password */
    CHECK(fw_nets_add(list, 5, &c, out) == 5);
    CHECK_STR(out[0].ssid, "C"); CHECK_STR(out[0].pass, "new"); CHECK(!out[0].open);
    CHECK_STR(out[1].ssid, "A"); CHECK_STR(out[2].ssid, "B"); CHECK_STR(out[3].ssid, "D"); CHECK_STR(out[4].ssid, "E");

    /* re-adding the newest leaves the list as it was */
    CHECK(fw_nets_add(list, 3, &list[0], out) == 3);
    CHECK_STR(out[0].ssid, "A"); CHECK_STR(out[1].ssid, "B"); CHECK_STR(out[2].ssid, "C");

    /* names compare exactly (case-sensitive) */
    {
        FwNet a = {"a", "z", false};
        CHECK(fw_nets_add(list, 2, &a, out) == 3);
    }

    CHECK(fw_nets_add(list, 5, &bad, out) == 0);
    CHECK(fw_nets_add(list, 5, NULL, out) == 0);

    /* legacy entry migrates into the list */
    {
        FwNet legacy;
        CHECK(fw_nets_legacy("Home", "secret12", false, false, &legacy) == 1);
        CHECK(fw_nets_add(&legacy, 1, &x, out) == 2);
        fw_nets_json(out, 2, buf, sizeof buf);
        CHECK_STR(buf, "[{\"ssid\":\"X\",\"pass\":\"9\",\"open\":false},{\"ssid\":\"Home\",\"pass\":\"secret12\",\"open\":false}]");
    }
}

int main(void){
    test_usable();
    test_json();
    test_parse();
    test_legacy();
    test_add();
    printf("PASS: %d nets checks\n", checks);
    return 0;
}
