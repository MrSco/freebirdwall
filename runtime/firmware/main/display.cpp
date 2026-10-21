#include "display.h"
#include "memory_probe.h"
#include "sdkconfig.h"
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
#include "fw_text.h"
#include "fw_screens.h"
#include "fw_clock.h"
#include "fw_flight.h"
#include "fw_logo.h"
#include "fw_assets.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "setup_server.h"
#include "wifi_link.h"
#include "games_server.h"
#include "feed.h"
#include "aliases.h"
#include "dashboard_api.h"
#include "big_buffer.h"
#include "sync_store.h"
#include "fw_sync.h"
#include "fw_schedule.h"
#include "community.h"
#include "fw_canvas.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstddef>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <new>

namespace {

/* Recovered from the vendor firmware (PANEL.md). These drive real GPIOs: do not
 * change any of them without new evidence from the board or vendor image. */
constexpr uint16_t PanelWidth = 64;
constexpr uint16_t PanelHeight = 64;
constexpr uint16_t ChainLength = 2;
constexpr int Width = PanelWidth * ChainLength;   // 128
constexpr int Height = PanelHeight;                // 64

const HUB75_I2S_CFG::i2s_pins Pins = {
    /* R1 G1 B1 */ 2, 6, 10,
    /* R2 G2 B2 */ 3, 7, 11,
    /* A B C D E */ 39, 38, 37, 36, 21,
    /* LAT OE CLK */ 33, 35, 34,
};

/* The vendor binary stored these raw values at these offsets (PANEL.md). If a
 * library update changes either, the recovered configuration no longer means
 * the same thing: fail the build rather than drive the panels differently. */
static_assert(HUB75_I2S_CFG::FM6126A == 2, "vendor driver value 2 must mean FM6126A");
static_assert(HUB75_I2S_CFG::TYPE138 == 0, "vendor line decoder value 0 must mean TYPE138");
static_assert(HUB75_I2S_CFG::HZ_8M == 8000000, "vendor clock literal is 8,000,000");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(HUB75_I2S_CFG, gpio) == 0x06 && offsetof(HUB75_I2S_CFG, driver) == 0x14 &&
              offsetof(HUB75_I2S_CFG, line_decoder) == 0x18 && offsetof(HUB75_I2S_CFG, double_buff) == 0x1c &&
              offsetof(HUB75_I2S_CFG, i2sspeed) == 0x20 && offsetof(HUB75_I2S_CFG, latch_blanking) == 0x24 &&
              offsetof(HUB75_I2S_CFG, clkphase) == 0x25 && offsetof(HUB75_I2S_CFG, min_refresh_rate) == 0x26,
              "HUB75_I2S_CFG layout differs from the vendor build");
#pragma GCC diagnostic pop

MatrixPanel_I2S_DMA *s_panel;
volatile uint32_t s_stack_free;
volatile uint32_t s_frames;

/* All drawing goes through the portable screens module, so the same code
 * renders here and in the desktop preview (tools/preview_screens.py). */
void panel_pixel(void *ctx, int x, int y, FwRgb c){
    static_cast<MatrixPanel_I2S_DMA *>(ctx)->drawPixelRGB888(x, y, c.r, c.g, c.b);
}

void panel_fill(void *ctx, int x, int y, int w, int h, FwRgb c){
    static_cast<MatrixPanel_I2S_DMA *>(ctx)->fillRect(x, y, w, h, c.r, c.g, c.b);
}

/* ---------------------------------------------------------------- logo */

bool asset_exists(void *, const char *path){ return fw_assets_exists(path); }

bool s_assets_mounted;

/* Decoded logos. Slot A always exists; once a logo has had to wait for the
 * shared block (a community request had it), a second slot comes from the
 * heap and the next flight's logo is decoded into it ahead, during the current
 * flight's turn, so it is ready when the flight changes. The vendor feed never
 * holds the block, so there the second slot is never made. */
FwLogo s_logo_a;
FwLogo *s_logo_b;                 /* the spare, heap, made on the first wait */
FwLogo *s_shown_logo = &s_logo_a; /* decoded logo for s_logo_code */
bool s_logo_ok;
char s_logo_code[8];
bool s_logo_pending;              /* the shown flight still needs its logo */
bool s_logo_waited;               /* a logo has had to wait at least once */
bool s_ahead_ready, s_ahead_ok;   /* the spare slot holds s_ahead_code's logo */
char s_ahead_code[8], s_ahead_class[24];

FwLogo *shown_logo(){ return s_logo_ok ? s_shown_logo : nullptr; }
FwLogo *spare_slot(){ return s_shown_logo == &s_logo_a ? s_logo_b : &s_logo_a; }

/* One file buffer for every logo read (all on this task): the largest airline
 * logo is 2,907 bytes, the system logo 1,932. */
uint8_t s_logo_file[4096];

void *take_decoder(){
    if (fw_logo_decoder_bytes() > fw_big_bytes) return nullptr;
    return fw_big_take(0);
}

/* Resolves and decodes a flight's logo the way the stock firmware does
 * (LOGOS.md), from the read-only vendor asset partition, into out. The decoder
 * (~44 KiB) lives in the shared block (big_buffer.h) meanwhile. Returns 1
 * decoded, 0 no logo, -1 the block is busy (nothing changed). */
int decode_logo(const char *al_code, const char *ac_class, FwLogo *out, bool ahead){
    char path_buf[FW_LOGO_PATH_MAX], extra[200];
    const FwElement &box = fw_layout_mini_default[0];
    int64_t t0 = esp_timer_get_time();
    if (!s_assets_mounted && !(s_assets_mounted = fw_assets_mount_partition())) {
        fw_mem_report("logo_assets_mount_failed", nullptr);
        return 0;
    }
    const char *path = fw_logo_resolve(al_code, ac_class, asset_exists, nullptr, path_buf, sizeof path_buf);
    int n = path ? fw_assets_read_file(path, s_logo_file, sizeof s_logo_file) : -1;
    if (n <= 0) { fw_mem_report("logo_read_failed", nullptr); return 0; }
    void *decoder = take_decoder();
    if (!decoder) return -1;
    int rc = fw_logo_decode(decoder, s_logo_file, (size_t)n, fw_logo_side_for_box(box.w, box.h), out);
    fw_big_give();
    std::snprintf(extra, sizeof extra, "\"rc\":%d,\"path\":\"%s\",\"file_bytes\":%d,\"drawn\":\"%dx%d\",\"ms\":%lld,\"ahead\":%s",
                  rc, path, n, out->drawn_w, out->drawn_h, (long long)((esp_timer_get_time() - t0) / 1000),
                  ahead ? "true" : "false");
    static unsigned decoded;
    if (rc != FW_LOGO_OK || ++decoded <= 3 || decoded % 50 == 0) fw_mem_report("logo_decoded", extra);   /* keep the log for hours */
    return rc == FW_LOGO_OK ? 1 : 0;
}

