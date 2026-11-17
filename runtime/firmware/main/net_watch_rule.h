#ifndef FLIGHTWALL_NET_WATCH_RULE_H
#define FLIGHTWALL_NET_WATCH_RULE_H
/* The network watchdog's rule (net_watch.c), kept free of SDK types so
 * tests/test_net_watch.c can check it on the desktop.
 *
 * The wall can lose the network without losing Wi-Fi: with the heap used up,
 * the driver and lwIP drop every frame, nothing frees, and the panel keeps
 * drawing the clock while the wall answers nothing, not even a ping. No task
 * starves and nothing crashes, so neither the task watchdog nor the crash
 * guard sees it. This rule does: it asks for a restart (which keeps the log)
 * when, while Wi-Fi is joined,
 *  - the router answered our pings earlier this start and has not for
 *    FW_NET_WATCH_SILENT_MS (a router that never answers pings is never
 *    held against the wall), or
 *  - free memory stayed under FW_NET_WATCH_HEAP_LOW, or its largest block
 *    under FW_NET_WATCH_BLOCK_LOW (too small for one Wi-Fi frame: the wall
 *    that went deaf had 6 KiB free in pieces of at most 1.3 KiB), for
 *    FW_NET_WATCH_HEAP_MS.
 * Without Wi-Fi it never asks: the link's own join / reconnect rules own that
 * case, and a restart there could start setup mode. */
#include <stdbool.h>
#include <stdint.h>

#define FW_NET_WATCH_PING_MS 15000u       /* one ping to the router this often */
#define FW_NET_WATCH_SILENT_MS 150000u    /* ten pings in a row unanswered */
#define FW_NET_WATCH_HEAP_LOW 4096u       /* bytes */
#define FW_NET_WATCH_BLOCK_LOW 2048u      /* bytes, the largest free block */
#define FW_NET_WATCH_HEAP_MS 60000u
#define FW_NET_WATCH_NOTE_LOW 10240u      /* a "heap_low" line in the log under this... */
#define FW_NET_WATCH_NOTE_OK 16384u       /* ...and "heap_ok" once back over this */

typedef enum { FW_NET_OK = 0, FW_NET_ROUTER_SILENT, FW_NET_HEAP_EXHAUSTED } FwNetVerdict;

typedef struct {
    bool replied;            /* the router answered a ping this start */
    uint32_t reply_at;       /* ms, the last answer */
    bool joined_before;      /* Wi-Fi joined at the last check */
    bool low;                /* free memory under FW_NET_WATCH_HEAP_LOW... */
    uint32_t low_since;      /* ...since then (ms) */
} FwNetWatch;

static inline void fw_net_watch_reply(FwNetWatch *w, uint32_t now){
    w->replied = true;
    w->reply_at = now;
}

/* Once a second or so. joined: Wi-Fi associated with an address; largest:
 * the largest free block. */
static inline FwNetVerdict fw_net_watch_check(FwNetWatch *w, uint32_t now, bool joined, uint32_t free_heap,
                                              uint32_t largest){
    if (!joined) {
        w->joined_before = false;
        w->low = false;
        return FW_NET_OK;
    }
    if (!w->joined_before) {          /* (re)joined: the silence counts from now */
        w->joined_before = true;
        if (w->replied) w->reply_at = now;
    }
    if (free_heap >= FW_NET_WATCH_HEAP_LOW && largest >= FW_NET_WATCH_BLOCK_LOW) w->low = false;
    else if (!w->low) {
        w->low = true;
        w->low_since = now;
    }
    if (w->low && now - w->low_since >= FW_NET_WATCH_HEAP_MS) return FW_NET_HEAP_EXHAUSTED;
    if (w->replied && now - w->reply_at >= FW_NET_WATCH_SILENT_MS) return FW_NET_ROUTER_SILENT;
    return FW_NET_OK;
}

#endif
