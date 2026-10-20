/* Desktop tests for components/fw_wifi fw_link (Wi-Fi failure and reconnect).
 * Run: py tests/run_host_tests.py
 * Timings and texts are stock's (SETUP.md "Wi-Fi failure and reconnect"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_link.h"

static int checks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); \
    if (!a_ || strcmp(a_, b_)) { fprintf(stderr, "FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, a_ ? a_ : "(null)", b_); exit(1); } ++checks; } while (0)

#define TICK 100

static FwLinkInput input(uint32_t now, bool connected){
    FwLinkInput in;
    memset(&in, 0, sizeof in);
    in.now = now;
    in.connected = connected;
    return in;
}

/* Ticks from..to (exclusive) with a fixed radio state; ORs the actions and
 * records when the first of `watch` happened. */
static unsigned run(FwLink *l, uint32_t from, uint32_t to, bool connected, unsigned watch, uint32_t *first){
    unsigned all = 0;
    if (first) *first = 0;
    for (uint32_t t = from; t < to; t += TICK) {
        FwLinkInput in = input(t, connected);
        unsigned a = fw_link_tick(l, &in);
        if ((a & watch) && first && !*first) *first = t;
        all |= a;
    }
    return all;
}

static void test_backoff(void){
    for (int n = 0; n <= 5; ++n) CHECK(fw_link_backoff(n, false) == 5000);
    CHECK(fw_link_backoff(6, false) == 10000);
    CHECK(fw_link_backoff(7, false) == 20000);
    CHECK(fw_link_backoff(8, false) == 30000);
    CHECK(fw_link_backoff(40, false) == 30000);
    CHECK(fw_link_backoff(0, true) == 60000 && fw_link_backoff(9, true) == 60000);
    CHECK(fw_link_phone_joined(100000, 1000, 50000, true) == 1000);     /* back within 59999 ms */
    CHECK(fw_link_phone_joined(109999, 1000, 50000, true) == 1000);
    CHECK(fw_link_phone_joined(110000, 1000, 50000, true) == 110000);
    CHECK(fw_link_phone_joined(5000, 1000, 2000, false) == 5000);
}

static void test_boot_ok(void){
    FwLink l;
    FwLinkInput in;
    CHECK(fw_link_start(&l, 0, 1) == FW_LINK_DO_ATTEMPT);
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_SPLASH);
    CHECK_STR(fw_link_view(&l)->line[0], "Connecting...");
    CHECK(run(&l, 100, 5000, false, 0, NULL) == 0);
    in = input(5000, true);
    CHECK(fw_link_tick(&l, &in) == 0);
    CHECK(l.mode == FW_LINK_ONLINE);
    CHECK_STR(fw_link_view(&l)->line[0], "Connected");
    run(&l, 5100, 6000, true, 0, NULL);
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_SPLASH);
    run(&l, 6000, 6100, true, 0, NULL);
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_NORMAL);
    /* no setup mode, no access point */
    CHECK(!l.ap_on && !l.setup);
    /* a short drop (under 2 s) is not a loss */
    CHECK(run(&l, 7000, 8900, false, 0, NULL) == 0);
    CHECK(l.mode == FW_LINK_ONLINE);
    run(&l, 9000, 9100, true, 0, NULL);
    CHECK(l.mode == FW_LINK_ONLINE && !l.losing);
}