/* The logo for the flight about to be drawn. Only one logo is shown at a time. */
void load_logo(const char *al_code, const char *ac_class){
    std::snprintf(s_logo_code, sizeof s_logo_code, "%s", al_code);
    if (s_ahead_ready && !std::strcmp(s_ahead_code, al_code) && !std::strcmp(s_ahead_class, ac_class ? ac_class : "")) {
        s_shown_logo = spare_slot();               /* decoded ahead: swap it in */
        s_logo_ok = s_ahead_ok;
        s_ahead_ready = false;
        s_logo_pending = false;
        fw_big_want(false);
        return;
    }
    int r = decode_logo(al_code, ac_class, s_shown_logo, false);
    if (r < 0) {                                   /* busy: draw without it, try again soon */
        s_logo_code[0] = 0;
        s_logo_ok = false;
        s_logo_pending = true;
        s_logo_waited = true;
        fw_big_want(true);
        return;
    }
    s_logo_ok = r > 0;
    s_logo_pending = false;
    fw_big_want(false);
}

/* While a flight shows: the next flight's logo into the spare slot. */
void logo_ahead(const FwFlight *next){
    if (!s_logo_waited || !next || s_logo_pending) return;
    if (!std::strcmp(next->al_code, s_logo_code)) return;          /* the same logo stays */
    if (s_ahead_ready && !std::strcmp(s_ahead_code, next->al_code) && !std::strcmp(s_ahead_class, next->ac_class)) return;
    if (!s_logo_b && !(s_logo_b = static_cast<FwLogo *>(heap_caps_malloc(sizeof(FwLogo), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)))) {
        fw_mem_report("logo_spare_alloc_failed", nullptr);
        s_logo_waited = false;                      /* do without; don't try every tick */
        return;
    }
    if (fw_big_busy()) { fw_big_want(true); return; }
    s_ahead_ready = false;                          /* the spare slot is about to change */
    int r = decode_logo(next->al_code, next->ac_class, spare_slot(), true);
    if (r < 0) { fw_big_want(true); return; }
    fw_big_want(false);
    s_ahead_ok = r > 0;
    std::snprintf(s_ahead_code, sizeof s_ahead_code, "%s", next->al_code);
    std::snprintf(s_ahead_class, sizeof s_ahead_class, "%s", next->ac_class);
    s_ahead_ready = true;
}

void load_demo_logo(){ load_logo("AAL", nullptr); }

/* ---------------------------------------------------------------- clock */

/* The owner's stock clock settings as read from the board: US Eastern with
 * North American DST, LARGE, date shown day-first with the weekday, 12-hour,
 * no seconds, English. Settings storage replaces this later. */
FwClockConfig owner_clock(){
    FwClockConfig c;
    std::memset(&c, 0, sizeof c);
    c.zone.standard_offset_min = -300;
    c.zone.dst_rule = FW_DST_NORTH_AMERICA;
    c.large = c.show_date = c.day_first = c.show_weekday = true;
    c.locale = "en";
    return c;
}

FwClockConfig s_clock_config = owner_clock();   /* replaced by the stored settings when live */
FwClockFrame s_clock_prev;
bool s_clock_reported;

bool clock_set(){ return std::time(nullptr) > 1700000000; }   /* SNTP has run */

/* Stock redraws the clock every pass of its display loop: the panel is cleared
 * only when the layout changes, otherwise just the changed lines. */
void clock_refresh(const FwSurface *s){
    FwClockFrame f;
    fw_clock_frame(&s_clock_config, (int64_t)std::time(nullptr), Width, Height, &f);
    fw_clock_draw(s, &f, &s_clock_prev);
    if (!s_clock_reported && f.count == 2) {
        char extra[160];
        s_clock_reported = true;
        std::snprintf(extra, sizeof extra, "\"date\":\"%s\",\"time\":\"%s\",\"scale\":%d,\"date_at\":\"%d,%d\",\"time_at\":\"%d,%d\"",
                      f.lines[0].text, f.lines[1].text, f.scale, f.lines[0].x, f.lines[0].y, f.lines[1].x, f.lines[1].y);
        fw_mem_report("clock_drawn", extra);
    }
}

void clock_enter(const FwSurface *s){
    std::memset(&s_clock_prev, 0, sizeof s_clock_prev);   /* first frame clears */
    clock_refresh(s);
}

/* ---------------------------------------------------------------- tracked */

/* The owner's stock flight settings as read from the board: title "airline",
 * mini-v1 DEFAULT, altitude in nautical miles, speeds in mph, cardinal track,
 * distance in miles, no anchor airports. */
FwFlightSettings owner_flight_settings(){
    FwFlightSettings st;
    std::memset(&st, 0, sizeof st);
    fw_units_parse(&st.units, "nautical_miles", "mph", "mph", "cardinal", "miles");
    st.title = fw_title_parse("airline");
    st.mini = true;
    st.layout = "DEFAULT";
    return st;
}

const FwFlightSettings s_flight_settings = owner_flight_settings();

/* A sample tracked flight (not live data) shown in every stock state: the four
 * pages at the stock 5 s each, then landed and on the ground. */
constexpr int TrackedStates = 6;
int s_tracked_state;
uint32_t s_tracked_since;
unsigned s_tracked_reported;

void tracked_draw(const FwSurface *s){
    FwFlight f;
    fw_flight_init(&f);
    std::strcpy(f.cs, "AAL1234"); std::strcpy(f.al_code, "AAL"); std::strcpy(f.al_name, "American");
    std::strcpy(f.orig_iata, "CLT"); std::strcpy(f.dest_iata, "PBI");
    std::strcpy(f.orig, "Charlotte Douglas"); std::strcpy(f.dest, "Palm Beach Intl");
    std::strcpy(f.ac_name, "737-800");
    f.alt_b = 10668; f.vel = 231.5; f.trk = 172; f.vr = 0;
    f.has_trk = true; f.prog = 42; f.dist = 482803;
    std::strcpy(f.dep, "35m ago"); std::strcpy(f.arr, "1h 10m"); std::strcpy(f.over, "South Carolina");
    int page = s_tracked_state < 4 ? s_tracked_state : 0;
    if (s_tracked_state == 4) { f.prog = 100; std::strcpy(f.tft, "1h 52m"); }
    if (s_tracked_state == 5) { f.prog = 0; f.trk_gnd = true; f.vel = 8; }
    if (std::strcmp(s_logo_code, f.al_code) != 0) load_logo(f.al_code, f.ac_class);
    fw_screen_tracked(s, &s_flight_settings, &f, page, (int64_t)std::time(nullptr), shown_logo());
    if (!(s_tracked_reported & (1u << s_tracked_state))) {
        static const char *const names[TrackedStates] = {"departed", "over", "metrics", "distance", "landed", "on_ground"};
        char line[FW_FLIGHT_TEXT_MAX], extra[200];
        FwTrackState t;
        fw_track_state(&f, &t);
        s_tracked_reported |= 1u << s_tracked_state;
        if (!fw_track_text(&s_flight_settings, &f, &t, page, "footer1", 124, 0, line, sizeof line)) line[0] = 0;
        std::snprintf(extra, sizeof extra, "\"state\":\"%s\",\"footer1\":\"%s\"", names[s_tracked_state], line);
        fw_mem_report("tracked_drawn", extra);
    }
}

