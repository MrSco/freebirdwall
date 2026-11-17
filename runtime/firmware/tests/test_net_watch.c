/* The network watchdog's rule (main/net_watch_rule.h). */
#include <stdio.h>
#include <string.h>
#include "net_watch_rule.h"

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define OKHEAP 30000u
#define S 1000u
#define BIG 20000u

int main(void){
    FwNetWatch w;
    uint32_t t;

    /* a router that answers: all quiet */
    memset(&w, 0, sizeof w);
    for (t = 0; t < 600 * S; t += S) {
        if (t % (15 * S) == 0) fw_net_watch_reply(&w, t);
        CHECK(fw_net_watch_check(&w, t, true, OKHEAP, BIG) == FW_NET_OK);
    }

    /* tonight: answered, then deaf on a joined network -> restart after 150 s */
    memset(&w, 0, sizeof w);
    fw_net_watch_reply(&w, 10 * S);
    CHECK(fw_net_watch_check(&w, 10 * S, true, OKHEAP, BIG) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 159 * S, true, OKHEAP, BIG) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 160 * S, true, OKHEAP, BIG) == FW_NET_ROUTER_SILENT);

    /* a router that never answers pings is never held against the wall */
    memset(&w, 0, sizeof w);
    for (t = 0; t < 3600 * S; t += S) CHECK(fw_net_watch_check(&w, t, true, OKHEAP, BIG) == FW_NET_OK);

    /* Wi-Fi down (router rebooting): never a restart, however long; the
     * silence counts again from the rejoin */
    memset(&w, 0, sizeof w);
    fw_net_watch_reply(&w, 0);
    CHECK(fw_net_watch_check(&w, 0, true, OKHEAP, BIG) == FW_NET_OK);
    for (t = S; t < 1200 * S; t += S) CHECK(fw_net_watch_check(&w, t, false, 100, 100) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 1200 * S, true, OKHEAP, BIG) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 1349 * S, true, OKHEAP, BIG) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 1350 * S, true, OKHEAP, BIG) == FW_NET_ROUTER_SILENT);

    /* memory: a dip under 4 KiB that recovers is fine; a minute of it is not */
    memset(&w, 0, sizeof w);
    CHECK(fw_net_watch_check(&w, 0, true, 3000, 3000) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 59 * S, true, 3000, 3000) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 60 * S, true, 5000, BIG) == FW_NET_OK);       /* back over: count restarts */
    CHECK(fw_net_watch_check(&w, 61 * S, true, 3000, 3000) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 120 * S, true, 3000, 3000) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 121 * S, true, 3000, 3000) == FW_NET_HEAP_EXHAUSTED);
    /* ...and also without any ping answered (a router that ignores pings) */
    CHECK(!w.replied);

    /* fragmented: plenty free, but no block big enough for a Wi-Fi frame
     * (the wall that went deaf: 6268 free, largest 1344) */
    memset(&w, 0, sizeof w);
    CHECK(fw_net_watch_check(&w, 0, true, 6268, 1344) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 59 * S, true, 6268, 1344) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 60 * S, true, 6268, 1344) == FW_NET_HEAP_EXHAUSTED);
    memset(&w, 0, sizeof w);
    CHECK(fw_net_watch_check(&w, 0, true, 6268, 1344) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 30 * S, true, 6268, 2048) == FW_NET_OK);   /* a big enough block again */
    CHECK(fw_net_watch_check(&w, 89 * S, true, 6268, 1344) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, 149 * S, true, 6268, 1344) == FW_NET_HEAP_EXHAUSTED);

    /* the clock wraps (49.7 days): differences still hold */
    memset(&w, 0, sizeof w);
    t = 0xFFFFFFFFu - 100 * S;
    fw_net_watch_reply(&w, t);
    CHECK(fw_net_watch_check(&w, t, true, OKHEAP, BIG) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, t + 149 * S, true, OKHEAP, BIG) == FW_NET_OK);
    CHECK(fw_net_watch_check(&w, t + 150 * S, true, OKHEAP, BIG) == FW_NET_ROUTER_SILENT);

    printf("%s: %d network watchdog checks\n", failed ? "FAIL" : "PASS", checks);
    return failed != 0;
}
