/* Joining, losing and regaining Wi-Fi as stock does; see fw_link.h and
 * SETUP.md "Wi-Fi failure and reconnect". */
#include "fw_link.h"
#include <string.h>

static void show(FwLink *l, FwLinkViewKind kind, const char *a, const char *b, const char *c, uint16_t color){
    l->view.kind = kind;
    l->view.line[0] = a;
    l->view.line[1] = b;
    l->view.line[2] = c;
    l->view.color = color;
    l->view.reconnecting = false;
    l->view.clear = false;
    ++l->view.serial;
}

static void show_setup(FwLink *l){
    show(l, FW_LINK_VIEW_SETUP, NULL, NULL, NULL, FW_LINK_WHITE);
    l->view.reconnecting = l->reconnect_flag;
}

static void hold(FwLink *l, uint32_t now, uint32_t ms){
    l->holding = true;
    l->hold_until = now + ms;
}

static bool held(const FwLink *l, uint32_t now){
    return l->holding && (int32_t)(now - l->hold_until) < 0;
}

static unsigned attempt(FwLink *l, uint32_t now){
    l->attempting = true;
    l->attempt_failed = false;
    l->attempt_start = now;
    return FW_LINK_DO_ATTEMPT;
}

static unsigned setup_on(FwLink *l){
    l->setup = true;
    l->ap_idle = false;
    if (l->ap_on) return 0;
    l->ap_on = true;
    return FW_LINK_DO_SETUP_START;
}

uint32_t fw_link_backoff(int attempts, bool setup){
    uint32_t ms;
    if (setup) return FW_LINK_SETUP_RETRY_MS;
    if (attempts <= 4) return FW_LINK_BACKOFF_MS;
    if (attempts - 5 >= 4) return FW_LINK_BACKOFF_MAX_MS;
    ms = (uint32_t)FW_LINK_BACKOFF_MS << (attempts - 5);
    return ms > FW_LINK_BACKOFF_MAX_MS ? FW_LINK_BACKOFF_MAX_MS : ms;
}

uint32_t fw_link_phone_joined(uint32_t now, uint32_t previous_join, uint32_t previous_leave, bool had_previous){
    return had_previous && now - previous_leave <= FW_LINK_REJOIN_MS ? previous_join : now;
}

/* App state 9 (FUN_420165b0: reset, retry at once, "attempting to reconnect"
 * flag; with setup, FUN_4201666c + FUN_4201fb20 as after a failed boot). */
static unsigned enter_reconnecting(FwLink *l, uint32_t now, bool with_setup){
    unsigned a = 0;
    l->mode = FW_LINK_RECONNECTING;
    l->entered = l->lost_since = now;
    l->attempts = 0;
    l->setup = false;
    l->message1 = l->message2 = false;
    l->reconnect_flag = true;
    l->retry_now = true;
    l->paused = false;
    l->conn = false;
    l->dns_pending = l->dns_done = false;
    l->repaired = false;
    l->holding = false;
    if (with_setup) {
        a |= setup_on(l);
        l->last_start = now;
        show_setup(l);
    }
    return a;
}

unsigned fw_link_start(FwLink *l, uint32_t now, int saved_networks){
    memset(l, 0, sizeof *l);
    if (saved_networks > 0) {
        l->mode = FW_LINK_BOOT;
        show(l, FW_LINK_VIEW_SPLASH, "Connecting...", NULL, NULL, FW_LINK_WHITE);
        return attempt(l, now);
    }
    l->mode = FW_LINK_FIRST_SETUP;
    show(l, FW_LINK_VIEW_SPLASH, "Starting...", NULL, NULL, FW_LINK_WHITE);
    return setup_on(l);
}

const FwLinkView *fw_link_view(const FwLink *l){ return &l->view; }

/* Owned: once online, the access point goes off after FW_LINK_AP_IDLE_MS
 * without a phone on it. */
static unsigned access_point_idle(FwLink *l, const FwLinkInput *in){
    if (!l->ap_on || in->ap_clients > 0) {
        l->ap_idle = false;
        return 0;
    }
    if (!l->ap_idle) {
        l->ap_idle = true;
        l->ap_idle_since = in->now;
        return 0;
    }
    if (in->now - l->ap_idle_since < FW_LINK_AP_IDLE_MS) return 0;
    l->ap_on = l->setup = l->ap_idle = false;
    return FW_LINK_DO_SETUP_STOP;
}

static unsigned online(FwLink *l, const FwLinkInput *in){
    uint32_t now = in->now;
    if (l->holding && !held(l, now)) {
        l->holding = false;
        show(l, FW_LINK_VIEW_NORMAL, NULL, NULL, NULL, FW_LINK_WHITE);
    }
    /* FUN_42016588: not connected for 2 s -> reconnecting */
    if (in->connected) {
        l->losing = false;
    } else if (!l->losing) {
        l->losing = true;
        l->loss_start = now;
    } else if (now - l->loss_start >= FW_LINK_LOSS_MS) {
        l->losing = false;
        return enter_reconnecting(l, now, false);   /* the first retry goes out next tick */
    }
    return access_point_idle(l, in);
}

/* App state 9: FUN_42016b50 retries, the phone pause, the loss messages,
 * then on a connection the network check (FUN_42016680) and repairs. */