void tracked_enter(const FwSurface *s){
    s_tracked_state = 0;
    s_tracked_since = (uint32_t)(esp_timer_get_time() / 1000);
    tracked_draw(s);
}

void tracked_refresh(const FwSurface *s){
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_tracked_since < fw_track_page_ms(4, 1)) return;
    s_tracked_since = now;
    s_tracked_state = (s_tracked_state + 1) % TrackedStates;
    tracked_draw(s);
}

/* ---------------------------------------------------------------- several flights */

/* Five sample flights (not live data) with 7 more reported, so the stock
 * "text" behaviour shows four flights and the too-many screen, at the owner's
 * "slow" 5 s. */
FwRotation s_rotation;
unsigned s_rotation_reported;

void sample(FwFlight *f, const char *icao, const char *cs, const char *al, const char *name, const char *from,
            const char *to, const char *from_name, const char *to_name, const char *aircraft){
    fw_flight_init(f);
    std::snprintf(f->icao, sizeof f->icao, "%s", icao);
    std::snprintf(f->cs, sizeof f->cs, "%s", cs);
    std::snprintf(f->al_code, sizeof f->al_code, "%s", al);
    std::snprintf(f->al_name, sizeof f->al_name, "%s", name);
    std::snprintf(f->orig_iata, sizeof f->orig_iata, "%s", from);
    std::snprintf(f->dest_iata, sizeof f->dest_iata, "%s", to);
    std::snprintf(f->orig, sizeof f->orig, "%s", from_name);
    std::snprintf(f->dest, sizeof f->dest, "%s", to_name);
    std::snprintf(f->ac_name, sizeof f->ac_name, "%s", aircraft);
}

/* When the card now on screen was drawn: its long lines scroll from then
 * (fw_screen_scroll_tick, FLIGHT-SCREENS.md "Long text"). */
uint32_t s_card_at;
uint32_t ms_now32(){ return (uint32_t)(esp_timer_get_time() / 1000); }
void card_drawn(){ s_card_at = ms_now32(); }
/* A rotation keeps a card until its longest line has scrolled through once. */
void card_hold(FwRotation *r){
    uint32_t pass = fw_screen_scroll_pass_ms();
    r->extra_ms = pass > r->dwell_ms ? pass - r->dwell_ms : 0;
}
void card_scroll(const FwSurface *s){ if (fw_screen_scroll_count()) fw_screen_scroll_tick(s, ms_now32() - s_card_at); }

void rotation_refresh(const FwSurface *s){
    static const FwFrame frame = {false, 50, false};   /* owner: border off */
    const FwFlight *f;
    int more;
    bool redraw;
    FwShow show = fw_rotation_tick(&s_rotation, (uint32_t)(esp_timer_get_time() / 1000), &f, &more, &redraw);
    if (!redraw) { card_scroll(s); return; }
    if (show == FW_SHOW_FLIGHT && f) {
        if (std::strcmp(s_logo_code, f->al_code) != 0) load_logo(f->al_code, f->ac_class);
        fw_screen_flight(s, &s_flight_settings, f, shown_logo());
        fw_screen_frame(s, &frame);
        card_drawn();
        card_hold(&s_rotation);
    } else if (show == FW_SHOW_TOO_MANY) {
        fw_screen_too_many(s, &frame, more);
    }
    if (!(s_rotation_reported & (1u << s_rotation.index))) {
        char extra[120];
        s_rotation_reported |= 1u << s_rotation.index;
        std::snprintf(extra, sizeof extra, "\"slot\":%d,\"show\":\"%s\",\"cs\":\"%s\",\"seeing\":%d", s_rotation.index,
                      show == FW_SHOW_TOO_MANY ? "too_many" : "flight", f ? f->cs : "", more);
        fw_mem_report("rotation_drawn", extra);
    }
}

void rotation_enter(const FwSurface *s){
    static FwFlight list[FW_FEED_MAX_FLIGHTS];
    sample(&list[0], "a1b2c3", "AAL1234", "AAL", "American", "CLT", "PBI", "Charlotte Douglas", "Palm Beach Intl", "737-800");
    sample(&list[1], "a2b3c4", "DAL402", "DAL", "Delta", "ATL", "JFK", "Hartsfield-Jackson", "John F Kennedy", "A321");
    sample(&list[2], "a3b4c5", "UAL1871", "UAL", "United", "ORD", "SFO", "Chicago O'Hare", "San Francisco Intl", "787-9");
    sample(&list[3], "a4b5c6", "SWA2290", "SWA", "Southwest", "MDW", "DEN", "Chicago Midway", "Denver Intl", "737 MAX 8");
    sample(&list[4], "a5b6c7", "JBU615", "JBU", "JetBlue", "BOS", "FLL", "Boston Logan", "Fort Lauderdale", "A320");
    fw_rotation_init(&s_rotation, fw_max_flights_parse("text", false, false), fw_display_speed_ms("slow"));
    fw_rotation_set_list(&s_rotation, list, FW_FEED_MAX_FLIGHTS, 7);
    rotation_refresh(s);
}

/* ---------------------------------------------------------------- live feed */

/* Live flights from the vendor feed (feed.c), shown the way stock's display
 * loop shows them: the flights in rotation, tracked flights page by page, or
 * the loading template when there are none; settings from the board's stored
 * configuration. Until the feed has a key the demo screens run instead. */
FwFeedFrame s_live;
uint32_t s_live_serial;
bool s_live_ready, s_live_settings_ready;
FwFeedDisplay s_live_display;          /* our copy: the strings below point into it */
uint32_t s_live_display_serial;
FwFlightSettings s_live_settings;
const char *s_live_anchors[8];
FwFrame s_live_frame;
FwRotation s_live_rotation;
enum LiveMode { LiveNone, LiveRotation, LiveTracked, LiveEmpty, LiveConfigure };
LiveMode s_live_mode = LiveNone;
int s_trk_flight, s_trk_page_i, s_trk_pages_n;
uint8_t s_trk_pages[FW_TRACK_MAX_PAGES];
uint32_t s_trk_since;
bool s_live_redraw;

/* time_zone.dst_rule as stock reads it (FUN_4202526c); anything else: none. */
uint8_t dst_rule(const char *r){
    return !std::strcmp(r, "north_america") ? FW_DST_NORTH_AMERICA : !std::strcmp(r, "europe") ? FW_DST_EUROPE : FW_DST_NONE;
}

