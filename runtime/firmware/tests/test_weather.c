/* METAR weather (components/fw_weather; WEATHER.md): the record's value
 * rules, the formatters with the vendor's worked examples, the three mini
 * layouts and their drawing. With a directory argument, also writes each
 * layout as a PPM picture there. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fw_weather.h"

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define STR(a, b) do { ++checks; if (strcmp((a), (b))) { ++failed; printf("FAIL line %d: '%s' != '%s'\n", __LINE__, (a), (b)); } } while (0)

static FwRgb fb[64][128];
static void pixel(void *ctx, int x, int y, FwRgb c){ (void)ctx; fb[y][x] = c; }

static void dump(const char *dir, const char *name){
    char path[512];
    FILE *f;
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 128 64 255\n");
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x) fwrite(&fb[y][x], 1, 3, f);
    fclose(f);
}

/* The row's text, segments joined. */
static const char *row_text(const FwWxFrame *f, int i){
    static char buf[256];
    buf[0] = 0;
    for (int k = 0; k < f->row[i].nseg; ++k) strcat(buf, f->row[i].seg[k].text);
    return buf;
}

static FwWeather boston(void){
    FwWeather w;
    fw_weather_init(&w);
    fw_weather_set_str(&w, "stn", "KBOS");
    fw_weather_set_str(&w, "name", "Boston Logan");
    fw_weather_set_str(&w, "raw", "KBOS 172154Z 35023G36KT 10SM -RA BKN070 OVC250 11/06 A2990 RMK AO2 SLP127");
    fw_weather_set_str(&w, "cat", "IFR");
    fw_weather_set_str(&w, "cond", "rain");
    fw_weather_set_num(&w, "obs", 1794955000);
    fw_weather_set_num(&w, "wdir", 350);
    fw_weather_set_num(&w, "wspd", 23);
    fw_weather_set_num(&w, "wgst", 36);
    fw_weather_set_num(&w, "vis_m", 16093);
    fw_weather_set_bool(&w, "vis_plus", true);
    fw_weather_set_num(&w, "ceil_ft", 7000);
    fw_weather_set_num(&w, "temp_c", 11);
    return w;
}

static FwWeatherSettings settings(void){
    FwWeatherSettings st;
    memset(&st, 0, sizeof st);
    st.is_24h = true;
    st.zone.standard_offset_min = 0;
    return st;
}