static void test_boot_fails(void){
    FwLink l;
    uint32_t at;
    unsigned a;
    fw_link_start(&l, 0, 2);
    /* the attempt runs a full 20 s ( > 20000 ms) */
    CHECK(run(&l, 100, 20100, false, 0, NULL) == 0);
    a = run(&l, 20100, 20200, false, FW_LINK_DO_ABORT, &at);
    CHECK(a == FW_LINK_DO_ABORT && at == 20100);
    CHECK_STR(fw_link_view(&l)->line[0], "Connection failed");
    CHECK(fw_link_view(&l)->color == FW_LINK_ORANGE);
    /* held 2 s, then setup mode and an immediate retry */
    CHECK(run(&l, 20200, 22100, false, 0, NULL) == 0);
    a = run(&l, 22100, 22200, false, FW_LINK_DO_SETUP_START, &at);
    CHECK(a == FW_LINK_DO_SETUP_START && at == 22100);
    CHECK(l.mode == FW_LINK_RECONNECTING && l.setup && l.ap_on);
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_SETUP && fw_link_view(&l)->reconnecting);
    a = run(&l, 22200, 22300, false, FW_LINK_DO_ATTEMPT, &at);
    CHECK(a == FW_LINK_DO_ATTEMPT && at == 22200);
    /* then one attempt a minute, counted from the start of the last one */
    {
        uint32_t starts[6], n = 0;
        for (uint32_t t = 22300; t < 22300 + 300000; t += TICK) {
            FwLinkInput in = input(t, false);
            if ((fw_link_tick(&l, &in) & FW_LINK_DO_ATTEMPT) && n < 6) starts[n++] = t;
        }
        CHECK(n == 5);
        CHECK(starts[0] == 82200 && starts[1] == 142200 && starts[4] == 322200);
    }
    /* no loss messages in setup mode */
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_SETUP);
}

static void test_reconnect_from_setup(void){
    FwLink l;
    uint32_t at;
    unsigned a;
    fw_link_start(&l, 0, 1);
    run(&l, 100, 22300, false, 0, NULL);                 /* failed boot, setup on, retry started at 22200 */
    CHECK(l.attempting);
    /* the retry connects */
    a = run(&l, 25000, 25100, true, 0, NULL);
    CHECK(a == 0 && !l.attempting);
    CHECK_STR(fw_link_view(&l)->line[0], "Checking network...");
    /* the network check goes out once connected for more than 2999 ms */
    a = run(&l, 25100, 28000, true, FW_LINK_DO_DNS_CHECK, &at);
    CHECK(a == 0);
    a = run(&l, 28000, 28100, true, FW_LINK_DO_DNS_CHECK, &at);
    CHECK(a == FW_LINK_DO_DNS_CHECK && at == 28000);
    {
        FwLinkInput in = input(28100, true);
        in.dns = 1;
        CHECK(fw_link_tick(&l, &in) == 0);
    }
    CHECK(l.mode == FW_LINK_ONLINE);
    CHECK_STR(fw_link_view(&l)->line[0], "Reconnected");
    CHECK(fw_link_view(&l)->clear && !l.reconnect_flag);
    /* owned: the access point goes off once no phone has been on it for 4999 ms */
    a = run(&l, 28200, 33100, true, FW_LINK_DO_SETUP_STOP, &at);
    CHECK(a == 0);
    a = run(&l, 33100, 33300, true, FW_LINK_DO_SETUP_STOP, &at);
    CHECK(a == FW_LINK_DO_SETUP_STOP && at == 33200 && !l.ap_on);
    /* "Reconnected" stayed 4 s (28100..32100), then the normal screens */
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_NORMAL);

    /* a phone still on the access point keeps it up */
    fw_link_start(&l, 0, 1);
    run(&l, 100, 25000, false, 0, NULL);
    run(&l, 25000, 28100, true, 0, NULL);
    {
        FwLinkInput in = input(28100, true);
        in.dns = 1;
        fw_link_tick(&l, &in);
    }
    for (uint32_t t = 28200; t < 60000; t += TICK) {
        FwLinkInput in = input(t, true);
        in.ap_clients = 1;
        in.ap_joined = 1000;
        CHECK(!(fw_link_tick(&l, &in) & FW_LINK_DO_SETUP_STOP));
    }
    CHECK(l.ap_on);
}