void live_settings(){
    const FwFeedDisplay *d = &s_live_display;
    s_live_display_serial = fw_feed_display_copy(&s_live_display);
    std::memset(&s_live_settings, 0, sizeof s_live_settings);
    s_live_settings.units = d->units;
    s_live_settings.title = d->title;
    s_live_settings.mini = true;
    s_live_settings.layout = d->layout;
    for (int i = 0; i < d->anchor_count; ++i) s_live_anchors[i] = d->anchors[i];
    s_live_settings.anchors = s_live_anchors;
    s_live_settings.anchor_count = d->anchor_count;
    s_live_settings.alias = fw_aliases_lookup;          /* owned feature: airport aliases */
    s_live_frame.border = d->border;
    s_live_frame.border_percent = d->border_percent;
    s_live_frame.indicator = false;
    fw_rotation_init(&s_live_rotation, d->max_flights, d->dwell_ms);
    fw_screen_scroll_speed(d->scroll_px_ms ? d->scroll_px_ms : FW_SCROLL_PX_MS);
    std::memset(&s_clock_config, 0, sizeof s_clock_config);
    s_clock_config.zone.standard_offset_min = d->utc_offset_min;
    s_clock_config.zone.dst_rule = dst_rule(d->dst_rule);
    s_clock_config.large = d->clock_large;
    s_clock_config.show_date = d->clock_show_date;
    s_clock_config.is_24h = d->clock_24h;
    s_clock_config.show_seconds = d->clock_seconds;
    s_clock_config.day_first = d->clock_day_first;
    s_clock_config.show_weekday = d->clock_weekday;
    s_clock_config.locale = d->locale;
    static_assert(FW_FEED_ZONE_UNSET == FW_ZONE_UNSET, "one 'no zone' value");
    s_clock_config.extra_count = d->extra_count < FW_CLOCK_MAX_EXTRA_ZONES ? d->extra_count : FW_CLOCK_MAX_EXTRA_ZONES;
    for (int i = 0; i < s_clock_config.extra_count; ++i) {
        FwExtraZone &x = s_clock_config.extra[i];
        std::snprintf(x.label, sizeof x.label, "%s", d->extra[i].label);
        x.zone.standard_offset_min = d->extra[i].utc_offset_min;
        x.zone.dst_rule = dst_rule(d->extra[i].dst_rule);
    }
    s_live_settings_ready = true;
}

/* No flights: the stored loading_template (TEXT "No flights...", CLOCK,
 * BLANK, ELLIPSIS). */
void live_empty(const FwSurface *s, bool enter){
    const char *t = s_live_display.loading;
    if (!std::strcmp(t, "CLOCK") && clock_set()) {
        if (enter) clock_enter(s); else clock_refresh(s);
    } else if (enter) {
        if (!std::strcmp(t, "BLANK")) fw_clear(s);
        else fw_screen_message(s, &s_live_frame, !std::strcmp(t, "ELLIPSIS") ? "..." : "No flights...");
    }
}

void live_tracked_draw(const FwSurface *s){
    const FwFlight *f = &s_live.flights[s_trk_flight];
    if (std::strcmp(s_logo_code, f->al_code) != 0) load_logo(f->al_code, f->ac_class);
    fw_screen_tracked(s, &s_live_settings, f, s_trk_pages[s_trk_page_i], (int64_t)std::time(nullptr),
                      shown_logo());
    fw_screen_frame(s, &s_live_frame);
    card_drawn();
}

void live_tracked_start(int flight){
    s_trk_flight = flight;
    s_trk_page_i = 0;
    const FwFlight &f = s_live.flights[flight];
    s_trk_pages_n = fw_track_pages(&f, f.trk_metrics, s_trk_pages);
    if (!f.over[0]) {                       /* nothing to say under "Flying over" (community source): skip it */
        int n = 0;
        for (int i = 0; i < s_trk_pages_n; ++i) if (s_trk_pages[i] != 1) s_trk_pages[n++] = s_trk_pages[i];
        s_trk_pages_n = n;
    }
    if (s_trk_pages_n < 1) { s_trk_pages[0] = 0; s_trk_pages_n = 1; }
    s_trk_since = (uint32_t)(esp_timer_get_time() / 1000);
}

void live_tracked_tick(const FwSurface *s){
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t page = fw_track_page_ms(s_trk_pages_n, s_live.count), pass = fw_screen_scroll_pass_ms();
    if (now - s_trk_since < (pass > page ? pass : page)) { card_scroll(s); return; }
    if (++s_trk_page_i >= s_trk_pages_n) live_tracked_start((s_trk_flight + 1) % s_live.count);
    else s_trk_since = now;
    live_tracked_draw(s);
}

void live_rotation_tick(const FwSurface *s){
    const FwFlight *f;
    int more;
    bool redraw;
    FwShow show = fw_rotation_tick(&s_live_rotation, (uint32_t)(esp_timer_get_time() / 1000), &f, &more, &redraw);
    if (s_logo_pending && !fw_big_busy()) s_live_redraw = true;   /* the logo can be decoded now */
    if (!redraw && !s_live_redraw) { logo_ahead(fw_rotation_next(&s_live_rotation)); card_scroll(s); return; }
    s_live_redraw = false;
    s_live_frame.indicator = s_live_rotation.indicator;
    if (show == FW_SHOW_FLIGHT && f) {
        if (std::strcmp(s_logo_code, f->al_code) != 0) load_logo(f->al_code, f->ac_class);
        fw_screen_flight(s, &s_live_settings, f, shown_logo());
        fw_screen_frame(s, &s_live_frame);
        card_drawn();
        card_hold(&s_live_rotation);
    } else if (show == FW_SHOW_TOO_MANY) {
        fw_screen_too_many(s, &s_live_frame, more);
    }
}

/* Runs the live screens; false while the feed is off (demo screens instead).
 * enter = the live screens are coming back after something else drew. */
bool live_step(const FwSurface *s, bool enter){
    FwFeedState st = fw_feed_state();
    LiveMode mode;
    bool fresh;
    if (st == FW_FEED_OFF) return false;
    if (st == FW_FEED_NO_CONFIG) {
        if (enter || s_live_mode != LiveConfigure)
            fw_screen_lines(s, nullptr, "Configure settings", "in the app to", "display flights...", {255, 255, 255});
        s_live_mode = LiveConfigure;
        return true;
    }
    if (!s_live_settings_ready || fw_feed_display_serial() != s_live_display_serial) {
        live_settings();                                 /* first time, or edited on the dashboard */
        enter = true;
    }
    fresh = fw_feed_serial() != s_live_serial;
    if (fresh) {
        s_live_serial = fw_feed_copy(&s_live);
        s_live_ready = true;
    }
    mode = !s_live_ready || !s_live.count ? LiveEmpty : s_live.tracked ? LiveTracked : LiveRotation;
    if (mode != s_live_mode || enter) {
        s_live_mode = mode;
        if (mode == LiveRotation) {
            fw_rotation_set_list(&s_live_rotation, s_live.flights, s_live.count, s_live.overflow);
            s_live_redraw = true;
            live_rotation_tick(s);
        } else if (mode == LiveTracked) {
            live_tracked_start(0);
            live_tracked_draw(s);
        } else {
            live_empty(s, true);
        }
        return true;
    }
    if (mode == LiveRotation) {
        if (fresh) fw_rotation_set_list(&s_live_rotation, s_live.flights, s_live.count, s_live.overflow);
        live_rotation_tick(s);
    } else if (mode == LiveTracked) {
        if (fresh) {
            if (s_trk_flight >= s_live.count) live_tracked_start(0);
            live_tracked_draw(s);                     /* same page, new numbers */
        }
        live_tracked_tick(s);
    } else {
        live_empty(s, false);
    }
    return true;
}

