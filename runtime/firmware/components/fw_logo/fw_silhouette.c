/* Aircraft silhouettes for flights whose airline has no logo (LOGOS.md
 * "Silhouettes"): the vendor 4.2 lookup (FUN_42009624) with the owned
 * firmware's own drawings of the eight families, and, ahead of them, the
 * per-type pictures the vendor's filesystem update carries when the wall has
 * it. The drawings here are original: simple shapes on a 32x32 grid, seen from
 * above with the nose to the right (helicopters from the side). */
#include "fw_logo.h"
#include "aircraft_families.h"
#include <string.h>

/* ------------------------------------------------------------ lookup */

int fw_aircraft_family(const char *type){
    int lo = 0, hi = FW_AIRCRAFT_TYPES - 1;
    size_t n = type ? strlen(type) : 0, i;
    if (n < 1 || n > 6) return FW_FAMILY_NONE;
    for (i = 0; i < n; ++i)                          /* FUN_420083f8: 0-9 and A-Z only, nothing upper-cased */
        if (!((type[i] >= '0' && type[i] <= '9') || (type[i] >= 'A' && type[i] <= 'Z'))) return FW_FAMILY_NONE;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(fw_aircraft_types[mid].type, type);
        if (!c) return fw_aircraft_types[mid].family;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return FW_FAMILY_NONE;
}

static const char *const NAMES[] = {
    NULL, "airliner_twin", "light", "prop_twin", "prop_quad", "helicopter_tandem", "default", "smalljet", "heli",
};

const char *fw_family_name(int family){ return family > 0 && family <= FW_FAMILY_HELI ? NAMES[family] : NULL; }

int fw_family_parse(const char *name){
    int i;
    for (i = 1; i <= FW_FAMILY_HELI; ++i)
        if (name && !strcmp(name, NAMES[i])) return i;
    return FW_FAMILY_NONE;
}

static bool type_ok(const char *t){
    size_t n = t ? strlen(t) : 0, i;
    if (n < 2 || n > 6) return false;
    for (i = 0; i < n; ++i)
        if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'A' && t[i] <= 'Z'))) return false;
    return true;
}

const char *fw_logo_resolve_42(const char *al_code, const char *ac_type, const char *ac_class,
                               bool (*exists)(void *ctx, const char *path), void *ctx, char *buf, size_t buf_len){
    int family;
    if (!exists || !buf || buf_len < FW_LOGO_PATH_MAX) return NULL;
    if (al_code && strlen(al_code) == 3) {
        const char *p = fw_logo_resolve(al_code, NULL, exists, ctx, buf, buf_len);
        if (p && strcmp(p, FW_LOGO_DEFAULT_PATH)) return p;           /* the airline's own logo */
    }
    if (type_ok(ac_type)) {                                           /* owned: the type's own picture */
        memcpy(buf, FW_AIRCRAFT_DIR, sizeof FW_AIRCRAFT_DIR - 1);
        strcpy(buf + sizeof FW_AIRCRAFT_DIR - 1, ac_type);
        strcat(buf, ".png");
        if (exists(ctx, buf)) return buf;
    }
    family = fw_aircraft_family(ac_type);
    if (!family)
        family = ac_class && !strcmp(ac_class, "Heli") ? FW_FAMILY_HELI : ac_class && !strcmp(ac_class, "Light") ? FW_FAMILY_LIGHT
               : ac_class && !strcmp(ac_class, "Smalljet") ? FW_FAMILY_SMALLJET : FW_FAMILY_DEFAULT;
    buf[0] = '@';
    strcpy(buf + 1, NAMES[family]);
    return buf;
}

/* ------------------------------------------------------------ drawings */

typedef struct { signed char x, y; } Pt;
typedef struct { const Pt *pts; int n; } Poly;

#define POLY(...) {(const Pt[]){__VA_ARGS__}, (int)(sizeof((const Pt[]){__VA_ARGS__}) / sizeof(Pt))}

