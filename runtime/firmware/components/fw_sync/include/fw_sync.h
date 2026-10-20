#ifndef FW_SYNC_H
#define FW_SYNC_H
/* Stock "Plus Sync" as the device implements it (SYNC.md): crew legs (the
 * active one and up to five upcoming), an optional message, and the screens
 * PLUS_SYNC_ACTIVE_MESSAGE, PLUS_SYNC_UPCOMING_NEXT and PLUS_SYNC_UPCOMING_LIST
 * on the mini-v1 panel, plus the text rules of PLUS_SYNC_ACTIVE_MAIN.
 *
 * Stock gets legs from its server, with the relative times ("in 2h 05m",
 * "35m ago") already written as text; the device only strips a leading "in ",
 * looks for an "ago" ending and for a digit followed by 'd'. The owned
 * firmware fills the same record from its own schedule.
 *
 * Portable C, drawn through FwSurface. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw_text.h"
#include "fw_screens.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FW_SYNC_UPCOMING_MAX 5
#define FW_SYNC_MESSAGE_MAX 256      /* 255 bytes + NUL (FUN_4200b1b0) */

/* One leg, 0x60 bytes in stock (FUN_4200a01c); keys lt ls al fn orig dest
 * dep_rel arr_rel dep_at arr_at. */
typedef struct {
    char lt[12];          /* leg type: "operating" / "deadhead" */
    char ls[12];          /* leg status: "canceled" */
    char al[8];           /* airline code (also the logo) */
    char fn[8];           /* flight number */
    char orig[8], dest[8];
    char dep_rel[16], arr_rel[16];   /* relative times as text */
    uint32_t dep_at, arr_at;         /* unix seconds, 0 = unknown */
} FwSyncLeg;

/* Live tracking of the active leg's flight, when stock found it in the feed
 * (the tracked-flight texts, already formatted); NULL when there is none. */
typedef struct {
    const char *trk_dep, *trk_arr, *trk_over;
    const char *dist;     /* "{trk_dist}" text, or NULL when the distance is not valid */
    double prog;          /* NAN when not valid */
} FwSyncLive;

void fw_sync_leg_init(FwSyncLeg *leg);

/* Derived strings (FUN_4200a0e8, FUN_4200a17c, FUN_4200a0b0, FUN_4200a274). */
void fw_sync_flight(const FwSyncLeg *leg, char *out, size_t cap);   /* al+fn when both set, else "" */
void fw_sync_route(const FwSyncLeg *leg, char *out, size_t cap);    /* orig-dest, "---" for a missing one */
const char *fw_sync_status(const FwSyncLeg *leg);                   /* Canceled / Operating / Deadhead / "" */
bool fw_sync_canceled(const FwSyncLeg *leg);
const char *fw_sync_strip_in(const char *rel);                      /* without a leading "in " */

/* ISO-8601 as stock reads dep_at/arr_at (FUN_42009db4): YYYY-MM-DD, then T, t
 * or space, HH:MM[:SS], an ignored .frac or ,frac, then Z, z, +-HH[[:]MM] or
 * nothing (UTC). Second 60 reads as 59. Returns unix seconds, 0 on failure. */
uint32_t fw_sync_parse_time(const char *iso);

/* fit(s, n) (FUN_4200a8b0): longer than n code points -> the first n-1,
 * trimmed, then "."; n <= 0 -> "". */
void fw_sync_fit(const char *s, int n, char *out, size_t cap);

/* Word wrap into two lines (FUN_4200aab8): words split on spaces; a word
 * moves to the second line when the first is not empty and it would make the
 * first longer than max; the second line takes the rest. */
void fw_sync_wrap(const char *text, int max, char *l1, char *l2, size_t cap);

/* The ident line (heading1 / ACTIVE_MESSAGE ident): the flight, or on odd
 * phases the status, when both exist; otherwise whichever exists. */
void fw_sync_ident(const FwSyncLeg *leg, int phase, char *out, size_t cap);

/* ACTIVE_MAIN page 0 footers (FUN_4200a298). live may be NULL. */
void fw_sync_footers(const FwSyncLeg *leg, const FwSyncLive *live, char *f1, char *f2, size_t cap);

/* ACTIVE_MAIN page 4: the message wrapped into the two footers at max code
 * points each, each line then fitted. */
void fw_sync_message_footers(const char *msg, int max, char *f1, char *f2, size_t cap);

/* ACTIVE_MESSAGE context lines (FUN_4200a648); returns how many (1..4). */
#define FW_SYNC_CONTEXT_MAX 4
int fw_sync_context(const FwSyncLeg *leg, const FwSyncLive *live, char lines[FW_SYNC_CONTEXT_MAX][40]);

/* Progress: live when valid, else from the times; NAN when canceled, a time
 * is missing or dep_at >= arr_at. now = unix seconds. */
double fw_sync_progress(const FwSyncLeg *leg, const FwSyncLive *live, int64_t now);

/* How long a display phase lasts, as a multiple of the slot
 * (flight_display_speed: slow 5000, fast 2000, else 3000 ms). */
typedef enum {
    FW_SYNC_PHASE_FLIGHTS,         /* main phase, no active leg: per flight */
    FW_SYNC_PHASE_MESSAGE_VIEW,    /* main_view "message": per context line */
    FW_SYNC_PHASE_ACTIVE,          /* active leg, no live flight: 1, 2 with a message */
    FW_SYNC_PHASE_ACTIVE_LIVE,     /* active leg tracked live: per tracked page */
    FW_SYNC_PHASE_UPCOMING,        /* per upcoming leg */
} FwSyncPhaseKind;
uint32_t fw_sync_phase_ms(FwSyncPhaseKind kind, int count, bool has_message, uint32_t slot_ms);

#define FW_SYNC_SUB_ROTATION_MS 4999   /* active-screen index advances after more than this */

/* ------------------------------------------------------------ screens (mini-v1) */

/* PLUS_SYNC_ACTIVE_MESSAGE (FUN_4200ae6c): the message large (scale 2 when
 * both lines fit, else scale 1), the ident and one context line, chosen by
 * phase, and the progress bar (nothing when unknown). */
void fw_screen_sync_message(const FwSurface *s, const FwFrame *frame, const FwSyncLeg *leg,
                            const FwSyncLive *live, const char *msg, int phase, int64_t now);

/* PLUS_SYNC_ACTIVE_MAIN: the tracked card on the live flight when there is
 * one (live_flight, with live), else on a record made from the leg (flight as
 * the callsign, al as the logo code, orig/dest or "---"); heading1 is the
 * ident for `phase`; page 0 has the leg footers, page 4 the message; the
 * progress bar from fw_sync_progress, a black line when unknown. */
void fw_screen_sync_active(const FwSurface *s, const FwFlightSettings *st, const FwSyncLeg *leg,
                           const FwFlight *live_flight, const FwSyncLive *live, const char *msg, int page,
                           int phase, int64_t now, const FwLogo *logo);

/* PLUS_SYNC_UPCOMING_NEXT (FUN_4200c604): leg `index` of `count`. */
void fw_screen_sync_next(const FwSurface *s, const FwFrame *frame, const FwSyncLeg *legs, int count, int index);

/* PLUS_SYNC_UPCOMING_LIST (FUN_4200b2d8): up to five rows. */
void fw_screen_sync_list(const FwSurface *s, const FwFrame *frame, const FwSyncLeg *legs, int count);

#ifdef __cplusplus
}
#endif
#endif