/* ---------------------------------------------------------------- Sync */

/* The owned Sync schedule on the panel (SYNC.md "Owned Sync"), with stock's
 * phases: "main" (the trip card or the message screen when a leg is active,
 * else the ordinary flights) and "upcoming" (one at a time or a list), each
 * as long as stock makes it (fw_sync_phase_ms; slot = flight_display_speed).
 * The schedule is worked out again every 15 s, so countdowns stay current. */
FwSchedView s_sv;
uint32_t s_sv_serial = UINT32_MAX, s_sv_at;
int s_sync_phase = -1;                  /* -1 off, 0 main, 1 upcoming */
uint32_t s_sync_phase_since, s_sync_phase_ms, s_sync_sub_since, s_sync_next_since;
int s_sync_sub, s_sync_next;
bool s_sync_drawn, s_sync_on;

uint32_t ms_now(){ return (uint32_t)(esp_timer_get_time() / 1000); }

/* The active leg tracked live (community source), and the pages its card
 * cycles through: stock's tracked pages without "Flying over" (as in Sync),
 * then the message. */
FwFlight s_sync_live;
bool s_sync_has_live;
uint8_t s_sync_pages[FW_TRACK_MAX_PAGES + 1];
int s_sync_page_count;

bool same_callsign(const char *a, const char *b){
    for (; *a && *b; ++a, ++b) if (std::toupper((unsigned char)*a) != std::toupper((unsigned char)*b)) return false;
    return !*a && !*b;
}

/* On the vendor feed the leg comes back as a tracked flight (feed.c asks for
 * it): the record whose callsign is the flight (stock's rule: al + fn) or its
 * ICAO form. Unlike stock, other tracked flights beside it are fine. */
bool sync_vendor_live(const char *icao, FwFlight *out){
    char iata[24], cs[24];
    if (fw_feed_serial() != s_live_serial) {           /* the ordinary screens are idle while the leg is active */
        s_live_serial = fw_feed_copy(&s_live);
        s_live_ready = true;
        s_live_mode = LiveNone;                         /* they start over with this list when they come back */
    }
    if (!s_live_ready || !s_live.tracked) return false;
    std::snprintf(iata, sizeof iata, "%s%s", s_sv.active.al, s_sv.active.fn);
    std::snprintf(cs, sizeof cs, "%s%s", icao ? icao : "", s_sv.active.fn);
    for (int i = 0; i < s_live.count; ++i)
        if (same_callsign(s_live.flights[i].cs, iata) || (icao && same_callsign(s_live.flights[i].cs, cs))) {
            *out = s_live.flights[i];
            return true;
        }
    return false;
}

void sync_live_refresh(){
    char cs[12];
    const char *icao = s_sv.has_active ? fw_sched_icao_airline(s_sv.active.al) : nullptr;
    s_sync_has_live = false;
    if (icao) {
        std::snprintf(cs, sizeof cs, "%s%s", icao, s_sv.active.fn);
        s_sync_has_live = fw_community_live(cs, &s_sync_live);
    }
    if (!s_sync_has_live && s_sv.has_active) s_sync_has_live = sync_vendor_live(icao, &s_sync_live);
    s_sync_page_count = 0;
    if (s_sync_has_live) {
        uint8_t pages[FW_TRACK_MAX_PAGES];
        int n = fw_track_pages(&s_sync_live, true, pages);
        for (int i = 0; i < n; ++i) if (pages[i] != 1) s_sync_pages[s_sync_page_count++] = pages[i];
    } else {
        s_sync_pages[s_sync_page_count++] = 0;
    }
    if (s_sv.message) s_sync_pages[s_sync_page_count++] = 4;
}

const char *sync_logo_code(const FwSyncLeg *leg){
    const char *icao = fw_sched_icao_airline(leg->al);
    return icao ? icao : leg->al;
}

uint32_t sync_phase_length(int phase){
    uint32_t slot = s_live_settings_ready ? s_live_display.dwell_ms : 3000;
    if (phase == 1) return fw_sync_phase_ms(FW_SYNC_PHASE_UPCOMING, s_sv.upcoming_count, false, slot);
    if (!s_sv.has_active) return fw_sync_phase_ms(FW_SYNC_PHASE_FLIGHTS, s_live_ready ? s_live.count : 0, false, slot);
    if (fw_sync_store_message_view()) {
        char lines[FW_SYNC_CONTEXT_MAX][40];
        return fw_sync_phase_ms(FW_SYNC_PHASE_MESSAGE_VIEW, fw_sync_context(&s_sv.active, nullptr, lines), false, slot);
    }
    if (s_sync_has_live) return fw_sync_phase_ms(FW_SYNC_PHASE_ACTIVE_LIVE, s_sync_page_count, false, slot);
    return fw_sync_phase_ms(FW_SYNC_PHASE_ACTIVE, 1, s_sv.message != nullptr, slot);
}

void sync_draw(const FwSurface *s, uint32_t now){
    int64_t t = (int64_t)std::time(nullptr);
    fw_screen_scroll_reset();                            /* the Sync list screens draw elsewhere */
    if (s_sync_phase == 1) {
        if (fw_sync_store_list_view()) fw_screen_sync_list(s, &s_live_frame, s_sv.upcoming, s_sv.upcoming_count);
        else fw_screen_sync_next(s, &s_live_frame, s_sv.upcoming, s_sv.upcoming_count, s_sync_next % s_sv.upcoming_count);
    } else if (fw_sync_store_message_view()) {
        fw_screen_sync_message(s, &s_live_frame, &s_sv.active, nullptr, s_sv.message ? s_sv.message : "", s_sync_sub, t);
    } else {
        /* live: stock's tracked pages, then the message; else page 0 and the message, 5 s each */
        int page = s_sync_pages[s_sync_sub % (s_sync_page_count ? s_sync_page_count : 1)];
        const char *code = s_sync_has_live && s_sync_live.al_code[0] ? s_sync_live.al_code : sync_logo_code(&s_sv.active);
        if (std::strcmp(s_logo_code, code) != 0) load_logo(code, nullptr);
        FwSyncLive lv = {s_sync_live.dep, s_sync_live.arr, nullptr, nullptr, s_sync_live.prog};
        fw_screen_sync_active(s, &s_live_settings, &s_sv.active, s_sync_has_live ? &s_sync_live : nullptr,
                              s_sync_has_live ? &lv : nullptr, s_sv.message, page, s_sync_sub, t, shown_logo());
        fw_screen_frame(s, &s_live_frame);
    }
    s_sync_drawn = true;
    (void)now;
}

/* True while Sync owns the panel this tick; false hands it to the ordinary
 * live screens (no schedule, or a main phase without an active leg). */
