#ifndef FW_LINK_H
#define FW_LINK_H
/* Joining Wi-Fi, losing it and getting it back, as the stock firmware does
 * (SETUP.md "Wi-Fi failure and reconnect"; app loop FUN_4201cc88, reconnect
 * manager FUN_420165b0 / FUN_42016b50, Wi-Fi manager FUN_42021e94).
 *
 * Setup mode (the access point and its pages) starts only when Wi-Fi fails:
 * no saved network at boot, the boot attempt failing, or five minutes without
 * a connection. Unlike stock, the owned firmware stops the access point again
 * once it is back online and no phone is on it (stock leaves it up for app
 * pairing, which the owned firmware does not use).
 *
 * Portable C: the caller feeds the time and the radio's state every tick
 * (stock ticks every 100 ms), carries out the returned actions, and draws
 * what fw_link_view() describes. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stock timings, ms */
#define FW_LINK_ATTEMPT_MS 20000        /* DAT_42000f30: one connection attempt */
#define FW_LINK_FAILED_HOLD_MS 2000     /* "Connection failed" before setup mode */
#define FW_LINK_CONNECTED_HOLD_MS 1000  /* "Connected" */
#define FW_LINK_ONLINE_HOLD_MS 4000     /* DAT_42000fd8: "Reconnected", then the network test */
#define FW_LINK_LOSS_MS 2000            /* lost for this long -> reconnecting */
#define FW_LINK_BACKOFF_MS 5000         /* DAT_42000260 */
#define FW_LINK_BACKOFF_MAX_MS 30000    /* DAT_42000a18 */
#define FW_LINK_SETUP_RETRY_MS 60000    /* DAT_420009b4: retry spacing in setup mode */
#define FW_LINK_SECOND_MESSAGE_MS 59999 /* DAT_420008d8 */
#define FW_LINK_SETUP_AFTER_MS 299999   /* DAT_420008dc: lost this long -> setup mode */
#define FW_LINK_PHONE_PAUSE_MS 299999   /* DAT_420008dc: a phone this new pauses retries */
#define FW_LINK_REJOIN_MS 59999         /* DAT_420008d8: a phone back within this keeps its join time */
#define FW_LINK_DNS_AFTER_MS 2999       /* DAT_42000a38 */
#define FW_LINK_DNS_GAP_MS 2000
#define FW_LINK_REPAIR_AFTER_MS 14999   /* DAT_420007c0 */
#define FW_LINK_REPAIR_EVERY_MS 59999   /* DAT_420008d8 */
#define FW_LINK_RESTART_AFTER_MS 599999 /* DAT_42000a40 */
#define FW_LINK_AP_IDLE_MS 4999         /* DAT_420007e8: owned: access point off this long after the last phone */

/* RGB565 text colours, as stock passes them */
#define FW_LINK_WHITE 0xFFFF
#define FW_LINK_ORANGE 0xFD20
#define FW_LINK_GREEN 0x07E0

/* Actions for the caller, as bits */
enum {
    FW_LINK_DO_ATTEMPT = 1 << 0,      /* pick a saved network and connect; it gets FW_LINK_ATTEMPT_MS */
    FW_LINK_DO_ABORT = 1 << 1,        /* disconnect: the attempt is over */
    FW_LINK_DO_SETUP_START = 1 << 2,  /* access point, servers, setup pages */
    FW_LINK_DO_SETUP_STOP = 1 << 3,   /* owned: access point and servers off */
    FW_LINK_DO_DNS_CHECK = 1 << 4,    /* resolve the API host; report it in FwLinkInput.dns */
    FW_LINK_DO_REPAIR = 1 << 5,       /* reset the IP configuration */
    FW_LINK_DO_RESTART = 1 << 6,
};

typedef struct {
    uint32_t now;            /* ms, monotonic */
    bool connected;          /* associated and has an address */
    int ap_clients;          /* phones on the access point */
    uint32_t ap_joined;      /* when the current phone joined (see fw_link_phone_joined) */
    bool provisioning;       /* the setup page is test-joining a network: hands off the radio */
    int dns;                 /* answer to FW_LINK_DO_DNS_CHECK: 0 none yet, 1 resolved, -1 failed */
    bool restart_allowed;    /* stock: at most one restart an hour (RTC marker) */
} FwLinkInput;

typedef enum {
    FW_LINK_BOOT,            /* first attempt (app state 0) */
    FW_LINK_FIRST_SETUP,     /* nothing saved: setup mode until a network is provisioned (app 1) */
    FW_LINK_ONLINE,          /* app 2..6 */
    FW_LINK_RECONNECTING,    /* app 9 */
} FwLinkMode;

typedef enum {
    FW_LINK_VIEW_NORMAL,     /* the normal screens */
    FW_LINK_VIEW_SPLASH,     /* boot splash with a status line (FUN_4201654c) */
    FW_LINK_VIEW_LINES,      /* panel lines at x 3, y 3/15/27 (fw_screen_lines) */
    FW_LINK_VIEW_SETUP,      /* the setup pages; page 0 says "Attempting to / reconnect..." when reconnecting */
} FwLinkViewKind;

typedef struct {
    FwLinkViewKind kind;
    const char *line[3];     /* NULL when unused */
    uint16_t color;          /* RGB565 */
    bool reconnecting;
    bool clear;              /* clear the panel first (stock, before "Reconnected") */
    uint32_t serial;         /* changes whenever the view changes */
} FwLinkView;

typedef struct {
    FwLinkMode mode;
    FwLinkView view;
    uint32_t hold_until;     /* the current splash/message stays until then */
    bool holding;
    /* attempt */
    bool attempting;
    uint32_t attempt_start;
    bool attempt_failed;
    /* reconnect manager */
    bool setup;              /* setup mode on (+0x2c) */
    bool ap_on;
    bool reconnect_flag;     /* +0xa8: page 0 says "Attempting to reconnect" */
    bool retry_now;          /* +0x2d */
    bool paused;
    int attempts;            /* [3] */
    uint32_t last_start;     /* [4] */
    uint32_t lost_since;     /* [5] */
    uint32_t entered;        /* [8]: when reconnecting began */
    bool message1, message2; /* +0x2e/+0x2f */
    bool conn;               /* connected since connected_since (while reconnecting) */
    uint32_t connected_since;
    bool dns_pending, dns_done;
    uint32_t last_dns;
    bool repaired;
    uint32_t last_repair;
    bool pending_setup;      /* "Connection failed" is up; setup mode follows */
    /* online */
    bool losing;
    uint32_t loss_start;
    bool ap_idle;
    uint32_t ap_idle_since;
} FwLink;

/* Boot (FUN_42016e70): with saved networks, "Connecting..." and the first
 * attempt; without, "Starting..." and setup mode. Returns actions. */
unsigned fw_link_start(FwLink *l, uint32_t now, int saved_networks);

/* One tick. Returns actions. */
unsigned fw_link_tick(FwLink *l, const FwLinkInput *in);

const FwLinkView *fw_link_view(const FwLink *l);

/* The retry spacing outside setup mode after `attempts` attempts: 5 s for the
 * first five, then doubling, at most 30 s (FUN_42016b50). */
uint32_t fw_link_backoff(int attempts, bool setup);

/* Join time to report for a phone that just joined: a phone back within
 * FW_LINK_REJOIN_MS of leaving keeps its earlier join time. */
uint32_t fw_link_phone_joined(uint32_t now, uint32_t previous_join, uint32_t previous_leave, bool had_previous);

#ifdef __cplusplus
}
#endif
#endif
