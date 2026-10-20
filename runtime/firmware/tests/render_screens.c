/* Renders every fw_screens screen to <out>/<name>.ppm through the same code
 * the board runs. Used by tools/preview_screens.py.
 *   render_screens <outdir> [logo.png [system-logo.png]]
 * (logo: flight card with a real logo; system logo: the boot splash) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_screens.h"
#include "fw_clock.h"
#include "fw_sync.h"
#include "fw_games.h"

#define W 128
#define H 64
static unsigned char rgb[H][W][3];
static FwLogo logo;

static void put(void *ctx, int x, int y, FwRgb c){
    (void)ctx;
    rgb[y][x][0] = c.r; rgb[y][x][1] = c.g; rgb[y][x][2] = c.b;
}

static int save(const char *dir, const char *name){
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
    return 0;
}

static FwLogo splash_logo;

/* box_w == 0: the flight-card square; else fitted to the whole box */
static int load_png(const char *path, FwLogo *out, int box_w, int box_h){
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    void *dec;
    long n;
    int rc;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    dec = malloc(fw_logo_decoder_bytes());
    if (!buf || !dec || fread(buf, 1, (size_t)n, f) != (size_t)n) return -1;
    fclose(f);
    rc = box_w ? fw_logo_decode_box(dec, buf, (size_t)n, box_w, box_h, out)
               : fw_logo_decode(dec, buf, (size_t)n,
                                fw_logo_side_for_box(fw_layout_mini_default[0].w, fw_layout_mini_default[0].h), out);
    free(buf); free(dec);
    return rc;
}

static int load_logo(const char *path){ return load_png(path, &logo, 0, 0); }

static FwSyncLeg sample_leg(const char *al, const char *fn, const char *orig, const char *dest,
                            const char *lt, const char *dep, const char *arr){
    FwSyncLeg l;
    fw_sync_leg_init(&l);
    snprintf(l.lt, sizeof l.lt, "%s", lt);
    snprintf(l.al, sizeof l.al, "%s", al);
    snprintf(l.fn, sizeof l.fn, "%s", fn);
    snprintf(l.orig, sizeof l.orig, "%s", orig);
    snprintf(l.dest, sizeof l.dest, "%s", dest);
    snprintf(l.dep_rel, sizeof l.dep_rel, "%s", dep);
    snprintf(l.arr_rel, sizeof l.arr_rel, "%s", arr);
    return l;
}