bool sync_step(const FwSurface *s, bool enter){
    uint32_t now = ms_now();
    if (!clock_set() || !fw_sync_store_enabled()) { s_sync_on = false; s_sync_phase = -1; return false; }
    if (fw_sync_store_serial() != s_sv_serial || now - s_sv_at >= 15000 || !s_sync_on) {
        bool had = s_sv.has_active;
        s_sv_serial = fw_sync_store_serial();
        s_sv_at = now;
        fw_sync_store_view((uint32_t)std::time(nullptr), &s_sv);
        if (had != s_sv.has_active) s_sync_phase = -1;          /* a leg became (in)active: start over */
        s_sync_drawn = false;                                    /* the countdown text moved on */
        sync_live_refresh();
    }
    bool upcoming = fw_sync_store_show_upcoming() && s_sv.upcoming_count > 0;
    if (!s_sv.has_active && !upcoming) { s_sync_on = false; s_sync_phase = -1; return false; }
    bool flights = s_live_ready && s_live.count > 0;
    if (!s_sync_on || s_sync_phase < 0 || now - s_sync_phase_since >= s_sync_phase_ms) {
        int next = s_sync_phase < 0 ? (s_sv.has_active || flights ? 0 : 1) : (s_sync_phase == 0 && upcoming ? 1 : 0);
        if (next == 0 && !s_sv.has_active && !flights) next = 1;    /* nothing for main: upcoming */
        if (next == 1 && !upcoming) next = 0;
        s_sync_phase = next;
        s_sync_phase_since = now;
        s_sync_phase_ms = sync_phase_length(next);
        s_sync_sub = 0;
        s_sync_sub_since = s_sync_next_since = now;
        s_sync_drawn = false;
        if (next == 0 && !s_sv.has_active) { s_sync_on = true; live_step(s, true); return true; }   /* ordinary flights */
    }
    s_sync_on = true;
    if (s_sync_phase == 0 && !s_sv.has_active) return live_step(s, enter);  /* the main phase shows the flights */
    if (s_sync_phase == 0 && now - s_sync_sub_since > FW_SYNC_SUB_ROTATION_MS) {
        ++s_sync_sub;
        s_sync_sub_since = now;
        s_sync_drawn = false;
    }
    if (s_sync_phase == 1 && !fw_sync_store_list_view()) {
        uint32_t slot = s_live_settings_ready ? s_live_display.dwell_ms : 3000;
        if (now - s_sync_next_since >= (slot > 500 ? slot : 500)) { ++s_sync_next; s_sync_next_since = now; s_sync_drawn = false; }
    }
    if (enter || !s_sync_drawn || (s_logo_pending && !fw_big_busy())) { sync_draw(s, now); card_drawn(); }
    else card_scroll(s);
    return true;
}

/* ---------------------------------------------------------------- setup */

/* The stock setup screens for this board's own identity (SETUP.md): read-only
 * from the NVS "identity" namespace the stock firmware created. Shown here as
 * a preview only; nothing is started. The name, password and QR text are
 * never logged. */
FwSetupInfo s_setup;
bool s_setup_ok;
int s_setup_state;
uint32_t s_setup_since;
bool s_setup_reported;

bool read_identity(){
    nvs_handle_t h;
    char id[16] = {0}, fp[40] = {0};
    size_t n;
    if (nvs_open("identity", NVS_READONLY, &h) != ESP_OK) return false;
    n = sizeof id;
    esp_err_t a = nvs_get_str(h, "id", id, &n);
    n = sizeof fp;
    esp_err_t b = nvs_get_str(h, "fp", fp, &n);
    nvs_close(h);
    bool ok = a == ESP_OK && b == ESP_OK && fw_setup_info(id, fp, &s_setup);
    std::memset(fp, 0, sizeof fp);
    return ok;
}

void setup_draw(const FwSurface *s){
    static const FwRgb white = {255, 255, 255};
    if (!s_setup_ok) { fw_screen_lines(s, nullptr, "WiFi", "Identity error", nullptr, white); return; }
    /* pages 0-2, then page 0 as it reads while reconnecting */
    fw_screen_setup(s, nullptr, s_setup_state % FW_SETUP_PAGES, &s_setup, s_setup_state == FW_SETUP_PAGES);
}

void setup_enter(const FwSurface *s){
    if (!s_setup_ok) s_setup_ok = read_identity();
    s_setup_state = 0;
    s_setup_since = (uint32_t)(esp_timer_get_time() / 1000);
    setup_draw(s);
    if (!s_setup_reported) {
        char extra[120];
        s_setup_reported = true;
        std::snprintf(extra, sizeof extra, "\"identity\":%s,\"ssid_len\":%u,\"password_len\":%u,\"qr_len\":%u",
                      s_setup_ok ? "true" : "false", (unsigned)std::strlen(s_setup.ssid),
                      (unsigned)std::strlen(s_setup.password), (unsigned)std::strlen(s_setup.qr));
        fw_mem_report("setup_drawn", extra);
    }
}

void setup_refresh(const FwSurface *s){
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_setup_since < FW_SETUP_PAGE_MS) return;
    s_setup_since = now;
    s_setup_state = (s_setup_state + 1) % (FW_SETUP_PAGES + 1);
    setup_draw(s);
}

struct Screen {
    void (*enter)(const FwSurface *);
    void (*refresh)(const FwSurface *);   /* every 40 ms tick, or null */
    bool (*ready)();                      /* null = always */
    unsigned ticks;                       /* how long it stays up, in 40 ms ticks */
};

/* Setup mode replaces the rotation, as stock shows only the setup pages. Owned
 * plan: the stock prompt, the standard Wi-Fi join QR held for 15 s (phones
 * need time to scan it), and the stock SSID/password page; with the stock
 * HTTPS endpoints on, the vendor QR page too. The server's short messages
 * ("Connecting...", "Wrong password?") take over meanwhile. */
struct SetupStep { int page; uint32_t ms; };
const SetupStep OwnedSetup[] = {{0, 4000}, {FW_SETUP_PAGE_JOIN_QR, 15000}, {2, 6000}};
const SetupStep VendorSetup[] = {{0, 4000}, {1, 4000}, {2, 4000}, {FW_SETUP_PAGE_JOIN_QR, 15000}};

void setup_mode(const FwSurface *s, unsigned &state, uint32_t &since, bool &showing_message, bool reconnecting){
    static const FwRgb white = {255, 255, 255};
    const SetupStep *plan = fw_setup_https() ? VendorSetup : OwnedSetup;
    const unsigned steps = fw_setup_https() ? sizeof VendorSetup / sizeof VendorSetup[0]
                                            : sizeof OwnedSetup / sizeof OwnedSetup[0];
    char l1[40], l2[40];
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (fw_setup_message(l1, sizeof l1, l2, sizeof l2)) {
        static char last1[40], last2[40];
        if (!showing_message || std::strcmp(last1, l1) || std::strcmp(last2, l2)) {
            fw_screen_lines(s, nullptr, l1, l2, nullptr, white);
            std::snprintf(last1, sizeof last1, "%s", l1);
            std::snprintf(last2, sizeof last2, "%s", l2);
        }
        showing_message = true;
        since = now;
        return;
    }
    if (state >= steps) state = 0;
    if (!showing_message && since && now - since < plan[state].ms) return;
    if (!showing_message && since) state = (state + 1) % steps;
    showing_message = false;
    since = now;
    fw_screen_setup(s, nullptr, plan[state].page, fw_setup_info_get(), reconnecting);
}