static void test_failed_check_and_repairs(void){
    FwLink l;
    uint32_t at;
    unsigned a;
    fw_link_start(&l, 0, 1);
    run(&l, 100, 25000, false, 0, NULL);                 /* entered reconnecting at 22100 */
    run(&l, 25000, 28100, true, 0, NULL);                /* check sent at 28000 */
    {
        FwLinkInput in = input(28100, true);
        in.dns = -1;
        CHECK(fw_link_tick(&l, &in) == 0);
    }
    /* rechecks at least 2000 ms apart */
    a = run(&l, 28200, 30100, true, FW_LINK_DO_DNS_CHECK, &at);
    CHECK(a == FW_LINK_DO_DNS_CHECK && at == 30000);
    /* connected without internet for more than 14999 ms: repair */
    a = run(&l, 30100, 40100, true, FW_LINK_DO_REPAIR, &at);
    CHECK(a & FW_LINK_DO_REPAIR);
    CHECK(at == 40000);                                  /* connected at 25000 */
    CHECK_STR(fw_link_view(&l)->line[0], "Repairing network...");
    /* then a full reconnect every 59999 ms */
    a = run(&l, 40100, 100100, true, FW_LINK_DO_ATTEMPT, &at);
    CHECK(at == 100000);
    /* restart only after 599999 ms reconnecting, and only when allowed */
    a = run(&l, 100100, 722200, true, FW_LINK_DO_RESTART, &at);
    CHECK(!(a & FW_LINK_DO_RESTART));
    {
        FwLinkInput in = input(722200, true);
        in.restart_allowed = true;
        CHECK(fw_link_tick(&l, &in) & FW_LINK_DO_RESTART);
        CHECK_STR(fw_link_view(&l)->line[0], "Restarting...");
    }
}

static void test_phone_pause(void){
    FwLink l;
    unsigned a;
    fw_link_start(&l, 0, 1);
    run(&l, 100, 22300, false, 0, NULL);                 /* setup, attempt running since 22200 */
    CHECK(l.attempting && fw_link_view(&l)->reconnecting);
    {
        FwLinkInput in = input(30000, false);
        in.ap_clients = 1;
        in.ap_joined = 29000;
        a = fw_link_tick(&l, &in);
        CHECK(a == FW_LINK_DO_ABORT && l.paused && !l.attempting);
        CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_SETUP && !fw_link_view(&l)->reconnecting);
        /* no retries while the phone is new */
        for (uint32_t t = 30100; t <= 29000 + FW_LINK_PHONE_PAUSE_MS; t += TICK) {
            in.now = t;
            CHECK(fw_link_tick(&l, &in) == 0);
        }
        /* five minutes on: retries resume at once */
        in.now = 29000 + FW_LINK_PHONE_PAUSE_MS + 1;
        CHECK(fw_link_tick(&l, &in) == FW_LINK_DO_ATTEMPT);
        CHECK(!l.paused && fw_link_view(&l)->reconnecting);
    }
    /* the phone leaving also resumes */
    fw_link_start(&l, 0, 1);
    run(&l, 100, 22300, false, 0, NULL);
    {
        FwLinkInput in = input(30000, false);
        in.ap_clients = 1;
        in.ap_joined = 29000;
        fw_link_tick(&l, &in);
        in.now = 40000;
        in.ap_clients = 0;
        CHECK(fw_link_tick(&l, &in) == FW_LINK_DO_ATTEMPT);
    }
}

