/* Owned settings: validate an edit and merge it; see fw_settings.h and
 * SETTINGS.md. Every editable value is checked against the choices the stock
 * firmware understands (FEED.md, CLOCK.md, FLIGHT-SCREENS.md). */
#include "fw_settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct {
    FwSettingsError *err;
    const char *now_iso;
    const cJSON *old_tracked;
} Ctx;

static bool fail(Ctx *c, const char *field, const char *why){
    if (c->err) {
        snprintf(c->err->field, sizeof c->err->field, "%s", field);
        snprintf(c->err->why, sizeof c->err->why, "%s", why);
    }
    return false;
}

static bool one_of(const cJSON *v, const char *const *choices){
    if (!cJSON_IsString(v)) return false;
    for (; *choices; ++choices)
        if (!strcmp(v->valuestring, *choices)) return true;
    return false;
}

static bool number_in(const cJSON *v, double lo, double hi){
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= lo && v->valuedouble <= hi;
}

static bool integer_in(const cJSON *v, double lo, double hi){
    return number_in(v, lo, hi) && v->valuedouble == floor(v->valuedouble);
}

static bool printable(const char *s, size_t max){
    size_t n = strlen(s);
    if (n > max) return false;
    for (; *s; ++s) if ((unsigned char)*s < 0x20) return false;
    return true;
}

/* An object with only these keys, each checked by its validator. In a
 * section (top level) a null removes the stored setting instead. */
typedef bool (*Check)(Ctx *, const char *field, const cJSON *);
typedef struct { const char *key; Check check; } Rule;

static bool keys(Ctx *c, const char *field, const cJSON *v, const Rule *rules, bool section){
    const cJSON *it;
    char path[96];
    if (!cJSON_IsObject(v)) return fail(c, field, "must be an object");
    cJSON_ArrayForEach(it, v) {
        const Rule *r = rules;
        while (r->key && strcmp(r->key, it->string)) ++r;
        snprintf(path, sizeof path, "%s.%s", field, it->string);
        if (!r->key) return fail(c, path, "unknown setting");
        if (section && cJSON_IsNull(it)) {
            if (!strcmp(it->string, "radius_requests")) return fail(c, path, "1 to 5 areas");
            continue;
        }
        if (!r->check(c, path, it)) return false;
    }
    return true;
}
static bool object(Ctx *c, const char *field, const cJSON *v, const Rule *rules){ return keys(c, field, v, rules, false); }

/* ------------------------------------------------------------ display_config */

#define ENUM(name, ...) \
    static bool name(Ctx *c, const char *f, const cJSON *v){ \
        static const char *const ok[] = {__VA_ARGS__, NULL}; \
        return one_of(v, ok) || fail(c, f, "not one of the choices"); }

ENUM(v_layout, "DEFAULT", "NO_LOGO", "TECHNICAL")
ENUM(v_loading, "TEXT", "CLOCK", "ELLIPSIS", "BLANK")
ENUM(v_title, "airline", "callsign", "registration")
ENUM(v_speed, "slow", "normal", "fast")
ENUM(v_maxf, "off", "text", "indicator_led")
ENUM(v_alt, "meters", "feet", "kilometers", "nautical_miles")
ENUM(v_vel, "kmh", "mps", "knots", "mph", "mach")
ENUM(v_vr, "kmh", "mps", "fps", "fpm", "mph")
ENUM(v_trk, "cardinal", "degrees")
ENUM(v_dist, "meters", "kilometers", "miles", "nautical_miles", "feet")
ENUM(v_dst, "none", "north_america", "europe")
ENUM(v_style, "DEFAULT", "LARGE")
ENUM(v_order, "month_day", "day_month")
ENUM(v_locale, "en", "es", "fr", "de", "it", "pt", "nl", "pl", "sv", "no", "da", "fi", "cs", "ro", "hu", "el", "tr",
     "uk", "ru")

static bool v_bool(Ctx *c, const char *f, const cJSON *v){ return cJSON_IsBool(v) || fail(c, f, "must be true or false"); }
static bool v_percent(Ctx *c, const char *f, const cJSON *v){ return integer_in(v, 0, 100) || fail(c, f, "must be 0..100"); }
static bool v_brightness(Ctx *c, const char *f, const cJSON *v){ return integer_in(v, 1, 100) || fail(c, f, "must be 1..100"); }
static bool v_offset(Ctx *c, const char *f, const cJSON *v){
    return (integer_in(v, -720, 840) && (int)v->valuedouble % 15 == 0) || fail(c, f, "must be -720..840 minutes, in 15s");
}
static bool v_label(Ctx *c, const char *f, const cJSON *v){
    return (cJSON_IsString(v) && printable(v->valuestring, 15)) || fail(c, f, "at most 15 characters");
}
static bool v_hhmm(Ctx *c, const char *f, const cJSON *v){
    const char *s = cJSON_IsString(v) ? v->valuestring : "";
    bool ok = strlen(s) == 5 && s[2] == ':' && s[0] >= '0' && s[0] <= '2' && s[1] >= '0' && s[1] <= '9' && s[3] >= '0' &&
              s[3] <= '5' && s[4] >= '0' && s[4] <= '9' && (s[0] - '0') * 10 + (s[1] - '0') < 24;
    return ok || fail(c, f, "must be HH:MM");
}