FwRgb rgb565(uint16_t c){ return fw_logo_rgb565_to_888(c); }

/* What the Wi-Fi link wants on the panel (wifi_link.c). Returns true while it
 * owns the panel; false = the normal screens. */
bool link_screen(const FwSurface *s, uint32_t &serial, FwSplash &splash, unsigned &setup_state,
                 uint32_t &setup_since, bool &setup_message){
    FwLinkView v;
    fw_wifi_link_view(&v);
    bool changed = v.serial != serial;
    serial = v.serial;
    switch (v.kind) {
    case FW_LINK_VIEW_SPLASH:
        if (changed) {
            if (v.clear) fw_clear(s);
            int n = v.line[2] ? 3 : v.line[1] ? 2 : 1;
            fw_screen_splash(s, &splash, v.line, n, rgb565(v.color));
        }
        return true;
    case FW_LINK_VIEW_LINES:
        if (changed) fw_screen_lines(s, nullptr, v.line[0], v.line[1], v.line[2], rgb565(v.color));
        return true;
    case FW_LINK_VIEW_SETUP:
        if (changed) { setup_state = 0; setup_since = 0; setup_message = false; }
        setup_mode(s, setup_state, setup_since, setup_message, v.reconnecting);
        return true;
    case FW_LINK_VIEW_NORMAL:
    default:
        /* stock's app state 2 resets the splash: its next use draws the logo */
        if (changed) splash.logo_drawn = false;
        return false;
    }
}

/* "Scan to play": a QR of the games controller. In the rotation only when the
 * dashboard's games switch is on (off by default). */
bool play_ready(){
    char url[48];
    return fw_games_in_rotation() && fw_games_url(url, sizeof url);
}

void play_enter(const FwSurface *s){
    char url[48];
    if (fw_games_qr_kind() == FW_QR_DASHBOARD) {
        if (fw_dashboard_url(url, sizeof url)) fw_screen_dashboard_qr(s, nullptr, url);
    } else if (fw_games_url(url, sizeof url)) {
        fw_screen_play(s, nullptr, url);
    }
}

/* "Scan to play" while live flights show: with the games switch on, the QR
 * for 15 s every 2 minutes, then back to the flights where they were (the
 * demo screens give it a turn of its own instead). Every turn of the
 * rotation would be too often: five flights go round in about 15 s. */
constexpr uint32_t LivePlayEveryMs = 120000, LivePlayForMs = 15000;
uint32_t s_live_play_at;          /* when the QR went up, 0 = not showing */
uint32_t s_live_play_last;        /* when the last one came down (or live began) */

enum LivePlay { LivePlayNo, LivePlayStart, LivePlayShowing, LivePlayEnd };

/* Something else took the panel (Wi-Fi screens, a sign-in code, a game): any
 * QR showing is gone, and the next waits a full 2 minutes from now. */
void live_play_wait(uint32_t now){
    s_live_play_at = 0;
    if (s_live_play_last) s_live_play_last = now ? now : 1;
}

LivePlay live_play(uint32_t now){
    if (s_live_play_at) {
        if (now - s_live_play_at < LivePlayForMs && play_ready()) return LivePlayShowing;
        s_live_play_at = 0;
        s_live_play_last = now;
        return LivePlayEnd;
    }
    if (fw_feed_state() == FW_FEED_OFF || !play_ready()) { s_live_play_last = 0; return LivePlayNo; }
    if (!s_live_play_last) { s_live_play_last = now ? now : 1; return LivePlayNo; }   /* the first after 2 min */
    if (now - s_live_play_last < LivePlayEveryMs) return LivePlayNo;
    s_live_play_at = now ? now : 1;
    return LivePlayStart;
}

/* Brightness from the owned settings, applied once a second: brightness_percent
 * (0..100 of the panel's 0..255; unset = the build default), off when
 * display_enabled is false, sleep_brightness_percent inside the sleep window
 * (local time). A game or a sign-in code wakes the panel. */
uint16_t s_brightness_applied = 0xffff;

int target_brightness(bool awake){
    const FwFeedDisplay &d = s_live_display;
    int normal = CONFIG_FW_DISPLAY_BRIGHTNESS;
    if (!s_live_settings_ready) return normal;
    if (d.brightness_percent) normal = d.brightness_percent * 255 / 100 < 1 ? 1 : d.brightness_percent * 255 / 100;
    if (awake) return normal;
    if (!d.display_enabled) return 0;
    if (d.has_sleep && clock_set()) {
        FwTime t;
        if (fw_local_time((int64_t)std::time(nullptr), s_clock_config.zone, &t)) {
            int now = t.hour * 60 + t.min, a = d.sleep_start_min, b = d.sleep_end_min;
            bool asleep = a <= b ? (now >= a && now < b) : (now >= a || now < b);   /* windows may cross midnight */
            if (asleep) return d.sleep_percent * 255 / 100;
        }
    }
    return normal;
}

void apply_brightness(bool awake){
    int v = target_brightness(awake);
    if (v == s_brightness_applied) return;
    s_brightness_applied = (uint16_t)v;
    s_panel->setBrightness8((uint8_t)v);
    char extra[48];
    std::snprintf(extra, sizeof extra, "\"brightness\":%d,\"awake\":%s", v, awake ? "true" : "false");
    fw_mem_report("brightness", extra);
}

/* A game's two frames (32 KiB) come from the shared block (big_buffer.h):
 * with the community feed on, the heap seldom has 32 KiB in one piece. The
 * feed makes no requests during a game; one already running finishes first
 * (a second or two), and meanwhile the game simply starts a moment later. */
uint16_t *take_game_frames(){
    static_assert(2u * Width * Height * sizeof(uint16_t) <= CONFIG_FW_TLS_ARENA_BYTES, "game frames fit the block");
    uint8_t *b = fw_big_take(0);
    fw_big_want(b == nullptr);
    return reinterpret_cast<uint16_t *>(b);
}