static int extras(const FwSurface *s, const char *dir, const FwLogo *splash){
    {   /* boot splash */
        FwSplash sp;
        const char *connecting[] = {"Connecting..."}, *failed[] = {"Connection failed"};
        memset(&sp, 0, sizeof sp);
        fw_screen_splash(s, &sp, connecting, 1, (FwRgb){255, 255, 255});
        if (save(dir, "splash_connecting")) return 1;
        fw_screen_splash(s, &sp, failed, 1, (FwRgb){255, 166, 0});
        if (save(dir, "splash_connection_failed")) return 1;
        fw_screen_lines(s, NULL, "Lost connection,", "retrying...", NULL, (FwRgb){255, 166, 0});
        if (save(dir, "wifi_lost")) return 1;
        fw_screen_lines(s, NULL, "WiFi disconnected,", "attempting to", "reconnect...", (FwRgb){255, 166, 0});
        if (save(dir, "wifi_disconnected")) return 1;
    }
    {   /* the dashboard's address for a new owner, and the sign-in code */
        fw_screen_dashboard_qr(s, NULL, "http://192.168.1.50:8080/");
        if (save(dir, "dashboard_address")) return 1;
        fw_screen_pair_code(s, "482915", 97);
        if (save(dir, "dashboard_code")) return 1;
    }
    {   /* Plus Sync, made-up legs */
        FwSyncLeg legs[4];
        legs[0] = sample_leg("AAL", "1234", "CLT", "PBI", "operating", "35m ago", "in 1h 10m");
        legs[0].dep_at = 1000; legs[0].arr_at = 2000;
        legs[1] = sample_leg("AAL", "2201", "PBI", "DFW", "operating", "in 2h 05m", "in 5h 40m");
        legs[2] = sample_leg("AAL", "918", "DFW", "ORD", "deadhead", "in 1d 3h", "in 1d 6h");
        legs[3] = sample_leg("AAL", "77", "ORD", "CLT", "operating", "in 2d 1h", "in 2d 3h");
        snprintf(legs[3].ls, sizeof legs[3].ls, "canceled");
        fw_screen_sync_message(s, NULL, &legs[0], NULL, "Home for dinner", 0, 1450);
        if (save(dir, "sync_active_message")) return 1;
        fw_screen_sync_message(s, NULL, &legs[0], NULL, "Running late, see you at dinner", 1, 1450);
        if (save(dir, "sync_active_message_long")) return 1;
        {   /* the owned Sync trip card (ACTIVE_MAIN), no live flight: page 0 and the message page */
            FwFlightSettings st;
            memset(&st, 0, sizeof st);
            fw_units_parse(&st.units, NULL, NULL, NULL, NULL, NULL);
            st.layout = "DEFAULT";
            st.mini = true;
            fw_screen_sync_active(s, &st, &legs[0], NULL, NULL, "Home for dinner", 0, 0, 1450, NULL);
            if (save(dir, "sync_active_main")) return 1;
            fw_screen_sync_active(s, &st, &legs[0], NULL, NULL, "Home for dinner", 4, 1, 1450, NULL);
            if (save(dir, "sync_active_main_message")) return 1;
        }
        fw_screen_sync_next(s, NULL, &legs[1], 3, 0);
        if (save(dir, "sync_upcoming_next")) return 1;
        fw_screen_sync_list(s, NULL, &legs[1], 3);
        if (save(dir, "sync_upcoming_list")) return 1;
    }
    fw_screen_play(s, NULL, "http://192.168.1.50:8080/");
    if (save(dir, "games_scan_to_play")) return 1;
    {   /* games, a few seconds into play */
        FwPong p;
        FwBreakout b;
        FwGameInput in;
        memset(&in, 0, sizeof in);
        in.p[0].target = in.p[1].target = FW_GAME_NO_TARGET;
        fw_pong_init(&p, 128, 64, 7);
        p.score[0] = 4; p.score[1] = 6;
        for (int i = 0; i < 180; ++i) fw_pong_tick(&p, &in);
        fw_pong_draw(&p, s);
        if (save(dir, "game_pong")) return 1;
        fw_breakout_init(&b, 128, 64, 7);
        in.p[0].present = true;
        for (int i = 0; i < 1500; ++i) {
            in.p[0].target = (int16_t)(b.ball_x / 256 + 1 + (i % 9) - 4);
            fw_breakout_tick(&b, &in);
        }
        fw_breakout_draw(&b, s);
        if (save(dir, "game_breakout")) return 1;
    }
    return 0;
}

