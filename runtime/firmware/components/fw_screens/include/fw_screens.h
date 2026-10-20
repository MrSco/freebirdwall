#ifndef FLIGHTWALL_FW_SCREENS_H
#define FLIGHTWALL_FW_SCREENS_H
/* Screens for the 128x64 panel. Each clears the surface and draws one full
 * frame; none keeps state, so the same call renders identically on the board
 * and in the desktop preview. */
#include "fw_text.h"
#include "fw_logo.h"
#include "fw_flight.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Layout elements, as the stock firmware's built-in tables (vendor-layouts.json). */
typedef enum { FW_EL_TEXT, FW_EL_IMAGE } FwElementType;
typedef struct {
    const char *name;
    FwElementType type;
    int16_t x, y, w, h;
    bool right;          /* stock right-aligns only elements whose style is "tag" */
} FwElement;

/* mini-v1 "DEFAULT": logo, heading1..3, footer1..2. */
extern const FwElement fw_layout_mini_default[6];

/* The mini-v1 flight layout for a layout_template name; unknown names get
 * DEFAULT, as the stock selector (FUN_42006e34) does. */
const FwElement *fw_layout_mini(const char *name, int *count);

/* Draws one text element the stock way: white unless color given, opaque 6x8
 * cells, at the box x (or right-aligned for tags, width = code points x 6),
 * vertically centred, clipped only by the panel edge. */
void fw_screen_element_text(const FwSurface *s, const FwElement *e, const char *text, FwRgb color);

/* Owned (FLIGHT-SCREENS.md "Long text"): a text element wider than its box is
 * drawn clipped to the box and remembered; after the card is drawn,
 * fw_screen_scroll_tick(ms since then) moves those lines left (still, then
 * one pixel per FW_SCROLL_PX_MS, still at the end, again from the start),
 * redrawing only them. Every full screen here forgets them;
 * fw_screen_scroll_reset does too, for screens drawn elsewhere. */
#define FW_SCROLL_LINES 5
#define FW_SCROLL_HOLD_MS 1000
#define FW_SCROLL_PX_MS 40             /* default 25 px/s: a pixel per 40 ms panel pass */
#define FW_SCROLL_END_MS 1000
/* Pixel time, from the owner's scroll_speed (20, 40 or 80 ms). */
void fw_screen_scroll_speed(int px_ms);
int fw_scroll_offset(int text_w, int box_w, uint32_t ms);
/* How long the card on screen needs for its longest line to scroll through
 * once and rest at its end; 0 when nothing scrolls. The rotation keeps the
 * card at least that long. */
uint32_t fw_screen_scroll_pass_ms(void);
bool fw_screen_scroll_tick(const FwSurface *s, uint32_t ms);   /* true when a line moved */
int fw_screen_scroll_count(void);
void fw_screen_scroll_reset(void);

/* The ordinary flight card (FUN_420097a4): every element's stock template
 * expanded for this flight; the logo (may be NULL) in the image element. */
void fw_screen_flight(const FwSurface *s, const FwFlightSettings *st, const FwFlight *f, const FwLogo *logo);

/* The tracked progress bar (FUN_4200b92c): a grey line 2 px in from each
 * side at y = H - 2, filled green for prog percent (clamped 0..100); NAN
 * leaves only the grey line. */
void fw_screen_progress(const FwSurface *s, double prog);

/* The tracked-flight card (FUN_4200c290): tracked headings and footers for
 * the given page, "Flight landed" in green, and the progress bar. now = unix
 * seconds (for the cancelled/delayed window). */
void fw_screen_tracked(const FwSurface *s, const FwFlightSettings *st, const FwFlight *f, int page,
                       int64_t now, const FwLogo *logo);

/* The same card with some element texts replaced (Plus Sync's ACTIVE_MAIN):
 * override(ctx, element, text, cap) returning true supplies the text for that
 * element ("" = blank); progress is drawn as given, and with nan_black a NAN
 * draws a black line instead of the grey one. */
typedef bool (*FwTextOverride)(void *ctx, const char *element, char *text, size_t cap);
void fw_screen_tracked_ex(const FwSurface *s, const FwFlightSettings *st, const FwFlight *f, int page, int64_t now,
                          const FwLogo *logo, FwTextOverride override, void *ctx, double progress, bool nan_black);

/* The pass stock runs after a screen (FUN_4201a998): the optional border
 * (border_enable, grey at border_brightness_percent) and the 2x2 mark in the
 * top-right corner, orange while indicator_led reports more flights. */
typedef struct { bool border; uint8_t border_percent; bool indicator; } FwFrame;
void fw_screen_frame(const FwSurface *s, const FwFrame *frame);

/* "Too many flights, reduce area or add filters / Seeing N flights" in
 * orange (FUN_4200959c). */
void fw_screen_too_many(const FwSurface *s, const FwFrame *frame, int seeing);