static uint32_t *grid;               /* one bit a pixel, bit x of row y: fw_silhouette's, on its stack */
#define SET(x, y) (grid[(y)] |= 1u << (x))
#define LIT(x, y) ((grid[(y)] >> (x)) & 1u)

static void line(int x0, int y0, int x1, int y1){
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < 32 && y0 >= 0 && y0 < 32) SET(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Filled: every pixel whose centre is inside, then the outline, so thin parts show. */
static void fill(const Poly *p){
    int x, y, i, j;
    for (y = 0; y < 32; ++y)
        for (x = 0; x < 32; ++x) {
            double px = x + 0.5, py = y + 0.5;
            bool in = false;
            for (i = 0, j = p->n - 1; i < p->n; j = i++) {
                double xi = p->pts[i].x, yi = p->pts[i].y, xj = p->pts[j].x, yj = p->pts[j].y;
                if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
            }
            if (in) SET(x, y);
        }
    for (i = 0, j = p->n - 1; i < p->n; j = i++) line(p->pts[j].x, p->pts[j].y, p->pts[i].x, p->pts[i].y);
}

static void rect(int x, int y, int w, int h){
    int i, k;
    for (k = y; k < y + h; ++k)
        for (i = x; i < x + w; ++i)
            if (i >= 0 && i < 32 && k >= 0 && k < 32) SET(i, k);
}

/* A jet seen from above: fuselage rows y0..y1, swept wings, tail. */
static void jet(int nose, int tail, int half, int wing_root, int wing_tip, int span_top, int stab){
    Poly body = POLY({(signed char)tail, (signed char)(15 - half)}, {(signed char)(nose - 4), (signed char)(15 - half)},
                     {(signed char)nose, 15}, {(signed char)nose, 16}, {(signed char)(nose - 4), (signed char)(16 + half)},
                     {(signed char)tail, (signed char)(16 + half)});
    Poly up = POLY({(signed char)wing_root, (signed char)(15 - half)}, {(signed char)(wing_root + 4), (signed char)(15 - half)},
                   {(signed char)(wing_tip + 2), (signed char)span_top}, {(signed char)wing_tip, (signed char)span_top});
    Poly down = POLY({(signed char)wing_root, (signed char)(16 + half)}, {(signed char)(wing_root + 4), (signed char)(16 + half)},
                     {(signed char)(wing_tip + 2), (signed char)(31 - span_top)}, {(signed char)wing_tip, (signed char)(31 - span_top)});
    Poly s1 = POLY({(signed char)(tail + 3), (signed char)(15 - half)}, {(signed char)(tail + 5), (signed char)(15 - half)},
                   {(signed char)(tail + 1), (signed char)(15 - stab)}, {(signed char)tail, (signed char)(15 - stab)});
    Poly s2 = POLY({(signed char)(tail + 3), (signed char)(16 + half)}, {(signed char)(tail + 5), (signed char)(16 + half)},
                   {(signed char)(tail + 1), (signed char)(16 + stab)}, {(signed char)tail, (signed char)(16 + stab)});
    fill(&body); fill(&up); fill(&down); fill(&s1); fill(&s2);
    rect(tail, 15, nose - tail, 2);                                   /* the fuselage's middle, to the nose */
}

/* A straight-winged aircraft seen from above. */
static void straight(int nose, int tail, int half, int wing_x, int chord, int span_top, int stab){
    rect(tail, 15 - half, nose - tail - 1, 2 + 2 * half);
    line(nose - 1, 15 - half + 1, nose, 15); line(nose - 1, 16 + half - 1, nose, 16);
    rect(wing_x, span_top, chord, 32 - 2 * span_top);
    rect(tail, 15 - stab, 3, 2 + 2 * stab);
}

static void draw_family(int family){
    memset(grid, 0, 32 * sizeof *grid);
    switch (family) {
    case FW_FAMILY_AIRLINER_TWIN:
        jet(30, 2, 1, 17, 10, 2, 6);
        rect(15, 8, 4, 2); rect(15, 22, 4, 2);                        /* engines */
        break;
    case FW_FAMILY_DEFAULT:                                           /* four engines, the big ones */
        jet(31, 1, 2, 16, 8, 1, 7);
        rect(16, 9, 4, 2); rect(16, 21, 4, 2); rect(12, 4, 4, 2); rect(12, 26, 4, 2);
        break;
    case FW_FAMILY_SMALLJET:
        jet(28, 5, 1, 17, 13, 5, 5);
        rect(8, 12, 4, 2); rect(8, 18, 4, 2);                         /* engines on the tail */
        break;
    case FW_FAMILY_PROP_TWIN:
        straight(28, 4, 1, 16, 3, 3, 5);
        rect(19, 8, 4, 2); rect(19, 22, 4, 2);                        /* nacelles */
        line(23, 6, 23, 11); line(23, 20, 23, 25);                    /* propellers */
        break;
    case FW_FAMILY_PROP_QUAD:
        straight(29, 3, 2, 15, 4, 1, 6);
        rect(19, 10, 3, 2); rect(19, 20, 3, 2); rect(19, 4, 3, 2); rect(19, 26, 3, 2);
        line(22, 9, 22, 12); line(22, 19, 22, 22); line(22, 3, 22, 6); line(22, 25, 22, 28);
        break;
    case FW_FAMILY_LIGHT:
        straight(25, 7, 1, 16, 3, 5, 4);
        line(26, 12, 26, 19);                                         /* propeller */
        break;
    case FW_FAMILY_HELI: {                                            /* from the side */
        Poly body = POLY({11, 14}, {20, 13}, {25, 16}, {25, 20}, {21, 23}, {12, 23}, {10, 19});
        fill(&body);
        rect(3, 17, 9, 2);                                            /* tail boom */
        line(2, 13, 4, 20);                                           /* tail rotor */
        rect(17, 10, 2, 3);                                           /* mast */
        line(4, 9, 30, 9);                                            /* rotor */
        line(12, 27, 25, 27); line(14, 24, 14, 27); line(22, 24, 22, 27);   /* skids */
        break;
    }
    case FW_FAMILY_HELICOPTER_TANDEM: {
        Poly body = POLY({4, 14}, {26, 14}, {29, 17}, {29, 22}, {26, 24}, {6, 24}, {4, 21});
        fill(&body);
        rect(6, 10, 2, 4); rect(25, 10, 2, 4);                        /* masts */
        line(0, 9, 14, 9); line(17, 9, 31, 9);                        /* rotors */
        line(8, 25, 8, 27); line(24, 25, 24, 27);                     /* wheels */
        break;
    }
    default:
        break;
    }
}

int fw_silhouette(int family, int side, FwLogo *out){
    int x, y;
    uint32_t canvas[32];
    if (!out || family <= FW_FAMILY_NONE || family > FW_FAMILY_HELI) return FW_LOGO_ERR_ARGS;
    grid = canvas;
    if (side < 1) side = 1;
    if (side > FW_LOGO_MAX_SIDE) side = FW_LOGO_MAX_SIDE;
    draw_family(family);
    memset(out, 0, sizeof *out);
    out->side = side;
    out->drawn_w = out->drawn_h = side;
    out->area_w = out->area_h = side;
    for (y = 0; y < side; ++y)
        for (x = 0; x < side; ++x) {
            int i = y * FW_LOGO_MAX_SIDE + x;          /* rows are FW_LOGO_MAX_SIDE apart, as fw_logo_draw reads them */
            out->px[i] = LIT(x * 32 / side, y * 32 / side) ? 0xFFFF : 0x0000;   /* white on black, as the vendor's */
            out->mask[i / 8] |= (uint8_t)(1u << (i % 8));                         /* the whole square, cleared */
        }
    return FW_LOGO_OK;
}
