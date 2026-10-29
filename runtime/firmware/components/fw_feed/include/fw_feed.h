#ifndef FW_FEED_H
#define FW_FEED_H
/* The vendor flight feed as the stock firmware speaks it (FEED.md):
 *  - the response (MessagePack, FUN_4200d594 / FUN_4200cee0) with stock's
 *    exact type rules: any wrongly typed value rejects the whole response;
 *  - the request body built from the stored configuration's request_config
 *    (FUN_4201325c);
 *  - the display settings from its display_config (FUN_420189b4).
 * Portable C: the board worker does the HTTP and NVS parts. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_flight.h"
#include "fw_sync.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FW_FEED_RESPONSE_MAX 5120   /* stock accepts Content-Length 1..5120 */
#define FW_FEED_ZONE_UNSET INT16_MIN   /* stock "no zone" (0x8000): UTC, named "UTC"; = FW_ZONE_UNSET */

typedef struct {
    FwFlight flights[FW_FEED_MAX_FLIGHTS];
    int count;                       /* records kept (extras beyond 5 skipped) */
    bool tracked;                    /* the last of f/tf seen was tf */
    int overflow;                    /* ov */
    char config_hash[40];            /* ch, cut to 39 bytes */
    bool has_active;                 /* sla */
    FwSyncLeg active;
    FwSyncLeg upcoming[FW_SYNC_UPCOMING_MAX];
    int upcoming_count;              /* slu */
    char message[FW_SYNC_MESSAGE_MAX];   /* scm */
} FwFeedFrame;

enum {
    FW_FEED_OK = 0,
    FW_FEED_ERR_FORMAT = -1,         /* not MessagePack, truncated, or a wrongly typed value */
    FW_FEED_ERR_VERSION = -2,        /* v > 1 */
};

/* Parses a response body. On any error the frame is left cleared. */
int fw_feed_parse(const uint8_t *body, size_t len, FwFeedFrame *out);

/* The POST /v2/flights body from the stored configuration JSON (usercfg/json,
 * its request_config): radius_request, geo_request, radius_requests,
 * geo_requests, data_filters, tracked_flights, in that order, with stock's
 * defaults and limits. Returns the length written (NUL-terminated), or -1
 * when the configuration has no request_config or the body does not fit. */
int fw_feed_body(const char *config_json, char *out, size_t cap);

/* Owned: the same, with the Sync leg in the air (SYNC.md "Live trip") as one
 * more tracked flight: sync_flight is its flight number (al + fn, e.g.
 * "AA1234"), NULL or invalid for none. Added once (not when the owner already
 * tracks it), auto_remove false, distance and metrics shown; with 8 already
 * tracked it takes the last place. */
int fw_feed_body_sync(const char *config_json, const char *sync_flight, char *out, size_t cap);

/* The display_config values the owned screens use. Strings point into
 * the struct; missing values keep stock defaults. */
typedef struct {
    FwUnits units;
    FwTitle title;
    char layout[24];                 /* layout_template, DEFAULT */
    char loading[12];                /* loading_template: TEXT (default), CLOCK, ELLIPSIS, BLANK */
    uint32_t dwell_ms;               /* flight_display_speed */
    FwMaxFlights max_flights;
    bool border;
    uint8_t border_percent;
    uint8_t brightness_percent;      /* 0 = not given */
    char anchors[8][8];              /* anchor_airports */
    int anchor_count;
    /* clock */
    int16_t utc_offset_min;          /* time_zone.standard_utc_offset_minutes; FW_FEED_ZONE_UNSET = none */
    char dst_rule[16];               /* time_zone.dst_rule */
    int extra_count;                 /* clock.additional_timezones, at most 2 (FUN_420189b4) */
    struct {
        char label[16];              /* "" = the zone's own name */
        int16_t utc_offset_min;      /* time_zone.standard_utc_offset_minutes; FW_FEED_ZONE_UNSET = none */
        char dst_rule[16];
    } extra[2];
    bool clock_24h, clock_seconds, clock_large, clock_show_date;
    bool clock_day_first, clock_weekday;   /* date_format.order "day_month", .show_weekday */
    char locale[8];
    /* power */
    bool display_enabled;            /* display_enabled, default true */
    bool has_sleep;                  /* sleep_time with start and end */
    int16_t sleep_start_min, sleep_end_min;   /* minutes after local midnight */
    uint8_t sleep_percent;           /* sleep_time.sleep_brightness_percent (0 = off) */
    bool has_display_config;
    /* owned */
    uint16_t scroll_px_ms;           /* owned_config.scroll_speed: slow 80, normal 40 (default), fast 20 ms a pixel */
    bool progress_bar;               /* owned_config.progress_bar, default false */
    bool heartbeat_enabled;          /* owned_config.heartbeat_enabled, default true: the blinking corner pixel */
} FwFeedDisplay;

/* Returns false when the JSON has no display_config. */
bool fw_feed_display(const char *config_json, FwFeedDisplay *out);

/* Owned: where flights come from (owned_config.flight_source; SETTINGS.md).
 * Anything but "community" is the vendor feed, the default. */
typedef enum { FW_SOURCE_VENDOR, FW_SOURCE_COMMUNITY } FwFlightSource;
FwFlightSource fw_feed_source(const char *config_json);

/* Owned: whether changes made in the FlightWall app reach the wall
 * (owned_config.follow_app; SETTINGS.md "Changes made in the app"). Anything
 * but false follows them, the default. */
bool fw_feed_follow_app(const char *config_json);

/* ------------------------------------------------------------ HTTP response */

/* An HTTP/1.x response read in pieces, as stock's client accepts it
 * (FUN_42014d68): a status line, headers up to 512 bytes each, a
 * Content-Length of 1..cap, no chunked transfer; the body lands in the
 * caller's buffer. */
typedef struct {
    int state;                       /* internal */
    int status;                      /* HTTP status */
    long content_length;             /* -1 = not given */
    bool msgpack;                    /* Content-Type starts with application/x-msgpack */
    bool chunked;
    char line[512];
    size_t line_len;
    uint8_t *body;
    size_t cap, body_len;
} FwHttpResponse;

void fw_http_init(FwHttpResponse *r, uint8_t *body, size_t cap);
/* 1 = complete, 0 = needs more, -1 = refused (malformed, chunked, missing or
 * too large Content-Length, or more bytes than announced). */
int fw_http_feed(FwHttpResponse *r, const uint8_t *data, size_t n);

#ifdef __cplusplus
}
#endif
#endif