/* A loading-template text such as "No flights..." (FUN_4201aaa4): cleared,
 * frame pass, white text centred at y = 28. */
void fw_screen_message(const FwSurface *s, const FwFrame *frame, const char *text);

/* Up to three white text lines at x = 3, y = 3 / 15 / 27 (FUN_4201aafc,
 * FUN_4201aa00, FUN_4201ab68 with a colour); NULL or "" lines are skipped. */
void fw_screen_lines(const FwSurface *s, const FwFrame *frame, const char *l1, const char *l2,
                     const char *l3, FwRgb color);

/* ------------------------------------------------------------ boot splash (SETUP.md) */

/* The splash's state (stock object at 0x3fca8458): whether the logo has been
 * drawn since the last reset, and the status text last drawn. */
typedef struct {
    bool logo_drawn;
    char key[96];
} FwSplash;

/* Logo box: x 0, y 8, the panel width, (int)(2 H * 0.9 / 3.0) rows (38 on 64). */
int fw_splash_logo_h(int height);

/* Owned: the FreeBirdWall mark, centred in a box from row top, box_h rows
 * tall: the Freebird bird (fw_brand.c, 4-bit coverage, white) and the name
 * ("FreeBird" white, "Wall" in the app's #00C8FF). */
void fw_screen_brand(const FwSurface *s, int top, int box_h);

/* The stock splash (FUN_4201654c -> FUN_42002788 / FUN_42002860, coloured:
 * FUN_4201bd70 -> FUN_4201bc04). First call after a reset: clears the panel
 * and draws the mark in the logo box (stock drew the vendor's
 * /images/system/logo.png there). Then, only when the text changed: a black
 * strip from the bottom of the logo box down, and the lines centred, the
 * first at y = logo_h + 11, every 10 px (empty lines skipped, still
 * advancing). White text is drawn transparent (FUN_4201a7a0); any other
 * colour opaque on black (FUN_4201a7bc). */
void fw_screen_splash(const FwSurface *s, FwSplash *st, const char *const *lines, int n, FwRgb color);

/* ------------------------------------------------------------ setup (SETUP.md) */

/* What the stock setup screens show, from the NVS "identity" namespace's id
 * and fp (FUN_4201f2dc, FUN_42023408, FUN_42023380). */
typedef struct {
    char ssid[32];        /* "Flightwall-" + id */
    char password[24];    /* the first 8 characters of fp */
    char qr[64];          /* "FW2;ID=<id>;FP=<fp>" for the vendor app */
    char join_qr[144];     /* owned addition: standard Wi-Fi join code for any phone camera */
} FwSetupInfo;

/* Returns false when stock would show "Identity error" (no id, or a password
 * shorter than 8 characters). */
bool fw_setup_info(const char *id, const char *fp, FwSetupInfo *out);

#define FW_SETUP_PAGES 3          /* the stock pages */
#define FW_SETUP_PAGE_JOIN_QR 3   /* owned addition, after the stock pages */
#define FW_SETUP_PAGE_MS 4000

/* Page 0: "Scan QR or type / credentials to / begin setup" (or, while
 * reconnecting, "Attempting to / reconnect..." in 0xFD20); page 1: the QR
 * code, version 3, low error correction, white modules, doubled when it fits
 * in 60 px, centred; page 2: "SSID" / ssid / "Password" / password
 * (FUN_42021ad8, FUN_4201ac4c, FUN_4201abf0). Page FW_SETUP_PAGE_JOIN_QR (owned
 * addition): the standard Wi-Fi join QR, drawn like the stock one. */
void fw_screen_setup(const FwSurface *s, const FwFrame *frame, int page, const FwSetupInfo *info,
                     bool reconnecting);

/* Owned: "Scan to play" with a QR of the games page URL (doubled, at 3,3,
 * drawn like the setup QR) and the game names (GAMES.md). */
void fw_screen_play(const FwSurface *s, const FwFrame *frame, const char *url);

/* Owned: the dashboard's address, as a QR and as text ("Scan for" /
 * "settings" / the address and port), for a new owner. */
void fw_screen_dashboard_qr(const FwSurface *s, const FwFrame *frame, const char *url);

/* Owned: the dashboard sign-in code. "Dashboard code" (cyan, y 6), the six
 * digits doubled and centred (y 22), "m:ss left" (grey, y 50). */
void fw_screen_pair_code(const FwSurface *s, const char *code, int seconds_left);

/* Sample flight card on the stock DEFAULT layout. With logo == NULL a dim
 * placeholder frame marks where the airline logo goes. */
void fw_screen_flight_card(const FwSurface *s, const FwLogo *logo);

/* Stock character handling: CP437 accents, the vendor's extra glyphs. */
void fw_screen_text_test(const FwSurface *s);

/* Wiring diagnostic: border and eight colour bars across both panel halves
 * and both chained panels (verified on the board, PANEL.md). */
void fw_screen_bars(const FwSurface *s);

#ifdef __cplusplus
}
#endif
#endif
