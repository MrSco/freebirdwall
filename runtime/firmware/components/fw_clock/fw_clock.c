#include "fw_clock.h"
#include "clock_tables.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ time */

/* Days since 1970-01-01 -> civil date (proleptic Gregorian), any range. */
static void civil_from_days(int64_t z, int *y, int *m, int *d){
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static bool is_leap(int y){ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* gmtime() for the fields the clock uses. */
static void split_time(int64_t t, FwTime *out){
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t rem = t - days * 86400;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    out->sec = (int)(rem % 60);
    out->min = (int)(rem / 60 % 60);
    out->hour = (int)(rem / 3600);
    out->mday = d;
    out->mon = m - 1;
    out->year = y - 1900;
    out->wday = (int)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
    out->yday = cum[m - 1] + d - 1 + (m > 2 && is_leap(y));
    out->dst = false;
}

/* FUN_42025124: Sakamoto's weekday without the January/February year
 * adjustment; stock only asks about March, October and November, where the
 * formula is exact. 0 = Sunday. */
static int dst_weekday(int y, int m, int d){
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* FUN_42025178: seconds since 1970 for y-m-d at hour h, counted as UTC. Stock
 * adds the leap day for any month; it is only used from March on. */
static int64_t dst_epoch(int y, int m, int d, int h){
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int64_t days = 0;
    int yy;
    for (yy = 1970; yy < y; ++yy) days += is_leap(yy) ? 366 : 365;
    days += cum[m - 1] + (is_leap(y) ? 1 : 0);
    return (d - 1 + days) * 86400 + (int64_t)h * 3600;
}

/* FUN_42025328. */
bool fw_local_time(int64_t utc, FwZone zone, FwTime *out){
    int offset = zone.standard_offset_min == FW_ZONE_UNSET ? 0 : zone.standard_offset_min;
    bool dst = false;
    if (!out) return false;
    if (zone.standard_offset_min != FW_ZONE_UNSET && zone.dst_rule != FW_DST_NONE) {
        FwTime std_local;
        int y;
        split_time(utc + (int64_t)offset * 60, &std_local);
        y = std_local.year + 1900;
        if (zone.dst_rule == FW_DST_NORTH_AMERICA) {
            /* 2nd Sunday of March to 1st Sunday of November, 02:00 local. */
            int start_day = (7 - dst_weekday(y, 3, 1)) % 7 + 8;
            int end_day = (7 - dst_weekday(y, 11, 1)) % 7 + 1;
            int64_t start = dst_epoch(y, 3, start_day, 2) - (int64_t)offset * 60;
            int64_t end = dst_epoch(y, 11, end_day, 2) - (int64_t)offset * 60 - 3600;
            dst = utc >= start && utc < end;
        } else if (zone.dst_rule == FW_DST_EUROPE) {
            /* Last Sunday of March to last Sunday of October, 01:00 UTC. */
            int64_t start = dst_epoch(y, 3, 31 - (dst_weekday(y, 3, 31) + 7) % 7, 1);
            int64_t end = dst_epoch(y, 10, 31 - (dst_weekday(y, 10, 31) + 7) % 7, 1);
            dst = utc >= start && utc < end;
        }
        if (dst) offset += 60;
    }
    split_time(utc + (int64_t)offset * 60, out);
    out->dst = dst;
    return true;
}

/* Owned: the names while daylight saving is on, by standard offset and rule
 * (stock names the standard offset all year, "EST" in July). */
static const struct { int16_t minutes; uint8_t rule; const char *name; } DAYLIGHT_NAMES[] = {
    {-600, FW_DST_NORTH_AMERICA, "HDT"}, {-540, FW_DST_NORTH_AMERICA, "AKDT"}, {-480, FW_DST_NORTH_AMERICA, "PDT"},
    {-420, FW_DST_NORTH_AMERICA, "MDT"}, {-360, FW_DST_NORTH_AMERICA, "CDT"}, {-300, FW_DST_NORTH_AMERICA, "EDT"},
    {-240, FW_DST_NORTH_AMERICA, "ADT"},
    {0, FW_DST_EUROPE, "BST"}, {60, FW_DST_EUROPE, "CEST"}, {120, FW_DST_EUROPE, "EEST"},
};

/* FUN_42019f90 / FUN_42019f20, and the daylight name when dst (fw_local_time):
 * a zone in daylight saving with no name here shows its offset as it is now. */
void fw_zone_name(FwZone zone, bool dst, char *buf, int cap){
    int i, mag, now;
    if (!buf || cap <= 0) return;
    if (zone.standard_offset_min == FW_ZONE_UNSET) { snprintf(buf, (size_t)cap, "UTC"); return; }
    for (i = 0; dst && i < (int)(sizeof DAYLIGHT_NAMES / sizeof DAYLIGHT_NAMES[0]); ++i)
        if (DAYLIGHT_NAMES[i].minutes == zone.standard_offset_min && DAYLIGHT_NAMES[i].rule == zone.dst_rule) {
            snprintf(buf, (size_t)cap, "%s", DAYLIGHT_NAMES[i].name);
            return;
        }
    for (i = 0; !dst && i < FW_CLOCK_ZONE_NAMES; ++i)
        if (fw_clock_zone_names[i].minutes == zone.standard_offset_min) {
            snprintf(buf, (size_t)cap, "%s", fw_clock_zone_names[i].name);
            return;
        }
    now = zone.standard_offset_min + (dst ? 60 : 0);
    mag = now < 0 ? -now : now;
    if (mag % 60)
        snprintf(buf, (size_t)(cap < 12 ? cap : 12), "UTC%c%d:%02d", now >= 0 ? '+' : '-', mag / 60, mag % 60);
    else
        snprintf(buf, (size_t)(cap < 12 ? cap : 12), "UTC%c%d", now >= 0 ? '+' : '-', mag / 60);
}

/* ------------------------------------------------------------ text */

/* FUN_420075b4. */
void fw_clock_time_text(const FwClockConfig *c, const FwTime *t, char *buf, int cap){
    size_t n = (size_t)(cap < 16 ? cap : 16);   /* stock formats into 16 bytes */
    if (!buf || cap <= 0) return;
    if (c->is_24h) {
        if (c->show_seconds) snprintf(buf, n, "%02d:%02d:%02d", t->hour, t->min, t->sec);
        else snprintf(buf, n, "%02d:%02d", t->hour, t->min);
    } else {
        int h = t->hour % 12;
        const char *ampm = t->hour <= 11 ? "AM" : "PM";
        if (h == 0) h = 12;
        if (c->show_seconds) snprintf(buf, n, "%d:%02d:%02d %s", h, t->min, t->sec, ampm);
        else snprintf(buf, n, "%d:%02d %s", h, t->min, ampm);
    }
}

static const FwClockLocale *find_locale(const char *code){
    int i;
    if (code)
        for (i = 0; i < FW_CLOCK_LOCALES; ++i)
            if (strcmp(code, fw_clock_locales[i].code) == 0) return &fw_clock_locales[i];
    return &fw_clock_locales[0];
}

/* FUN_42007664. */
void fw_clock_date_text(const FwClockConfig *c, const FwTime *t, char *buf, int cap){
    const FwClockLocale *loc = find_locale(c->locale);
    int mon = t->mon >= 0 && t->mon <= 11 ? t->mon : 0;
    int wday = t->wday >= 0 && t->wday <= 6 ? t->wday : 0;
    int mday = t->mday >= 1 && t->mday <= 31 ? t->mday : 1;
    char day_month[24], out[32];
    int n;
    if (!buf || cap <= 0) return;
    if (c->day_first) snprintf(day_month, sizeof day_month, "%d %s", mday, loc->months[mon]);
    else snprintf(day_month, sizeof day_month, "%s %d", loc->months[mon], mday);
    n = snprintf(out, sizeof out, "%s%s%s", c->show_weekday ? loc->weekdays[wday] : "",
                 c->show_weekday ? " " : "", day_month);
    if (n < 0 || n > 31) out[0] = 0;   /* stock drops a date that does not fit */
    snprintf(buf, (size_t)cap, "%s", out);
}

/* Extra-zone label: trimmed, ASCII upper-cased; empty means the zone's name. */
static void zone_label(const FwExtraZone *z, bool dst, char *buf, int cap){
    const char *b = z->label, *e;
    char *o;
    size_t len = 0;
    while (len < sizeof z->label && z->label[len]) ++len;
    while (b < z->label + len && (*b == ' ' || (*b >= '\t' && *b <= '\r'))) ++b;
    e = z->label + len;
    while (e > b && (e[-1] == ' ' || (e[-1] >= '\t' && e[-1] <= '\r'))) --e;
    if (e == b) { fw_zone_name(z->zone, dst, buf, cap); return; }
    snprintf(buf, (size_t)cap, "%.*s", (int)(e - b), b);
    for (o = buf; *o; ++o)
        if (*o >= 'a' && *o <= 'z') *o = (char)(*o - 'a' + 'A');
}

/* ------------------------------------------------------------ layout */

static void add_line(FwClockFrame *f, FwClockLineKind kind, const char *text){
    FwClockLine *l = &f->lines[f->count++];
    memset(l, 0, sizeof *l);
    l->kind = (uint8_t)kind;
    snprintf(l->text, sizeof l->text, "%s", text);
}

/* C division as the Xtensa code does it, guarded the same way: a block wider
 * than the panel by more than one pixel starts at 0. */
static int centre_start(int span, int size){
    return span - size >= -1 ? (span - size) / 2 : 0;
}

void fw_clock_frame(const FwClockConfig *c, int64_t utc, int width, int height, FwClockFrame *f){
    FwTime t;
    char time_text[16], name[16], line[40];
    int max_lines = c->large ? 2 : 3, extras, i, longest = 0, scale, line_h, block_w = 0, gaps, x0, y;
    int count = c->extra_count < 0 ? 0 : c->extra_count > FW_CLOCK_MAX_EXTRA_ZONES ? FW_CLOCK_MAX_EXTRA_ZONES
                                                                                     : c->extra_count;
    memset(f, 0, sizeof *f);

    /* FUN_42013874: which lines. */
    if (c->show_date) extras = max_lines == 3 ? (count < 1 ? count : 1) : 0;
    else extras = count < max_lines - 1 ? count : max_lines - 1;
    if (c->show_date && fw_local_time(utc, c->zone, &t)) {
        fw_clock_date_text(c, &t, line, sizeof line);
        add_line(f, FW_CLOCK_DATE, line);
    }
    if (fw_local_time(utc, c->zone, &t)) {
        fw_clock_time_text(c, &t, time_text, sizeof time_text);
        if (extras == 0) add_line(f, FW_CLOCK_TIME, time_text);
        else {
            fw_zone_name(c->zone, t.dst, name, sizeof name);
            snprintf(line, sizeof line, "%s %s", name, time_text);
            add_line(f, FW_CLOCK_TIME, line);
        }
        for (i = 0; i < extras; ++i) {
            if (!fw_local_time(utc, c->extra[i].zone, &t)) continue;
            fw_clock_time_text(c, &t, time_text, sizeof time_text);
            zone_label(&c->extra[i], t.dst, name, sizeof name);
            snprintf(line, sizeof line, "%s %s", name, time_text);
            add_line(f, FW_CLOCK_ZONE, line);
        }
    }

    /* FUN_42013b88: size and place them. */
    for (i = 0; i < f->count; ++i) {
        int n = fw_text_glyph_count(f->lines[i].text);
        if (n > longest) longest = n;
    }
    gaps = f->count > 1 ? f->count - 1 : 0;
    scale = 1;
    if (c->large)
        scale = longest == 0 || (longest * 12 <= width && f->count * 16 + gaps <= height) ? 2 : 1;
    line_h = scale * 8;
    for (i = 0; i < f->count; ++i) {
        f->lines[i].w = scale * fw_text_glyph_count(f->lines[i].text) * 6;
        if (f->lines[i].w > block_w) block_w = f->lines[i].w;
    }
    x0 = centre_start(width, block_w);
    y = centre_start(height, line_h * f->count + gaps);
    for (i = 0; i < f->count; ++i) {
        f->lines[i].x = (block_w - f->lines[i].w) / 2 + x0;
        f->lines[i].y = y;
        f->lines[i].h = line_h;
        y += line_h + 1;
    }
    f->scale = scale;
}

/* ------------------------------------------------------------ drawing */

static const FwRgb WHITE = {255, 255, 255};
static const FwRgb BLACK = {0, 0, 0};

static const FwRowGlyph *date_glyph(int32_t cp){
    int lo = 0, hi = FW_CLOCK_DATE_GLYPHS - 1;
    if (cp < 0) return NULL;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fw_clock_date_glyphs[mid].cp == (uint32_t)cp) return &fw_clock_date_glyphs[mid];
        if (fw_clock_date_glyphs[mid].cp < (uint32_t)cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* FUN_4200f7ec: the date line draws Greek/Cyrillic/Turkish capitals from its
 * own table and everything else as normal text, one code point at a time. */
static void draw_date(const FwSurface *s, int x, int y, const char *text, int scale){
    const char *p = text;
    for (;;) {
        const char *start = p;
        int32_t cp = fw_text_next_codepoint(&p, NULL);
        const FwRowGlyph *g;
        char one[5];
        if (cp == FW_TEXT_END) return;
        if ((g = date_glyph(cp)) != NULL) fw_text_draw_rows(s, x, y, g->rows, WHITE, &BLACK, scale);
        else {
            size_t n = (size_t)(p - start);
            if (cp == FW_TEXT_MALFORMED || n > 4) { one[0] = '?'; n = 1; }
            else memcpy(one, start, n);
            one[n] = 0;
            fw_text_draw_bg(s, x, y, one, WHITE, BLACK, scale);
        }
        x += FW_FONT_ADVANCE * scale;
    }
}

static bool same_place(const FwClockLine *a, const FwClockLine *b){
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

void fw_clock_draw(const FwSurface *s, const FwClockFrame *f, FwClockFrame *prev){
    bool cleared = !prev || prev->count == 0 || prev->scale != f->scale || prev->count != f->count;
    int i;
    if (!cleared)
        for (i = 0; i < f->count; ++i)
            if (!same_place(&f->lines[i], &prev->lines[i])) cleared = true;
    if (cleared) fw_clear(s);
    for (i = 0; i < f->count; ++i) {
        const FwClockLine *l = &f->lines[i];
        int scale = l->h >> 3 < 1 ? 1 : l->h >> 3;
        if (!cleared && strcmp(l->text, prev->lines[i].text) == 0) continue;
        if (l->kind == FW_CLOCK_DATE) draw_date(s, l->x, l->y, l->text, scale);
        else fw_text_draw_bg(s, l->x, l->y, l->text, WHITE, BLACK, scale);
    }
    if (prev) *prev = *f;
}

void fw_screen_clock(const FwSurface *s, const FwClockConfig *c, int64_t utc){
    FwClockFrame f;
    fw_clock_frame(c, utc, s->width, s->height, &f);
    fw_clock_draw(s, &f, NULL);
}