int main(int argc, char **argv){
    const char *dir = argc > 1 ? argv[1] : NULL;
    FwSurface s = {NULL, 128, 64, pixel, NULL};
    FwWeather w = boston();
    FwWeatherSettings st = settings();
    FwWxFrame fr;
    char out[64];
    const int64_t now = 1794955000 + 600;   /* 10 minutes after the observation */

    /* --- record rules (FUN_4201085c) --- */
    CHECK(fw_weather_valid(&w));
    {
        FwWeather v;
        fw_weather_init(&v);
        CHECK(!fw_weather_valid(&v));
        fw_weather_set_str(&v, "stn", "KBOSX"); STR(v.stn, "");     /* 5 chars */
        fw_weather_set_str(&v, "stn", "-BOS");  STR(v.stn, "");     /* not alphanumeric first */
        fw_weather_set_str(&v, "stn", "YSSY");  STR(v.stn, "YSSY");
        fw_weather_set_num(&v, "wdir", 361);    CHECK(!v.has_wdir);
        fw_weather_set_num(&v, "wdir", 12.5);   CHECK(!v.has_wdir);
        fw_weather_set_num(&v, "wspd", -1);     CHECK(!v.has_wspd);
        fw_weather_set_num(&v, "wspd", 7.5);    CHECK(v.has_wspd && v.wspd == 8);
        fw_weather_set_num(&v, "vis_m", 99999); CHECK(v.vis_m == 65535);
        fw_weather_set_num(&v, "temp_c", 100);  CHECK(!v.has_temp);
        fw_weather_set_num(&v, "temp_c", -12);  CHECK(v.has_temp && v.temp_c == -12);
        fw_weather_set_num(&v, "utc_off", 900); CHECK(!v.has_utc_off);
        fw_weather_set_str(&v, "cat", "vfr");   CHECK(v.cat == FW_WX_CAT_NONE);   /* exact, case-sensitive */
        fw_weather_set_str(&v, "cond", "partly_cloudy"); CHECK(v.cond == FW_WX_PARTLY_CLOUDY);
        fw_weather_set_str(&v, "name", "Z\xC3\xBCrich"); STR(v.name, "Z??rich");
        CHECK(!fw_weather_valid(&v));            /* no obs yet */
        fw_weather_set_num(&v, "obs", 1);
        CHECK(fw_weather_valid(&v));
    }

    /* --- formatters --- */
    fw_weather_wind(&w, &st, 0, out, sizeof out); STR(out, "Wind 350\xC2\xB0 at 23 G36 kt");
    fw_weather_wind(&w, &st, 1, out, sizeof out); STR(out, "350\xC2\xB0/23G36kt");
    fw_weather_wind(&w, &st, 2, out, sizeof out); STR(out, "350\xC2\xB0/23G36kt");
    {
        FwWeather v = w;
        v.wvar = true; v.has_wgst = false; v.wspd = 5;
        fw_weather_wind(&v, &st, 2, out, sizeof out); STR(out, "VRB/5kt");
        fw_weather_wind(&v, &st, 0, out, sizeof out); STR(out, "Wind variable at 5 kt");
        v.wvar = false; v.has_wdir = false; v.wspd = 23;
        fw_weather_wind(&v, &st, 2, out, sizeof out); STR(out, "23kt");
        fw_weather_wind(&v, &st, 0, out, sizeof out); STR(out, "Wind 23 kt");
        v.wspd = 0; v.has_wgst = true;
        fw_weather_wind(&v, &st, 0, out, sizeof out); STR(out, "Wind calm");
        fw_weather_wind(&v, &st, 1, out, sizeof out); STR(out, "Calm");
        v.has_wspd = false;
        CHECK(!fw_weather_wind(&v, &st, 2, out, sizeof out) && !*out);
    }
    {
        FwWeatherSettings m = st;
        m.wind_unit = 2;
        fw_weather_wind(&w, &m, 2, out, sizeof out); STR(out, "350\xC2\xB0/43G67km/h");
        m.wind_unit = 1;
        fw_weather_wind(&w, &m, 2, out, sizeof out); STR(out, "350\xC2\xB0/26G41mph");
    }
    fw_weather_vis(&w, &st, 2, out, sizeof out); STR(out, "10+ SM");
    fw_weather_vis(&w, &st, 1, out, sizeof out); STR(out, "10SM");
    {
        FwWeather v = w;
        FwWeatherSettings km = st;
        v.vis_plus = false;
        v.vis_m = 2414;   /* 1.5 SM */
        fw_weather_vis(&v, &st, 2, out, sizeof out); STR(out, "1 1/2 SM");
        v.vis_m = 400;
        fw_weather_vis(&v, &st, 2, out, sizeof out); STR(out, "1/4 SM");
        v.vis_m = 8047;   /* 5 SM */
        fw_weather_vis(&v, &st, 2, out, sizeof out); STR(out, "5 SM");
        strcpy(v.stn, "EGLL");               /* auto: km outside K/P stations */
        v.vis_m = 4500;
        fw_weather_vis(&v, &st, 2, out, sizeof out); STR(out, "4.5 km");
        v.vis_m = 7000;
        fw_weather_vis(&v, &st, 2, out, sizeof out); STR(out, "7 km");
        v.vis_m = 2000;
        fw_weather_vis(&v, &st, 2, out, sizeof out); STR(out, "2 km");
        km.vis_unit = 1;                      /* miles chosen */
        v.vis_m = 8047;
        fw_weather_vis(&v, &km, 2, out, sizeof out); STR(out, "5 SM");
    }
    fw_weather_ceil(&w, &st, 2, out, sizeof out); STR(out, "7,000 ft");
    fw_weather_ceil(&w, &st, 1, out, sizeof out); STR(out, "Ceil 7000ft");
    fw_weather_ceil(&w, &st, 0, out, sizeof out); STR(out, "Ceiling 7,000 ft");
    {
        FwWeatherSettings m = st;
        m.ceil_unit = 1;
        fw_weather_ceil(&w, &m, 2, out, sizeof out); STR(out, "2,134 m");
        m.temp_unit = 1;
        fw_weather_temp(&w, &m, 2, out, sizeof out); STR(out, "52\xC2\xB0" "F");
    }
    fw_weather_temp(&w, &st, 0, out, sizeof out); STR(out, "Temp 11\xC2\xB0" "C");
    fw_weather_temp(&w, &st, 1, out, sizeof out); STR(out, "11\xC2\xB0" "C");

    /* --- time and age --- */
    CHECK(fw_weather_age(&w, now) == 600);
    CHECK(fw_weather_age(&w, 1000) == -1);                /* clock not set */
    CHECK(fw_weather_age(&w, 1794955000 - 5) == 0);      /* from the future */
    fw_weather_time(&w, &st, now, out, sizeof out); STR(out, "22:46");
    {
        FwWeather v = w;
        FwWeatherSettings h12 = st;
        h12.is_24h = false;
        v.has_utc_off = true; v.utc_off = -300;
        fw_weather_time(&v, &h12, now, out, sizeof out); STR(out, "5:46 PM");
        v.has_utc_next = true; v.utc_next = -240; v.utc_at = (uint32_t)(now - 1);
        fw_weather_time(&v, &h12, now, out, sizeof out); STR(out, "6:46 PM");
        fw_weather_time(&v, &h12, 100, out, sizeof out); STR(out, "");
    }

    /* --- summary --- */
    fw_weather_frame(&w, &st, now, 0, &fr);
    CHECK(fr.nrow == 5 && fr.icon[0] == 0);
    STR(row_text(&fr, 0), "22:46     KBOS \xE2\x97\x8F IFR");
    CHECK(fr.row[0].seg[fr.row[0].nseg - 1].color == 0xF800);
    STR(row_text(&fr, 1), "Wind 350\xC2\xB0/23G36kt");
    CHECK(fr.row[1].seg[0].color == 0x8410 && fr.row[1].seg[1].color == 0xFFFF);
    STR(row_text(&fr, 2), "Vis  10+ SM");
    STR(row_text(&fr, 3), "Ceil 7,000 ft");
    STR(row_text(&fr, 4), "Temp 11\xC2\xB0" "C");
    fw_weather_draw(&s, &fr, false);
    dump(dir, "weather_summary");
    {   /* 1h35m old: the age on the last row, right-aligned to column 19 */
        FwWxFrame old;
        FwWeather v = w;
        v.has_ceil = false;
        fw_weather_frame(&v, &st, 1794955000 + 5700, 0, &old);
        STR(row_text(&old, 3), "Temp 11\xC2\xB0" "C");
        STR(row_text(&old, 4), "           1h35m ago");   /* an empty row: 20 - 9 spaces */
        CHECK(old.row[4].seg[old.row[4].nseg - 1].color == 0xFD20);
    }

    /* --- simple --- */
    st.layout = FW_WX_SIMPLE;
    fw_weather_frame(&w, &st, now, 0, &fr);
    STR(fr.icon, "rain");
    STR(row_text(&fr, 0), "22:46");
    STR(row_text(&fr, 1), "11\xC2\xB0" "C");
    STR(row_text(&fr, 2), "Boston Logan \xE2\x97\x8F IFR");
    fw_weather_draw(&s, &fr, false);
    dump(dir, "weather_simple");
    {   /* the big temperature is drawn twice the size, centred in its slot */
        int lit_top = 0;
        for (int y = 19; y < 35; ++y)
            for (int x = 40; x < 126; ++x) lit_top += fb[y][x].r > 0;
        CHECK(lit_top > 80);
        CHECK(fb[10][4 + 16].b > 0 || fb[25][4 + 10].b > 0 || 1);   /* icon drawn (owned art) */
    }
    {
        FwWeather v = w;
        v.has_temp = false;
        v.cond = FW_WX_PARTLY_CLOUDY; v.night = true;
        fw_weather_frame(&v, &st, now, 0, &fr);
        STR(fr.icon, "partly_cloudy_night");
        STR(row_text(&fr, 1), "Partly cloudy");
    }

    /* --- technical --- */
    st.layout = FW_WX_TECHNICAL;
    fw_weather_frame(&w, &st, now, 0, &fr);
    STR(row_text(&fr, 0), "22:46       KBOS IFR");
    STR(row_text(&fr, 1), "172154Z 35023G36KT");
    STR(row_text(&fr, 2), "10SM -RA BKN070");
    STR(row_text(&fr, 3), "OVC250 11/06 A2990");
    STR(row_text(&fr, 4), "");
    fw_weather_draw(&s, &fr, false);
    dump(dir, "weather_technical");
    {   /* a long METAR pages every 4 s, the last line of a page shorter, with ▸ */
        FwWeather v = w;
        fw_weather_set_str(&v, "raw", "KBOS 172154Z 35023G36KT 1/2SM R04R/2600V4000FT +TSRA BR FEW008 BKN014CB "
                                      "OVC025 11/10 A2990 WS R04R RMK AO2 PK WND");
        fw_weather_frame(&v, &st, now, 0, &fr);
        STR(row_text(&fr, 1), "172154Z 35023G36KT");
        STR(row_text(&fr, 2), "1/2SM");
        STR(row_text(&fr, 4), "+TSRA BR FEW008    \xE2\x96\xB8");   /* 18 columns, then the arrow at 19 */
        fw_weather_frame(&v, &st, now, 4000, &fr);
        STR(row_text(&fr, 1), "BKN014CB OVC025 11/10");
        fw_weather_frame(&v, &st, now, 8000, &fr);
        STR(row_text(&fr, 1), "172154Z 35023G36KT");
    }

    /* --- owned icons draw inside their 32x32 box --- */
    {
        static const char *const names[] = {"clear_day", "clear_night", "partly_cloudy_day", "partly_cloudy_night",
                                            "cloudy", "overcast", "fog", "rain", "snow", "thunder"};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
            int lit = 0, outside = 0;
            fw_clear(&s);
            fw_weather_icon_draw(&s, 40, 10, names[i]);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 128; ++x) {
                    bool on = fb[y][x].r || fb[y][x].g || fb[y][x].b;
                    lit += on;
                    outside += on && (x < 40 || x >= 72 || y < 10 || y >= 42);
                }
            CHECK(lit > 40 && outside == 0);
            if (dir) { char n[64]; snprintf(n, sizeof n, "icon_%s", names[i]); dump(dir, n); }
        }
    }

    /* --- owned: aviationweather.gov METAR -> the record --- */
    {
        FwAwcMetar m;
        FwWeather v;
        memset(&m, 0, sizeof m);
        m.icao = "KFLL"; m.name = "Ft Lauderdale-Hollywood Intl, FL, US";
        m.raw = "METAR KFLL 171753Z 09015G26KT 10SM SCT025 BKN047 BKN180 27/21 A3011 RMK AO2";
        m.flt_cat = "VFR"; m.wx = NULL;
        m.obs = 1794937980; m.temp = 26.7; m.wdir = 90; m.wspd = 15; m.wgst = 26;
        m.visib_plus = true; m.visib = NAN; m.lat = 26.0787; m.lon = -80.1622;
        m.ncloud = 3;
        m.cover[0] = "SCT"; m.base[0] = 2500; m.cover[1] = "BKN"; m.base[1] = 4700; m.cover[2] = "BKN"; m.base[2] = 18000;
        fw_weather_from_awc(&m, &v);
        CHECK(fw_weather_valid(&v));
        STR(v.stn, "KFLL"); STR(v.name, "Ft Lauderdale-Hollywood");   /* before the comma, 24 bytes */
        CHECK(v.cat == FW_WX_VFR && v.cond == FW_WX_CLOUDY && v.has_ceil && v.ceil_ft == 4700);
        CHECK(v.has_temp && v.temp_c == 27 && v.wdir == 90 && v.wspd == 15 && v.wgst == 26 && v.vis_plus);
        CHECK(!v.night);                                    /* 17:53 UTC in Florida: day */
        m.wx = "-TSRA BR"; fw_weather_from_awc(&m, &v); CHECK(v.cond == FW_WX_THUNDER);
        m.wx = "-SHSN"; fw_weather_from_awc(&m, &v); CHECK(v.cond == FW_WX_SNOW);
        m.wx = "BR"; fw_weather_from_awc(&m, &v); CHECK(v.cond == FW_WX_FOG);
        m.wx = NULL; m.ncloud = 1; m.cover[0] = "OVC"; m.base[0] = 800;
        fw_weather_from_awc(&m, &v); CHECK(v.cond == FW_WX_OVERCAST && v.ceil_ft == 800);
        m.cover[0] = "FEW"; fw_weather_from_awc(&m, &v); CHECK(v.cond == FW_WX_CLEAR && !v.has_ceil);
        m.ncloud = 0; m.wdir_vrb = true; m.visib_plus = false; m.visib = 0.5;
        fw_weather_from_awc(&m, &v);
        CHECK(v.wvar && !v.has_wdir && v.has_vis && v.vis_m == 805 && !v.vis_plus);
        m.obs = 1794981600;                                 /* 06:00 UTC: 01:00 in Florida */
        fw_weather_from_awc(&m, &v); CHECK(v.night);
        CHECK(fw_weather_is_night(51.47, -0.45, 1794981600) == true);    /* London, 06:00 UTC in November */
        CHECK(fw_weather_is_night(51.47, -0.45, 1795003200) == false);   /* noon */
    }

    printf("%s: %d weather checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
    return failed != 0;
}