int main(int argc, char **argv){
    const FwSurface s = {NULL, W, H, put, NULL};
    if (argc < 2) return 2;
    memset(rgb, 0, sizeof rgb); fw_screen_flight_card(&s, NULL); if (save(argv[1], "flight_card")) return 1;
    memset(rgb, 0, sizeof rgb); fw_screen_text_test(&s); if (save(argv[1], "text_test")) return 1;
    memset(rgb, 0, sizeof rgb); fw_screen_bars(&s); if (save(argv[1], "bars")) return 1;
    {
        /* The clock at 2026-10-19 14:25:07 UTC with the owner's stock settings
         * (US Eastern, LARGE, date shown day-first with weekday, 12-hour), then
         * the other stock variants. */
        const int64_t t = 1792419907LL;
        FwClockConfig c;
        memset(&c, 0, sizeof c);
        c.zone.standard_offset_min = -300;
        c.zone.dst_rule = FW_DST_NORTH_AMERICA;
        c.large = c.show_date = c.day_first = c.show_weekday = true;
        c.locale = "en";
        fw_screen_clock(&s, &c, t); if (save(argv[1], "clock")) return 1;
        c.show_seconds = true;
        fw_screen_clock(&s, &c, t); if (save(argv[1], "clock_seconds")) return 1;
        c.show_seconds = false;
        c.large = false;
        c.extra_count = 1;
        strcpy(c.extra[0].label, "London");
        c.extra[0].zone.standard_offset_min = 0;
        c.extra[0].zone.dst_rule = FW_DST_EUROPE;
        fw_screen_clock(&s, &c, t); if (save(argv[1], "clock_zones")) return 1;
        c.extra_count = 0;
        c.large = true;
        c.locale = "ru";
        fw_screen_clock(&s, &c, t); if (save(argv[1], "clock_ru")) return 1;
    }
    if (argc >= 3 && load_logo(argv[2]) != FW_LOGO_OK) return 3;
    {
        /* Tracked flight with the owner's stock settings (airline title, mini
         * DEFAULT; altitude in NM, speeds in mph, cardinal track, miles), every
         * page, then landed and on the ground. Sample values, not live data. */
        static const char *const names[4] = {"tracked_departed", "tracked_over", "tracked_metrics", "tracked_distance"};
        const FwLogo *lg = argc >= 3 ? &logo : NULL;
        FwFlightSettings st;
        FwFlight f;
        int page;
        memset(&st, 0, sizeof st);
        fw_units_parse(&st.units, "nautical_miles", "mph", "mph", "cardinal", "miles");
        st.title = fw_title_parse("airline");
        st.mini = true;
        st.layout = "DEFAULT";
        fw_flight_init(&f);
        strcpy(f.cs, "AAL1234"); strcpy(f.al_code, "AAL"); strcpy(f.al_name, "American");
        strcpy(f.orig_iata, "CLT"); strcpy(f.dest_iata, "PBI");
        strcpy(f.orig, "Charlotte Douglas"); strcpy(f.dest, "Palm Beach Intl"); strcpy(f.ac_name, "737-800");
        f.alt_b = 10668; f.vel = 231.5; f.trk = 172; f.vr = 0;
        f.has_trk = true; f.prog = 42; f.dist = 482803;
        strcpy(f.dep, "35m ago"); strcpy(f.arr, "1h 10m"); strcpy(f.over, "South Carolina");
        for (page = 0; page < 4; ++page) {
            fw_screen_tracked(&s, &st, &f, page, 0, lg); if (save(argv[1], names[page])) return 1;
        }
        f.prog = 100; strcpy(f.tft, "1h 52m");
        fw_screen_tracked(&s, &st, &f, 0, 0, lg); if (save(argv[1], "tracked_landed")) return 1;
        f.prog = 0; f.trk_gnd = true; f.vel = 8;
        fw_screen_tracked(&s, &st, &f, 0, 0, lg); if (save(argv[1], "tracked_on_ground")) return 1;
        f.has_trk = false; f.vel = 231.5; f.trk_gnd = false;
        st.layout = "TECHNICAL";
        fw_screen_flight(&s, &st, &f, lg); if (save(argv[1], "flight_technical")) return 1;
        {
            const FwFrame bordered = {true, 50, true};
            fw_screen_too_many(&s, NULL, 12); if (save(argv[1], "too_many")) return 1;
            fw_screen_message(&s, NULL, "No flights..."); if (save(argv[1], "no_flights")) return 1;
            st.layout = "DEFAULT";
            fw_screen_flight(&s, &st, &f, lg);
            fw_screen_frame(&s, &bordered); if (save(argv[1], "flight_border_indicator")) return 1;
        }
        {
            /* Setup screens for a made-up identity (never the board's). */
            static const char *const names[4] = {"setup_prompt", "setup_qr", "setup_credentials", "setup_reconnect"};
            FwSetupInfo info;
            int page;
            fw_setup_info("K7QM2P", "ABCDEFGHIJKLMNOPQRST", &info);
            for (page = 0; page < 4; ++page) {
                fw_screen_setup(&s, NULL, page % 3, &info, page == 3);
                if (save(argv[1], names[page])) return 1;
            }
        }
    }
    if (argc >= 3) {
        memset(rgb, 0, sizeof rgb); fw_screen_flight_card(&s, &logo);
        if (save(argv[1], "flight_card_logo")) return 1;
    }
    {
        bool have_splash = argc >= 4 && load_png(argv[3], &splash_logo, W, fw_splash_logo_h(H)) == 0;
        memset(rgb, 0, sizeof rgb);
        if (extras(&s, argv[1], have_splash ? &splash_logo : NULL)) return 1;
    }
    return 0;
}