void animate(void *){
    load_demo_logo();
    const FwSurface s{s_panel, Width, Height, panel_pixel, panel_fill};
    const Screen screens[] = {
        {play_enter, nullptr, play_ready, 375},                       /* 15 s */
        {rotation_enter, rotation_refresh, nullptr, 625},             /* 4 flights + too many, 5 s each */
        {tracked_enter, tracked_refresh, nullptr, 750},               /* 6 states x 5 s */
        {clock_enter, clock_refresh, clock_set, 100},
        {setup_enter, setup_refresh, nullptr, 400},                   /* 4 pages x 4 s */
        {fw_screen_text_test, nullptr, nullptr, 100},
        {fw_screen_bars, nullptr, nullptr, 100},
    };
    constexpr unsigned Count = sizeof screens / sizeof screens[0];
    unsigned tick = 0, screen = 1, shown = 0;
    screens[screen].enter(&s);
    unsigned setup_state = 0;
    uint32_t setup_since = 0, link_serial = 0;
    bool setup_message = false, link_owned = false;
    FwSplash splash;
    std::memset(&splash, 0, sizeof splash);
    /* A game draws into an offscreen canvas and only changed pixels reach the
     * panel, so the moving ball and paddles do not flicker. The two frames
     * (32 KiB) exist only while a game is on. */
    FwCanvas canvas;
    uint16_t *game_buf = nullptr;
    bool canvas_ready = false;
    uint32_t qr_shown = 0;
    int pair_shown_left = -1;
    uint32_t heartbeat_serial = 0;
    bool heartbeat_enabled = true, heartbeat_was_enabled = true;
    for (;;) {
        char pair_code[7];
        int pair_left = 0;
        bool pairing = fw_dashboard_pair_code(pair_code, &pair_left);
        if (!pairing) pair_shown_left = -1;          /* the next code draws from scratch */
        if (tick % 25 == 0) apply_brightness(pairing || game_buf);
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (link_screen(&s, link_serial, splash, setup_state, setup_since, setup_message)) {
            link_owned = true;
            qr_shown = 0;                             /* a QR still due is drawn again afterwards */
            live_play_wait(now_ms);
            if (game_buf) fw_canvas_invalidate(&canvas);
        } else if (pairing) {
            if (pair_left != pair_shown_left) {       /* redraw once a second for the countdown */
                pair_shown_left = pair_left;
                fw_screen_pair_code(&s, pair_code, pair_left);
                apply_brightness(true);
            }
            link_owned = true;                        /* afterwards everything is drawn again */
            qr_shown = 0;
            live_play_wait(now_ms);
            if (game_buf) fw_canvas_invalidate(&canvas);
        } else if (fw_games_active() && (game_buf || (game_buf = take_game_frames()))) {
            if (!canvas_ready) {                      /* first frame: the whole panel is written */
                fw_canvas_init(&canvas, Width, Height, game_buf, game_buf + Width * Height);
                canvas_ready = true;
                fw_mem_report("game_started", nullptr);
            }
            link_owned = false;
            live_play_wait(now_ms);
            FwSurface cs = fw_canvas_surface(&canvas);
            if (fw_games_frame(&cs, (uint32_t)(esp_timer_get_time() / 1000))) fw_canvas_flush(&canvas, &s);
        } else if (game_buf) {
            fw_big_give();                            /* game over: back to the normal screens */
            game_buf = nullptr;
            canvas_ready = false;
            qr_shown = 0;
            fw_mem_report("game_ended", nullptr);
            shown = 0;
            if (!sync_step(&s, true) && !live_step(&s, true)) screens[screen].enter(&s);
        } else if (uint32_t qr = fw_games_qr_request((uint32_t)(esp_timer_get_time() / 1000))) {
            if (qr != qr_shown) {                     /* the dashboard asked for the QR (30 s), or its own address */
                qr_shown = qr;
                play_enter(&s);
            }
            live_play_wait(now_ms);                   /* no rotation QR right after it */
        } else if (qr_shown) {
            qr_shown = 0;                             /* QR time over: back to the normal screens */
            shown = 0;
            if (!sync_step(&s, true) && !live_step(&s, true)) screens[screen].enter(&s);
        } else if (link_owned) {
            link_owned = false;                       /* back to the normal screens: redraw */
            shown = 0;
            if (!sync_step(&s, true) && !live_step(&s, true)) screens[screen].enter(&s);
        } else if (LivePlay lp = live_play(now_ms)) {
            if (lp == LivePlayStart) play_enter(&s);
            else if (lp == LivePlayEnd) {             /* back to the flights: redraw */
                shown = 0;
                if (!sync_step(&s, true) && !live_step(&s, true)) screens[screen].enter(&s);
            }
        } else if (sync_step(&s, false) || live_step(&s, false)) {
            /* live flights replace the demo screens once the feed has a key */
        } else if (++shown >= screens[screen].ticks) {
            shown = 0;
            do screen = (screen + 1) % Count;
            while (screens[screen].ready && !screens[screen].ready());
            screens[screen].enter(&s);
        } else if (screens[screen].refresh) {
            screens[screen].refresh(&s);
        }
        /* Heartbeat in the bottom-right corner: blinks about twice a second,
         * so a frozen panel is obvious even on a static screen. */
        /* Read independently of live_step: this also applies on demo, setup,
         * pairing and game screens, and updates as soon as settings arrive. */
        if (fw_feed_display_serial() != heartbeat_serial) {
            FwFeedDisplay d;
            heartbeat_serial = fw_feed_display_copy(&d);
            heartbeat_enabled = d.heartbeat_enabled;
        }
        if (heartbeat_enabled) {
            bool on = (tick / 12) % 2;
            s_panel->drawPixelRGB888(Width - 1, Height - 1, on ? 255 : 0, on ? 64 : 0, 0);
        } else if (heartbeat_was_enabled) {
            s_panel->drawPixelRGB888(Width - 1, Height - 1, 0, 0, 0);
        }
        heartbeat_was_enabled = heartbeat_enabled;
        ++tick;
        ++s_frames;
        s_stack_free = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
        vTaskDelay(pdMS_TO_TICKS(game_buf ? 20 : 40));   /* a game frame per game tick */
    }
}

}  // namespace

extern "C" bool fw_display_start(void){
    char extra[96];
    fw_mem_report("display_before", nullptr);

    HUB75_I2S_CFG cfg(PanelWidth, PanelHeight, ChainLength, Pins,
                      HUB75_I2S_CFG::FM6126A, HUB75_I2S_CFG::TYPE138,
                      /*double_buff*/ false, HUB75_I2S_CFG::HZ_8M,
                      /*latch_blanking*/ 2, /*clkphase*/ false,
                      /*min_refresh_rate*/ 60, /*colour depth*/ 8);

    s_panel = new (std::nothrow) MatrixPanel_I2S_DMA(cfg);
    if (!s_panel) {
        fw_mem_report("display_alloc_failed", nullptr);
        return false;
    }
    if (!s_panel->begin()) {
        /* The library's destructor releases only the DMA bus, not the frame
         * buffers, and deleting a half-initialised object is riskier than
         * leaving it. Keep it, never draw to it; the FWMEM line shows what a
         * failed start cost. */
        s_panel = nullptr;
        fw_mem_report("display_begin_failed", nullptr);
        return false;
    }
    s_panel->setBrightness8(CONFIG_FW_DISPLAY_BRIGHTNESS);
    s_panel->clearScreen();   /* the panel task draws every screen */
    std::snprintf(extra, sizeof extra, "\"width\":%d,\"height\":%d,\"brightness\":%d",
                  Width, Height, CONFIG_FW_DISPLAY_BRIGHTNESS);
    fw_mem_report("display_ready", extra);

    if (xTaskCreate(animate, "fw_panel", 6144, nullptr, 2, nullptr) != pdPASS)
        fw_mem_report("display_task_failed", nullptr);
    return true;
}

extern "C" uint32_t fw_display_task_stack_free(void){ return s_stack_free; }
extern "C" uint32_t fw_display_frames(void){ return s_frames; }
