/* Following the FlightWall app (components/fw_settings/fw_cloud.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "fw_cloud.h"

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* The value at a dotted path, printed ("" when absent). */
static const char *at(const char *doc, const char *path){
    static char out[512];
    char part[64];
    cJSON *root = cJSON_Parse(doc), *v = root;
    const char *p = path;
    out[0] = 0;
    while (v && *p) {
        size_t n = strcspn(p, ".");
        snprintf(part, sizeof part, "%.*s", (int)n, p);
        v = cJSON_GetObjectItemCaseSensitive(v, part);
        p += n + (p[n] == '.');
    }
    if (v) { char *t = cJSON_PrintUnformatted(v); snprintf(out, sizeof out, "%s", t); cJSON_free(t); }
    cJSON_Delete(root);
    return out;
}

#define APP0 "{\"version\":2,\"request_config\":{\"radius_request\":{\"latitude\":1,\"longitude\":2,\"radius_km\":10}," \
             "\"data_filters\":{\"show_heli_ac\":true}},\"display_config\":{\"brightness_percent\":50," \
             "\"layout_template\":\"DEFAULT\",\"sleep_time\":{\"start\":\"23:00\",\"end\":\"07:00\"}},\"plus\":{\"sync\":{}}}"

int main(void){
    FwCloudBase base0, base, b2;
    char *doc;
    int changed, rc;

    /* the first configuration ever seen: fingerprints only, nothing copied */
    const char *owned = "{\"version\":2,\"request_config\":{\"radius_requests\":[{\"latitude\":5,\"longitude\":6}]},"
                        "\"display_config\":{\"brightness_percent\":80,\"layout_template\":\"NO_LOGO\"},"
                        "\"owned_config\":{\"flight_source\":\"community\"}}";
    rc = fw_cloud_merge(owned, APP0, NULL, false, &base0, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && !doc && changed == 0);
    CHECK(base0.magic == FW_CLOUD_MAGIC && base0.count == 7);  /* version, data_filters, #areas, 3 display, plus.sync */
    {   /* a wrong magic counts as none */
        FwCloudBase junk = base0;
        junk.magic = 0;
        rc = fw_cloud_merge(owned, APP0, &junk, false, &base, &doc, &changed);
        CHECK(rc == FW_CLOUD_OK && !doc && changed == 0);
    }

    /* the same configuration again: nothing to do */
    rc = fw_cloud_merge(owned, APP0, &base0, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && !doc && changed == 0 && fw_cloud_same(&base0, &base));

    /* brightness changed in the app: it wins for that setting; the dashboard's
     * layout, areas and owned_config stay */
    const char *app1 = "{\"version\":2,\"request_config\":{\"radius_request\":{\"latitude\":1,\"longitude\":2,\"radius_km\":10},"
                       "\"data_filters\":{\"show_heli_ac\":true}},\"display_config\":{\"brightness_percent\":30,"
                       "\"layout_template\":\"DEFAULT\",\"sleep_time\":{\"start\":\"23:00\",\"end\":\"07:00\"}},\"plus\":{\"sync\":{}}}";
    rc = fw_cloud_merge(owned, app1, &base0, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && doc && changed == 1);
    CHECK(!fw_cloud_same(&base0, &base));
    if (doc) {
        CHECK(!strcmp(at(doc, "display_config.brightness_percent"), "30"));
        CHECK(!strcmp(at(doc, "display_config.layout_template"), "\"NO_LOGO\""));
        CHECK(!strcmp(at(doc, "request_config.radius_requests"), "[{\"latitude\":5,\"longitude\":6}]"));
        CHECK(!strcmp(at(doc, "request_config.radius_request"), ""));
        CHECK(!strcmp(at(doc, "owned_config.flight_source"), "\"community\""));
        CHECK(!strcmp(at(doc, "display_config.sleep_time"), ""));   /* never in the owned copy, unchanged in the app */
        free(doc);
    }

    /* applied once: the next fetch with the same app settings changes nothing */
    b2 = base;
    rc = fw_cloud_merge(owned, app1, &b2, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && !doc && changed == 0);

    /* the app removed sleep_time and added a locale; the owned copy follows */
    const char *owned2 = "{\"version\":2,\"request_config\":{},\"display_config\":{\"brightness_percent\":80,"
                         "\"sleep_time\":{\"start\":\"22:00\",\"end\":\"06:00\"}}}";
    const char *app2 = "{\"version\":2,\"request_config\":{\"radius_request\":{\"latitude\":1,\"longitude\":2,\"radius_km\":10},"
                       "\"data_filters\":{\"show_heli_ac\":true}},\"display_config\":{\"brightness_percent\":50,"
                       "\"layout_template\":\"DEFAULT\",\"locale\":\"fr\"},\"plus\":{\"sync\":{}}}";
    rc = fw_cloud_merge(owned2, app2, &base0, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && doc && changed == 2);
    if (doc) {
        CHECK(!strcmp(at(doc, "display_config.sleep_time"), ""));
        CHECK(!strcmp(at(doc, "display_config.locale"), "\"fr\""));
        CHECK(!strcmp(at(doc, "display_config.brightness_percent"), "80"));   /* unchanged in the app */
        free(doc);
    }

    /* the app moved the area: all of the dashboard's area keys give way to it */
    const char *app3 = "{\"version\":2,\"request_config\":{\"radius_request\":{\"latitude\":9,\"longitude\":9,\"radius_km\":20},"
                       "\"data_filters\":{\"show_heli_ac\":true}},\"display_config\":{\"brightness_percent\":50,"
                       "\"layout_template\":\"DEFAULT\",\"sleep_time\":{\"start\":\"23:00\",\"end\":\"07:00\"}},\"plus\":{\"sync\":{}}}";
    rc = fw_cloud_merge(owned, app3, &base0, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && doc && changed == 1);
    if (doc) {
        CHECK(!strcmp(at(doc, "request_config.radius_requests"), ""));
        CHECK(!strcmp(at(doc, "request_config.radius_request.latitude"), "9"));
        CHECK(!strcmp(at(doc, "display_config.brightness_percent"), "80"));
        free(doc);
    }

    /* the app dropped every area: the dashboard's go too */
    const char *app4 = "{\"version\":2,\"request_config\":{\"data_filters\":{\"show_heli_ac\":true}},"
                       "\"display_config\":{\"brightness_percent\":50,\"layout_template\":\"DEFAULT\","
                       "\"sleep_time\":{\"start\":\"23:00\",\"end\":\"07:00\"}},\"plus\":{\"sync\":{}}}";
    rc = fw_cloud_merge(owned, app4, &base0, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && doc && changed == 1);
    if (doc) { CHECK(!strcmp(at(doc, "request_config.radius_requests"), "")); free(doc); }

    /* a type change counts (50 vs "50"), as does the top-level version */
    const char *app5 = "{\"version\":3,\"request_config\":{\"radius_request\":{\"latitude\":1,\"longitude\":2,\"radius_km\":10},"
                       "\"data_filters\":{\"show_heli_ac\":true}},\"display_config\":{\"brightness_percent\":\"50\","
                       "\"layout_template\":\"DEFAULT\",\"sleep_time\":{\"start\":\"23:00\",\"end\":\"07:00\"}},\"plus\":{\"sync\":{}}}";
    rc = fw_cloud_merge(owned, app5, &base0, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && doc && changed == 2);
    if (doc) { CHECK(!strcmp(at(doc, "version"), "3")); CHECK(!strcmp(at(doc, "display_config.brightness_percent"), "\"50\"")); free(doc); }

    /* tracked flights changed in the app: the list is replaced */
    const char *owned6 = "{\"request_config\":{\"tracked_flights\":[{\"flight_number\":\"DL1\"}]},\"display_config\":{}}";
    const char *app6a = "{\"request_config\":{\"tracked_flights\":[{\"flight_number\":\"DL1\"}]},\"display_config\":{}}";
    const char *app6b = "{\"request_config\":{\"tracked_flights\":[{\"flight_number\":\"AA22\"}]},\"display_config\":{}}";
    fw_cloud_merge(owned6, app6a, NULL, false, &b2, &doc, &changed);
    rc = fw_cloud_merge(owned6, app6b, &b2, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && doc && changed == 1);
    if (doc) { CHECK(!strcmp(at(doc, "request_config.tracked_flights"), "[{\"flight_number\":\"AA22\"}]")); free(doc); }

    /* owned_config in the app's document is never taken */
    const char *app7 = "{\"request_config\":{},\"display_config\":{},\"owned_config\":{\"flight_source\":\"vendor\"}}";
    fw_cloud_merge(owned, "{\"request_config\":{},\"display_config\":{}}", NULL, false, &b2, &doc, &changed);
    rc = fw_cloud_merge(owned, app7, &b2, false, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && !doc && changed == 0);
    rc = fw_cloud_merge(owned, app7, NULL, true, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && doc);
    if (doc) {
        /* full import: the app's document, with the owned owned_config */
        CHECK(!strcmp(at(doc, "owned_config.flight_source"), "\"community\""));
        CHECK(!strcmp(at(doc, "display_config"), "{}"));
        CHECK(!strcmp(at(doc, "version"), ""));
        free(doc);
    }
    rc = fw_cloud_merge(owned, APP0, &base0, true, &base, &doc, &changed);
    CHECK(rc == FW_CLOUD_OK && doc && changed == 7 && fw_cloud_same(&base, &base0));
    if (doc) {
        CHECK(!strcmp(at(doc, "display_config.layout_template"), "\"DEFAULT\""));
        CHECK(!strcmp(at(doc, "request_config.radius_requests"), ""));
        CHECK(!strcmp(at(doc, "owned_config.flight_source"), "\"community\""));
        free(doc);
    }
    /* full import of what is already there: no document to save */
    {
        const char *same = "{\"request_config\":{},\"display_config\":{}}";
        rc = fw_cloud_merge(same, same, NULL, true, &base, &doc, &changed);
        CHECK(rc == FW_CLOUD_OK && !doc);
    }

    /* what stock would not use is refused */
    CHECK(fw_cloud_merge(owned, "{}", &base0, false, &base, &doc, &changed) == FW_CLOUD_BAD_CONFIG && !doc);
    CHECK(fw_cloud_merge(owned, "{\"request_config\":{}}", &base0, false, &base, &doc, &changed) == FW_CLOUD_BAD_CONFIG);
    CHECK(fw_cloud_merge(owned, "{\"request_config\":[],\"display_config\":{}}", &base0, false, &base, &doc, &changed) == FW_CLOUD_BAD_CONFIG);
    CHECK(fw_cloud_merge(owned, "not json", &base0, false, &base, &doc, &changed) == FW_CLOUD_BAD_CONFIG);
    CHECK(fw_cloud_merge("[]", app1, &base0, false, &base, &doc, &changed) == FW_CLOUD_BAD_OWNED && !doc);
    /* a broken owned copy does not matter when nothing changed */
    CHECK(fw_cloud_merge("[]", APP0, &base0, false, &base, &doc, &changed) == FW_CLOUD_OK);

    /* too many settings */
    {
        char big[4096];
        size_t n = (size_t)snprintf(big, sizeof big, "{\"request_config\":{},\"display_config\":{");
        for (int i = 0; i < FW_CLOUD_UNITS + 1; ++i) n += (size_t)snprintf(big + n, sizeof big - n, "%s\"k%d\":1", i ? "," : "", i);
        snprintf(big + n, sizeof big - n, "}}");
        CHECK(fw_cloud_merge(owned, big, NULL, false, &base, &doc, &changed) == FW_CLOUD_TOO_MANY);
    }
    /* a result past the stored limit is refused */
    {
        static char big[6000], app[6200];
        memset(big, 'x', 4200);
        big[4200] = 0;
        snprintf(app, sizeof app, "{\"request_config\":{},\"display_config\":{\"locale\":\"%s\"}}", big);
        fw_cloud_merge(owned, "{\"request_config\":{},\"display_config\":{}}", NULL, false, &b2, &doc, &changed);
        CHECK(fw_cloud_merge(owned, app, &b2, false, &base, &doc, &changed) == FW_CLOUD_TOO_LARGE && !doc && changed == 0);
    }

    printf("%s: %d cloud checks\n", failed ? "FAIL" : "PASS", checks);
    return failed != 0;
}