static const Rule METRIC[] = {{"alt_b_unit", v_alt}, {"vel_unit", v_vel}, {"vr_unit", v_vr}, {"trk_unit", v_trk},
                              {"dist_unit", v_dist}, {NULL, NULL}};
static bool v_metric(Ctx *c, const char *f, const cJSON *v){ return object(c, f, v, METRIC); }

static const Rule ZONE[] = {{"standard_utc_offset_minutes", v_offset}, {"dst_rule", v_dst}, {NULL, NULL}};
static bool v_zone(Ctx *c, const char *f, const cJSON *v){ return object(c, f, v, ZONE); }

/* The vendor's shape (stock FUN_420189b4): {"label", "time_zone": {...}}. */
static const Rule EXTRA[] = {{"label", v_label}, {"time_zone", v_zone}, {NULL, NULL}};
static bool v_extras(Ctx *c, const char *f, const cJSON *v){
    const cJSON *e;
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > 2) return fail(c, f, "at most 2 extra time zones");
    cJSON_ArrayForEach(e, v) if (!object(c, f, e, EXTRA)) return false;
    return true;
}
static const Rule CLOCK[] = {{"style", v_style}, {"show_date", v_bool}, {"additional_timezones", v_extras}, {NULL, NULL}};
static bool v_clock(Ctx *c, const char *f, const cJSON *v){ return object(c, f, v, CLOCK); }

static const Rule DATEF[] = {{"order", v_order}, {"show_weekday", v_bool}, {NULL, NULL}};
static bool v_datef(Ctx *c, const char *f, const cJSON *v){ return object(c, f, v, DATEF); }

static const Rule SLEEP[] = {{"start", v_hhmm}, {"end", v_hhmm}, {"sleep_brightness_percent", v_percent}, {NULL, NULL}};
static bool v_sleep(Ctx *c, const char *f, const cJSON *v){ return object(c, f, v, SLEEP); }

static bool v_anchors(Ctx *c, const char *f, const cJSON *v){
    const cJSON *a;
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > 8) return fail(c, f, "at most 8 airports");
    cJSON_ArrayForEach(a, v) {
        size_t n = cJSON_IsString(a) ? strlen(a->valuestring) : 0;
        if (n < 3 || n > 4) return fail(c, f, "airport codes are 3 or 4 letters");
        for (size_t i = 0; i < n; ++i) {
            char ch = a->valuestring[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return fail(c, f, "airport codes are A-Z, 0-9");
        }
    }
    return true;
}

static const Rule DISPLAY[] = {
    {"layout_template", v_layout}, {"loading_template", v_loading}, {"title_text", v_title},
    {"flight_display_speed", v_speed}, {"max_flights_behavior", v_maxf}, {"metric_display", v_metric},
    {"border_enable", v_bool}, {"border_brightness_percent", v_percent}, {"brightness_percent", v_brightness},
    {"anchor_airports", v_anchors}, {"time_zone", v_zone}, {"clock_is_24hr", v_bool}, {"clock_show_seconds", v_bool},
    {"clock", v_clock}, {"date_format", v_datef}, {"locale", v_locale}, {"display_enabled", v_bool},
    {"sleep_time", v_sleep}, {NULL, NULL}};

/* ------------------------------------------------------------ request_config */

static bool v_lat(Ctx *c, const char *f, const cJSON *v){ return number_in(v, -90, 90) || fail(c, f, "must be -90..90"); }
static bool v_lon(Ctx *c, const char *f, const cJSON *v){ return number_in(v, -180, 180) || fail(c, f, "must be -180..180"); }
static bool v_radius(Ctx *c, const char *f, const cJSON *v){ return number_in(v, 1, 250) || fail(c, f, "must be 1..250 km"); }
static bool v_alt_m(Ctx *c, const char *f, const cJSON *v){ return integer_in(v, 0, 20000) || fail(c, f, "must be 0..20000"); }
static bool v_name(Ctx *c, const char *f, const cJSON *v){
    return (cJSON_IsString(v) && printable(v->valuestring, 31)) || fail(c, f, "at most 31 characters");
}
static const Rule RADIUS[] = {{"latitude", v_lat}, {"longitude", v_lon}, {"radius_km", v_radius},
                              {"min_altitude", v_alt_m}, {"max_altitude", v_alt_m}, {"name", v_name}, {NULL, NULL}};