static void test_loss(void){
    FwLink l;
    FwLinkInput in;
    uint32_t starts[16], n = 0, t, base;
    unsigned a;
    fw_link_start(&l, 0, 1);
    in = input(1000, true);
    fw_link_tick(&l, &in);
    run(&l, 1100, 10000, true, 0, NULL);
    CHECK(l.mode == FW_LINK_ONLINE && fw_link_view(&l)->kind == FW_LINK_VIEW_NORMAL);
    /* lost at 10000: reconnecting after 2 s */
    run(&l, 10000, 12000, false, 0, NULL);
    CHECK(l.mode == FW_LINK_ONLINE);
    in = input(12000, false);
    CHECK(fw_link_tick(&l, &in) == 0);
    CHECK(l.mode == FW_LINK_RECONNECTING && l.reconnect_flag && !l.setup);
    base = 12000;
    in = input(12100, false);
    CHECK(fw_link_tick(&l, &in) == FW_LINK_DO_ATTEMPT);
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_LINES && fw_link_view(&l)->color == FW_LINK_ORANGE);
    CHECK_STR(fw_link_view(&l)->line[0], "Lost connection,");
    CHECK_STR(fw_link_view(&l)->line[1], "retrying...");
    starts[n++] = 12100;
    for (t = 12200; t < base + 300000; t += TICK) {
        in = input(t, false);
        a = fw_link_tick(&l, &in);
        if ((a & FW_LINK_DO_ATTEMPT) && n < 16) starts[n++] = t;
        if (t == base + 59900) CHECK_STR(fw_link_view(&l)->line[0], "Lost connection,");   /* > 59999 ms lost */
        if (t == base + 60000) {
            CHECK_STR(fw_link_view(&l)->line[0], "WiFi disconnected,");
            CHECK_STR(fw_link_view(&l)->line[2], "reconnect...");
        }
        CHECK(!(a & FW_LINK_DO_SETUP_START));
    }
    /* attempts run back to back (each 20 s) until the spacing reaches 30 s */
    CHECK(starts[1] == starts[0] + 20100);               /* 20001 ms + the tick that notices */
    CHECK(starts[7] - starts[6] == 20100);
    CHECK(starts[8] - starts[7] == 30000);
    CHECK(starts[9] - starts[8] == 30000);
    /* more than 299999 ms lost: setup mode, then a retry a minute */
    in = input(base + 300000, false);
    a = fw_link_tick(&l, &in);
    CHECK(a & FW_LINK_DO_SETUP_START);
    CHECK(l.setup && fw_link_view(&l)->kind == FW_LINK_VIEW_SETUP && fw_link_view(&l)->reconnecting);
    {
        uint32_t at;
        a = run(&l, base + 300100, base + 400000, false, FW_LINK_DO_ATTEMPT, &at);
        CHECK(at == base + 360000);
    }
}

static void test_first_setup(void){
    FwLink l;
    FwLinkInput in;
    CHECK(fw_link_start(&l, 0, 0) == FW_LINK_DO_SETUP_START);
    CHECK_STR(fw_link_view(&l)->line[0], "Starting...");
    CHECK(l.mode == FW_LINK_FIRST_SETUP && l.ap_on);
    in = input(100, false);
    CHECK(fw_link_tick(&l, &in) == 0);
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_SETUP && !fw_link_view(&l)->reconnecting);
    CHECK(run(&l, 200, 600000, false, 0, NULL) == 0);   /* nothing to retry */
    in = input(600000, true);
    fw_link_tick(&l, &in);
    CHECK(fw_link_view(&l)->kind == FW_LINK_VIEW_LINES && fw_link_view(&l)->color == FW_LINK_GREEN);
    CHECK_STR(fw_link_view(&l)->line[0], "Connected");
}

static void test_provisioning(void){
    FwLink l;
    FwLinkInput in;
    fw_link_start(&l, 0, 1);
    run(&l, 100, 22300, false, 0, NULL);
    in = input(22300, false);
    in.provisioning = true;
    CHECK(fw_link_tick(&l, &in) == 0 && !l.attempting);
    for (uint32_t t = 22400; t < 200000; t += TICK) {
        in.now = t;
        CHECK(fw_link_tick(&l, &in) == 0);
    }
}

int main(void){
    test_backoff();
    test_boot_ok();
    test_boot_fails();
    test_reconnect_from_setup();
    test_failed_check_and_repairs();
    test_phone_pause();
    test_loss();
    test_first_setup();
    test_provisioning();
    printf("PASS: %d link checks\n", checks);
    return 0;
}
