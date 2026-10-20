/* Several flights: the stock rotation (FLIGHT-SCREENS.md "Several flights"). */
#include "fw_flight.h"
#include <stdio.h>
#include <string.h>

static bool eq(const char *a, const char *b){ return a && b && strcmp(a, b) == 0; }

FwMaxFlights fw_max_flights_parse(const char *behavior, bool hide_given, bool hide){
    if (eq(behavior, "off")) return FW_MAXF_OFF;
    if (eq(behavior, "text")) return FW_MAXF_TEXT;
    if (eq(behavior, "indicator_led")) return FW_MAXF_INDICATOR;
    if (!hide_given) return FW_MAXF_TEXT;
    return hide ? FW_MAXF_OFF : FW_MAXF_TEXT;
}

uint32_t fw_display_speed_ms(const char *speed){
    if (eq(speed, "slow")) return 5000;
    if (eq(speed, "fast")) return 2000;
    return 3000;
}

void fw_rotation_init(FwRotation *r, FwMaxFlights behavior, uint32_t dwell_ms){
    memset(r, 0, sizeof *r);
    r->behavior = behavior;
    r->dwell_ms = dwell_ms < 500 ? 500 : dwell_ms;    /* FUN_4200ed6c */
    r->cap = FW_FEED_MAX_FLIGHTS;
}

static int shown(const FwRotation *r){ return r->count < r->cap ? r->count : r->cap; }

const FwFlight *fw_rotation_next(const FwRotation *r){
    int n = shown(r), i;
    if (!r->have_list || n < 1) return NULL;
    i = r->index + 1;
    if (i >= n) i = 0;                        /* past the last flight, or the "too many" screen */
    return &r->list[i];
}

static void remember(FwRotation *r, const FwFlight *f){
    r->have_current = f != NULL;
    snprintf(r->cur_icao, sizeof r->cur_icao, "%s", f ? f->icao : "");
    snprintf(r->cur_cs, sizeof r->cur_cs, "%s", f ? f->cs : "");
}

static bool same(const FwRotation *r, const FwFlight *f){
    return (*r->cur_icao && strcmp(r->cur_icao, f->icao) == 0) || (*r->cur_cs && strcmp(r->cur_cs, f->cs) == 0);
}

/* FUN_4200e114: keep showing the same aircraft when it is still listed (by
 * ICAO address, else callsign); otherwise start again from the first. */
static void take_list(FwRotation *r, int cap, bool stay_on_too_many){
    int n, i, at = 0;
    bool found = false;
    r->cap = cap;
    n = shown(r);
    if (r->have_current && n > 0) {
        for (i = 0; i < n && !found; ++i)
            if (*r->cur_icao && strcmp(r->cur_icao, r->list[i].icao) == 0) { at = i; found = true; }
        for (i = 0; i < n && !found; ++i)
            if (*r->cur_cs && strcmp(r->cur_cs, r->list[i].cs) == 0) { at = i; found = true; }
        if (!found) found = same(r, &r->list[0]);
    }
    if (stay_on_too_many) {
        r->index = n;                         /* stay on the "too many" screen */
    } else {
        r->index = at;
        if (!found) r->since = 0;             /* a new aircraft gets a full turn */
    }
    r->dirty = true;
    remember(r, n > 0 ? &r->list[at < n ? at : 0] : NULL);
}

void fw_rotation_set_list(FwRotation *r, const FwFlight *flights, int count, int overflow){
    bool stay = r->too_many && r->on_too_many;   /* decided on the previous list, as stock */
    if (count < 0) count = 0;
    if (count > FW_FEED_MAX_FLIGHTS) count = FW_FEED_MAX_FLIGHTS;
    memcpy(r->list, flights, (size_t)count * sizeof *flights);
    r->count = count;
    r->overflow = overflow;
    r->have_list = true;
    r->indicator = false;
    if (count == 0) {                         /* FUN_4200f3e4: "No flights..." */
        r->too_many = false;
        remember(r, NULL);
        r->dirty = true;
        return;
    }
    if (overflow > 0 && r->behavior == FW_MAXF_TEXT) {
        take_list(r, 4, stay);
        r->too_many = true;                   /* four flights and the "too many" screen */
        return;
    }
    r->indicator = overflow > 0 && r->behavior == FW_MAXF_INDICATOR;
    take_list(r, FW_FEED_MAX_FLIGHTS, stay);
    r->too_many = false;
}

FwShow fw_rotation_tick(FwRotation *r, uint32_t now, const FwFlight **flight, int *more, bool *redraw){
    int n = shown(r), slots = n + (r->too_many ? 1 : 0);
    bool advanced = false;
    *flight = NULL;
    *more = r->overflow + 5;
    *redraw = false;
    if (!r->have_list) { *redraw = r->dirty; r->dirty = false; return FW_SHOW_NOTHING_YET; }
    if (r->count == 0) { *redraw = r->dirty; r->dirty = false; return FW_SHOW_NO_FLIGHTS; }
    if (slots < 2) {
        if (r->since == 0) { r->since = now ? now : 1; r->dirty = true; advanced = true; }
    } else if (r->since == 0 || now - r->since >= r->dwell_ms + r->extra_ms) {
        if (r->since != 0) r->index = (r->index + 1) % slots;
        r->since = now ? now : 1;
        r->extra_ms = 0;
        r->dirty = true;
        advanced = true;
    }
    r->on_too_many = r->index >= n;
    if (r->index < n) {
        if (advanced) remember(r, &r->list[r->index]);
        *flight = &r->list[r->index];
        *redraw = r->dirty;
        r->dirty = false;
        return FW_SHOW_FLIGHT;
    }
    if (!r->too_many) {
        /* Left on a slot the new list no longer has: stock draws nothing new
         * until the next turn of the rotation. */
        return FW_SHOW_FLIGHT;
    }
    *redraw = r->dirty;
    r->dirty = false;
    return FW_SHOW_TOO_MANY;
}
