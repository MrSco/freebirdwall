/* Runs components/fw_feed on a file and prints the result as JSON, for
 * tests/test_feed.py.
 *   feed_dump parse <body.msgpack>    -> {"rc":..,"frame":{..}}
 *   feed_dump body <config.json>      -> the request body, or "-1"
 *   feed_dump display <config.json>   -> {"has":..,..} */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_feed.h"

static unsigned char *slurp(const char *path, size_t *n){
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len + 1);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) return NULL;
    b[len] = 0;
    fclose(f);
    *n = (size_t)len;
    return b;
}

static void js(const char *s){
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20 || c >= 0x80) printf("\\u%04x", c);   /* raw bytes, so cut UTF-8 stays visible */
        else putchar(c);
    }
    putchar('"');
}

static void jd(double v){ if (v != v) printf("null"); else printf("%.17g", v); }

static void flight(const FwFlight *f){
    printf("{\"icao\":"); js(f->icao); printf(",\"cs\":"); js(f->cs); printf(",\"reg\":"); js(f->reg);
    printf(",\"alt_b\":"); jd(f->alt_b); printf(",\"alt_g\":"); jd(f->alt_g); printf(",\"vel\":"); jd(f->vel);
    printf(",\"trk\":"); jd(f->trk); printf(",\"vr\":"); jd(f->vr); printf(",\"gnd\":%s", f->gnd ? "true" : "false");
    printf(",\"sqk\":"); js(f->sqk); printf(",\"orig_icao\":"); js(f->orig_icao); printf(",\"orig_iata\":"); js(f->orig_iata);
    printf(",\"dest_icao\":"); js(f->dest_icao); printf(",\"dest_iata\":"); js(f->dest_iata);
    printf(",\"orig\":"); js(f->orig); printf(",\"dest\":"); js(f->dest);
    printf(",\"al_code\":"); js(f->al_code); printf(",\"al_paint\":"); js(f->al_paint); printf(",\"al_op\":"); js(f->al_op);
    printf(",\"al_name\":"); js(f->al_name); printf(",\"ac_type\":"); js(f->ac_type); printf(",\"ac_name\":"); js(f->ac_name);
    printf(",\"ac_class\":"); js(f->ac_class);
    printf(",\"has_trk\":%s,\"sts\":\"%s\",\"sexp\":%lld", f->has_trk ? "true" : "false", f->sts ? (char[2]){f->sts, 0} : "",
           (long long)f->sexp);
    printf(",\"prog\":"); jd(f->prog); printf(",\"dist\":"); jd(f->dist);
    printf(",\"conf\":"); js(f->conf); printf(",\"dep\":"); js(f->dep); printf(",\"arr\":"); js(f->arr);
    printf(",\"over\":"); js(f->over); printf(",\"tft\":"); js(f->tft); printf(",\"trk_gnd\":%s}", f->trk_gnd ? "true" : "false");
}

static void leg(const FwSyncLeg *l){
    printf("{\"lt\":"); js(l->lt); printf(",\"ls\":"); js(l->ls); printf(",\"al\":"); js(l->al); printf(",\"fn\":"); js(l->fn);
    printf(",\"orig\":"); js(l->orig); printf(",\"dest\":"); js(l->dest); printf(",\"dep_rel\":"); js(l->dep_rel);
    printf(",\"arr_rel\":"); js(l->arr_rel); printf(",\"dep_at\":%u,\"arr_at\":%u}", (unsigned)l->dep_at, (unsigned)l->arr_at);
}