static unsigned reconnecting(FwLink *l, const FwLinkInput *in){
    uint32_t now = in->now;
    unsigned a = 0;
    bool pause = l->ap_on && in->ap_clients > 0 && now - in->ap_joined <= FW_LINK_PHONE_PAUSE_MS;

    if (pause) {
        /* a phone is setting the board up: leave the radio to it */
        if (!l->paused) {
            l->paused = true;
            l->reconnect_flag = false;
            if (l->attempting) {
                l->attempting = false;
                a |= FW_LINK_DO_ABORT;
            }
            if (l->view.kind == FW_LINK_VIEW_SETUP) show_setup(l);
        }
        return a;
    }
    if (l->paused) {
        l->paused = false;
        l->reconnect_flag = true;
        l->retry_now = true;
        if (l->view.kind == FW_LINK_VIEW_SETUP) show_setup(l);
    }

    if (!in->connected) {
        if (l->conn) {
            l->conn = false;
            if (l->setup) show_setup(l);
        }
        if (!l->setup) {
            if (now - l->lost_since > FW_LINK_SECOND_MESSAGE_MS) {
                if (!l->message2) {
                    l->message2 = true;
                    show(l, FW_LINK_VIEW_LINES, "WiFi disconnected,", "attempting to", "reconnect...", FW_LINK_ORANGE);
                }
            } else if (!l->message1) {
                l->message1 = true;
                show(l, FW_LINK_VIEW_LINES, "Lost connection,", "retrying...", NULL, FW_LINK_ORANGE);
            }
            if (now - l->lost_since > FW_LINK_SETUP_AFTER_MS) {
                a |= setup_on(l);
                l->last_start = now;
                show_setup(l);
            }
        }
        if ((l->retry_now || now - l->last_start >= fw_link_backoff(l->attempts, l->setup)) && !l->attempting) {
            l->retry_now = false;
            l->last_start = now;
            ++l->attempts;
            a |= attempt(l, now);
        }
        return a;
    }

    if (!l->conn) {
        l->conn = true;
        l->connected_since = now;
        l->dns_pending = l->dns_done = false;
        l->repaired = false;
        l->message1 = l->message2 = false;
        show(l, FW_LINK_VIEW_SPLASH, "Checking network...", NULL, NULL, FW_LINK_WHITE);
    }
    if (l->dns_pending && in->dns != 0) {
        l->dns_pending = false;
        if (in->dns > 0) {
            /* FUN_42016650: back online */
            l->mode = FW_LINK_ONLINE;
            l->reconnect_flag = false;
            l->setup = false;
            l->losing = false;
            l->ap_idle = false;
            show(l, FW_LINK_VIEW_SPLASH, "Reconnected", NULL, NULL, FW_LINK_WHITE);
            l->view.clear = true;
            hold(l, now, FW_LINK_ONLINE_HOLD_MS);
            return a;
        }
    }
    if (!l->dns_pending && now - l->connected_since > FW_LINK_DNS_AFTER_MS &&
        (!l->dns_done || now - l->last_dns >= FW_LINK_DNS_GAP_MS)) {
        l->dns_pending = l->dns_done = true;
        l->last_dns = now;
        a |= FW_LINK_DO_DNS_CHECK;
    }
    if (!l->repaired && now - l->connected_since > FW_LINK_REPAIR_AFTER_MS) {
        l->repaired = true;
        l->last_repair = now;
        show(l, FW_LINK_VIEW_SPLASH, "Repairing network...", NULL, NULL, FW_LINK_WHITE);
        a |= FW_LINK_DO_REPAIR;
    } else if (l->repaired && now - l->last_repair > FW_LINK_REPAIR_EVERY_MS) {
        l->last_repair = now;
        show(l, FW_LINK_VIEW_SPLASH, "Repairing network...", NULL, NULL, FW_LINK_WHITE);
        a |= FW_LINK_DO_ABORT | attempt(l, now);
    }
    if (now - l->entered > FW_LINK_RESTART_AFTER_MS && in->restart_allowed) {
        show(l, FW_LINK_VIEW_SPLASH, "Restarting...", NULL, NULL, FW_LINK_WHITE);
        a |= FW_LINK_DO_RESTART;
    }
    return a;
}

unsigned fw_link_tick(FwLink *l, const FwLinkInput *in){
    uint32_t now = in->now;
    unsigned a = 0;
    if (in->provisioning) {
        /* /provision owns the station; its own messages are on screen */
        l->attempting = false;
        return 0;
    }
    /* FUN_42021c4c / FUN_42021d84: an attempt ends connected or after 20 s */
    if (l->attempting) {
        if (in->connected) {
            l->attempting = false;
        } else if (now - l->attempt_start > FW_LINK_ATTEMPT_MS) {
            l->attempting = false;
            l->attempt_failed = true;
            a |= FW_LINK_DO_ABORT;
        }
    }
    switch (l->mode) {
    case FW_LINK_BOOT:
        if (l->pending_setup) {
            if (!held(l, now)) {
                l->pending_setup = false;
                a |= enter_reconnecting(l, now, true);
            }
        } else if (in->connected) {
            l->mode = FW_LINK_ONLINE;
            show(l, FW_LINK_VIEW_SPLASH, "Connected", NULL, NULL, FW_LINK_WHITE);
            hold(l, now, FW_LINK_CONNECTED_HOLD_MS);
        } else if (l->attempt_failed) {
            l->pending_setup = true;
            show(l, FW_LINK_VIEW_SPLASH, "Connection failed", NULL, NULL, FW_LINK_ORANGE);
            hold(l, now, FW_LINK_FAILED_HOLD_MS);
        }
        break;
    case FW_LINK_FIRST_SETUP:
        if (in->connected) {
            l->mode = FW_LINK_ONLINE;
            show(l, FW_LINK_VIEW_LINES, "Connected", NULL, NULL, FW_LINK_GREEN);
            hold(l, now, FW_LINK_CONNECTED_HOLD_MS);
        } else if (l->view.kind != FW_LINK_VIEW_SETUP) {
            show_setup(l);
        }
        break;
    case FW_LINK_ONLINE:
        a |= online(l, in);
        break;
    case FW_LINK_RECONNECTING:
        a |= reconnecting(l, in);
        break;
    }
    return a;
}