static bool v_zones(Ctx *c, const char *f, const cJSON *v){
    const cJSON *z;
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 1 || cJSON_GetArraySize(v) > 5) return fail(c, f, "1 to 5 areas");
    cJSON_ArrayForEach(z, v) {
        const cJSON *lo, *hi;
        if (!object(c, f, z, RADIUS)) return false;
        if (!cJSON_GetObjectItemCaseSensitive(z, "latitude") || !cJSON_GetObjectItemCaseSensitive(z, "longitude"))
            return fail(c, f, "every area needs latitude and longitude");
        lo = cJSON_GetObjectItemCaseSensitive(z, "min_altitude");
        hi = cJSON_GetObjectItemCaseSensitive(z, "max_altitude");
        if (lo && hi && lo->valuedouble > hi->valuedouble) return fail(c, f, "minimum altitude above maximum");
    }
    return true;
}

static bool v_speed_mps(Ctx *c, const char *f, const cJSON *v){ return integer_in(v, 0, 400) || fail(c, f, "must be 0..400"); }
static bool v_airlines(Ctx *c, const char *f, const cJSON *v){
    const cJSON *a;
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > 32) return fail(c, f, "at most 32 airlines");
    cJSON_ArrayForEach(a, v) {
        const char *s = cJSON_IsString(a) ? a->valuestring : "";
        if (strlen(s) != 3 || s[0] < 'A' || s[0] > 'Z' || s[1] < 'A' || s[1] > 'Z' || s[2] < 'A' || s[2] > 'Z')
            return fail(c, f, "airline codes are 3 capital letters");
    }
    return true;
}
static const Rule FILTERS[] = {
    {"show_light_ac", v_bool}, {"show_heli_ac", v_bool}, {"show_commercial_ac", v_bool}, {"show_smalljet_ac", v_bool},
    {"show_other_ac", v_bool}, {"show_na_routes", v_bool}, {"min_speed_mps", v_speed_mps}, {"max_speed_mps", v_speed_mps},
    {"airline_icao_filter", v_airlines}, {NULL, NULL}};
static bool v_filters(Ctx *c, const char *f, const cJSON *v){ return object(c, f, v, FILTERS); }

static bool v_flight_number(Ctx *c, const char *f, const cJSON *v){
    const char *s = cJSON_IsString(v) ? v->valuestring : "";
    size_t n = strlen(s);
    if (n < 2 || n > 11) return fail(c, f, "2 to 11 letters and digits");
    for (; *s; ++s)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))) return fail(c, f, "capital letters and digits only");
    return true;
}
static const Rule TRACKED[] = {{"flight_number", v_flight_number}, {"auto_remove", v_bool},
                               {"show_distance_travelled", v_bool}, {"show_metrics", v_bool}, {NULL, NULL}};
static bool v_tracked(Ctx *c, const char *f, const cJSON *v){
    const cJSON *t;
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > 8) return fail(c, f, "at most 8 tracked flights");
    cJSON_ArrayForEach(t, v) {
        if (!object(c, f, t, TRACKED)) return false;
        if (!cJSON_GetObjectItemCaseSensitive(t, "flight_number")) return fail(c, f, "every tracked flight needs a number");
    }
    return true;
}

/* ------------------------------------------------------------ owned_config */
/* Owned-only settings; the vendor app and stock never read this section. */
ENUM(v_source, "vendor", "community")
ENUM(v_tracking, "community", "aeroapi")
ENUM(v_scroll, "slow", "normal", "fast")
static const Rule OWNED[] = {{"flight_source", v_source}, {"follow_app", v_bool}, {"tracking_source", v_tracking},
                             {"scroll_speed", v_scroll}, {"progress_bar", v_bool},
                             {NULL, NULL}};

static const Rule REQUEST[] = {{"radius_requests", v_zones}, {"data_filters", v_filters},
                               {"tracked_flights", v_tracked}, {NULL, NULL}};

/* ------------------------------------------------------------ merge */

static void replace(cJSON *obj, const char *key, cJSON *value){
    if (cJSON_GetObjectItemCaseSensitive(obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, value);
    else cJSON_AddItemToObject(obj, key, value);
}

static cJSON *section(cJSON *root, const char *key){
    cJSON *s = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(s)) {
        s = cJSON_CreateObject();
        replace(root, key, s);
    }
    return s;
}