int main(int argc, char **argv){
    size_t n;
    unsigned char *b;
    if (argc != 3 || !(b = slurp(argv[2], &n))) return 2;
    if (!strcmp(argv[1], "parse")) {
        static FwFeedFrame fr;
        int rc = fw_feed_parse(b, n, &fr);
        printf("{\"rc\":%d,\"count\":%d,\"tracked\":%s,\"overflow\":%d,\"ch\":", rc, fr.count, fr.tracked ? "true" : "false",
               fr.overflow);
        js(fr.config_hash);
        printf(",\"flights\":[");
        for (int i = 0; i < fr.count; ++i) { if (i) putchar(','); flight(&fr.flights[i]); }
        printf("],\"has_active\":%s,\"active\":", fr.has_active ? "true" : "false");
        leg(&fr.active);
        printf(",\"upcoming\":[");
        for (int i = 0; i < fr.upcoming_count; ++i) { if (i) putchar(','); leg(&fr.upcoming[i]); }
        printf("],\"message\":");
        js(fr.message);
        printf("}\n");
    } else if (!strcmp(argv[1], "http")) {
        /* the response fed in chunks of 1, 2, 3, ... bytes, as a socket might */
        static uint8_t body[FW_FEED_RESPONSE_MAX];
        FwHttpResponse r;
        int rc = 0;
        size_t at = 0, step = 1;
        fw_http_init(&r, body, sizeof body);
        while (at < n && rc == 0) {
            size_t k = n - at < step ? n - at : step;
            rc = fw_http_feed(&r, b + at, k);
            at += k;
            step = step % 7 + 1;
        }
        printf("{\"rc\":%d,\"status\":%d,\"length\":%ld,\"msgpack\":%s,\"body\":%u}\n", rc, r.status,
               r.content_length, r.msgpack ? "true" : "false", (unsigned)r.body_len);
    } else if (!strcmp(argv[1], "body")) {
        static char out[8192];
        int len = fw_feed_body((const char *)b, out, sizeof out);
        if (len < 0) printf("-1\n"); else printf("%s\n", out);
    } else if (!strcmp(argv[1], "bodysync")) {
        /* first line: the Sync flight number (may be empty); the rest: the configuration */
        static char out[8192];
        char *nl = strchr((char *)b, '\n');
        int len;
        if (!nl) return 2;
        *nl = 0;
        len = fw_feed_body_sync(nl + 1, (const char *)b, out, sizeof out);
        if (len < 0) printf("-1\n"); else printf("%s\n", out);
    } else if (!strcmp(argv[1], "source")) {
        printf("%d\n", (int)fw_feed_source((const char *)b));
    } else if (!strcmp(argv[1], "follow")) {
        printf("%d\n", (int)fw_feed_follow_app((const char *)b));
    } else if (!strcmp(argv[1], "display")) {
        FwFeedDisplay d;
        bool has = fw_feed_display((const char *)b, &d);
        printf("{\"has\":%s,\"alt\":%d,\"vel\":%d,\"vr\":%d,\"dist\":%d,\"cardinal\":%s,\"title\":%d,\"layout\":",
               has ? "true" : "false", d.units.alt, d.units.vel, d.units.vr, d.units.dist, d.units.trk_cardinal ? "true" : "false",
               (int)d.title);
        js(d.layout); printf(",\"loading\":"); js(d.loading);
        printf(",\"dwell\":%u,\"max\":%d,\"border\":%s,\"border_percent\":%d,\"brightness\":%d,\"anchors\":[", (unsigned)d.dwell_ms,
               (int)d.max_flights, d.border ? "true" : "false", d.border_percent, d.brightness_percent);
        for (int i = 0; i < d.anchor_count; ++i) { if (i) putchar(','); js(d.anchors[i]); }
        printf("],\"offset\":%d,\"dst\":", d.utc_offset_min); js(d.dst_rule);
        printf(",\"extras\":[");
        for (int i = 0; i < d.extra_count; ++i) {
            if (i) putchar(',');
            putchar('['); js(d.extra[i].label); printf(",%d,", d.extra[i].utc_offset_min); js(d.extra[i].dst_rule); putchar(']');
        }
        putchar(']');
        printf(",\"h24\":%s,\"seconds\":%s,\"large\":%s,\"date\":%s,\"day_first\":%s,\"weekday\":%s,\"locale\":",
               d.clock_24h ? "true" : "false", d.clock_seconds ? "true" : "false", d.clock_large ? "true" : "false",
               d.clock_show_date ? "true" : "false", d.clock_day_first ? "true" : "false", d.clock_weekday ? "true" : "false");
        js(d.locale);
        printf(",\"enabled\":%s,\"has_sleep\":%s,\"sleep\":[%d,%d,%d],\"scroll\":%u}\n", d.display_enabled ? "true" : "false",
               d.has_sleep ? "true" : "false", d.sleep_start_min, d.sleep_end_min, d.sleep_percent, (unsigned)d.scroll_px_ms);
    } else {
        return 2;
    }
    free(b);
    return 0;
}