/* created_at of an already tracked flight, kept; new ones get now_iso */
static const char *created_at(const Ctx *c, const char *number){
    const cJSON *t;
    cJSON_ArrayForEach(t, c->old_tracked) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(t, "flight_number");
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(t, "created_at");
        if (cJSON_IsString(n) && !strcmp(n->valuestring, number) && cJSON_IsString(at)) return at->valuestring;
    }
    return c->now_iso;
}

int fw_settings_apply(const char *current, const char *edit, const char *now_iso, char **out, FwSettingsError *err){
    cJSON *root = cJSON_Parse(current), *e = cJSON_Parse(edit);
    const cJSON *dc, *rc, *oc, *it;
    Ctx c = {err, now_iso ? now_iso : "", NULL};
    char *text;
    int rv = FW_SETTINGS_INVALID;
    *out = NULL;
    if (err) memset(err, 0, sizeof *err);
    if (!cJSON_IsObject(root)) { rv = FW_SETTINGS_BAD_CURRENT; goto done; }
    if (!cJSON_IsObject(e)) { fail(&c, "", "not a JSON object"); goto done; }
    cJSON_ArrayForEach(it, e)
        if (strcmp(it->string, "display_config") && strcmp(it->string, "request_config") &&
            strcmp(it->string, "owned_config")) { fail(&c, it->string, "unknown section"); goto done; }
    dc = cJSON_GetObjectItemCaseSensitive(e, "display_config");
    rc = cJSON_GetObjectItemCaseSensitive(e, "request_config");
    if (dc && !keys(&c, "display_config", dc, DISPLAY, true)) goto done;
    if (rc && !keys(&c, "request_config", rc, REQUEST, true)) goto done;
    oc = cJSON_GetObjectItemCaseSensitive(e, "owned_config");
    if (oc && !keys(&c, "owned_config", oc, OWNED, true)) goto done;
    if (oc) {
        cJSON *o = section(root, "owned_config");
        cJSON_ArrayForEach(it, oc) {
            if (cJSON_IsNull(it)) cJSON_DeleteItemFromObjectCaseSensitive(o, it->string);
            else replace(o, it->string, cJSON_Duplicate(it, 1));
        }
    }

    if (dc) {
        cJSON *d = section(root, "display_config");
        cJSON_ArrayForEach(it, dc) {
            if (cJSON_IsNull(it)) cJSON_DeleteItemFromObjectCaseSensitive(d, it->string);
            else replace(d, it->string, cJSON_Duplicate(it, 1));
        }
    }
    if (rc) {
        cJSON *r = section(root, "request_config");
        c.old_tracked = cJSON_GetObjectItemCaseSensitive(r, "tracked_flights");
        cJSON_ArrayForEach(it, rc) {
            cJSON *copy, *t;
            if (cJSON_IsNull(it)) { cJSON_DeleteItemFromObjectCaseSensitive(r, it->string); continue; }
            copy = cJSON_Duplicate(it, 1);
            if (!strcmp(it->string, "radius_requests")) {
                /* the list is now the source: drop the singular and the legacy form */
                cJSON_ArrayForEach(t, copy) cJSON_DeleteItemFromObjectCaseSensitive(t, "type");
                cJSON_DeleteItemFromObjectCaseSensitive(r, "radius_request");
                cJSON_DeleteItemFromObjectCaseSensitive(r, "type");
            }
            if (!strcmp(it->string, "tracked_flights")) {
                cJSON_ArrayForEach(t, copy) {
                    const char *n = cJSON_GetObjectItemCaseSensitive(t, "flight_number")->valuestring;
                    cJSON_AddStringToObject(t, "created_at", created_at(&c, n));
                }
            }
            replace(r, it->string, copy);
        }
    }
    text = cJSON_PrintUnformatted(root);
    if (!text) { rv = FW_SETTINGS_TOO_LARGE; goto done; }
    if (strlen(text) > FW_SETTINGS_MAX) {
        cJSON_free(text);
        fail(&c, "", "settings too large");
        rv = FW_SETTINGS_TOO_LARGE;
        goto done;
    }
    *out = malloc(strlen(text) + 1);
    if (*out) { strcpy(*out, text); rv = FW_SETTINGS_OK; }
    else rv = FW_SETTINGS_TOO_LARGE;
    cJSON_free(text);
done:
    cJSON_Delete(e);
    cJSON_Delete(root);
    return rv;
}

const char *fw_settings_default(void){
    return "{\"version\":2,\"request_config\":{},\"display_config\":{}}";
}
