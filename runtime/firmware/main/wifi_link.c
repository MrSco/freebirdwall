/* Wi-Fi on the board; see wifi_link.h. Never logs network names or
 * passwords: reports carry counts, indexes and reasons only. */
#include "wifi_link.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "memory_probe.h"
#include "saved_networks.h"
#include "setup_server.h"

#define TICK_MS 100                       /* stock loop() period */
#define PICK_CACHE_MS 19999               /* DAT_420011a4 */
#define SCAN_MS_PER_CHANNEL 300           /* FUN_4201db4c */
#define API_HOST "api.theflightwall.com"  /* the stock network check (FUN_42016680) */
#define RESTART_MARK 0x4E4F4E54u          /* DAT_42000a48 */
#define RESTART_QUIET_MS 3599999u         /* DAT_42000a0c */
#define TEST_SSID "FW-TEST-NO-SUCH-NETWORK"

#define BIT_ONLINE BIT0
#define BIT_STARTED BIT1

static FwLink s_link;
static FwSavedNetworks s_nets;
static esp_netif_t *s_sta;
static EventGroupHandle_t s_events;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static FwLinkView s_view;
static volatile bool s_connected, s_auto;
static volatile uint8_t s_last_reason;
static unsigned s_attempts;
static int s_fail_left = CONFIG_FW_WIFI_FAIL_ATTEMPTS;
#if CONFIG_FW_WIFI_DROP_AFTER_S > 0
static bool s_dropped;
#endif
static uint32_t s_online_at;
static bool s_online_seen;
static volatile unsigned s_connections;

/* stock: the RTC word at 0x50000000 limits restarts to one an hour */
static RTC_NOINIT_ATTR uint32_t s_restart_mark;

/* chosen network cache (FUN_4201f120) */
static int s_pick = -1;
static uint32_t s_pick_at;

static uint32_t now_ms(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data){
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_events, BIT_STARTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        ++s_connections;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        s_last_reason = d->reason;
        /* Arduino's auto-reconnect: keep trying within an attempt and while
         * online; an aborted attempt stays down. /provision owns the station
         * while it test-joins. */
        if (s_auto && !fw_setup_station_busy()) esp_wifi_connect();
    }
}

/* FUN_4201f120 / FUN_4201db4c: with several saved networks, the strongest one
 * a scan finds (choice kept for 20 s); none found -> the first. */
static int pick(void){
    wifi_scan_config_t scan;
    static wifi_ap_record_t rec[20];
    uint16_t n = sizeof rec / sizeof rec[0];
    int best = -1, best_rssi = -1000;
    uint32_t now = now_ms();
    if (s_nets.count <= 1) return 0;
    if (s_pick >= 0 && now - s_pick_at <= PICK_CACHE_MS) return s_pick;
    memset(&scan, 0, sizeof scan);
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.max = SCAN_MS_PER_CHANNEL;
    if (esp_wifi_scan_start(&scan, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
        for (uint16_t i = 0; i < n; ++i)
            for (int j = 0; j < s_nets.count; ++j)
                if (rec[i].rssi > best_rssi && strcmp((const char *)rec[i].ssid, s_nets.net[j].ssid) == 0) {
                    best = j;
                    best_rssi = rec[i].rssi;
                }
    }
    memset(rec, 0, sizeof rec);
    s_pick_at = now;
    s_pick = best;
    return best < 0 ? 0 : best;
}

static void attempt(void){
    wifi_config_t c;
    int i = pick();
    char extra[64];
    bool test_fail = s_fail_left > 0;
    if (test_fail) {
        --s_fail_left;
        fw_station_config(&c, TEST_SSID, "not-a-password");
    } else {
        fw_station_config(&c, s_nets.net[i].ssid, s_nets.net[i].pass);
    }
    ++s_attempts;
    s_auto = true;
    esp_wifi_set_config(WIFI_IF_STA, &c);      /* RAM storage: never persisted */
    memset(&c, 0, sizeof c);
    esp_wifi_connect();
    snprintf(extra, sizeof extra, "\"n\":%u,\"network\":%d,\"saved\":%d,\"test_fail\":%s", s_attempts, i,
             s_nets.count, test_fail ? "true" : "false");
    fw_mem_report("wifi_attempt", extra);
}

static void abort_attempt(void){
    char extra[32];
    s_auto = false;
    esp_wifi_disconnect();
    snprintf(extra, sizeof extra, "\"reason\":%u", (unsigned)s_last_reason);
    fw_mem_report("wifi_abort", extra);
}

/* FUN_42016680: resolve the API host; fall back to public resolvers. */
static int dns_check(void){
    static const char *const fallback[] = {"8.8.8.8", "1.1.1.1"};
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    for (size_t i = 0; i <= sizeof fallback / sizeof fallback[0]; ++i) {
        if (i) {
            esp_netif_dns_info_t dns;
            memset(&dns, 0, sizeof dns);
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = ipaddr_addr(fallback[i - 1]);
            esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns);
        }
        if (getaddrinfo(API_HOST, NULL, &hints, &res) == 0 && res) {
            char extra[32];
            freeaddrinfo(res);
            snprintf(extra, sizeof extra, "\"ok\":true,\"fallback\":%u", (unsigned)i);
            fw_mem_report("wifi_dns_check", extra);
            return 1;
        }
    }
    fw_mem_report("wifi_dns_check", "\"ok\":false");
    return -1;
}

static void repair(void){
    /* stock: WiFi.config() back to DHCP */
    esp_netif_dhcpc_stop(s_sta);
    esp_netif_dhcpc_start(s_sta);
    fw_mem_report("wifi_repair", NULL);
}

static void restart(void){
    s_restart_mark = RESTART_MARK;
    fw_mem_report("wifi_restart", NULL);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void publish(void){
    const FwLinkView *v = fw_link_view(&s_link);
    bool changed;
    portENTER_CRITICAL(&s_mux);
    changed = v->serial != s_view.serial;
    s_view = *v;
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        char extra[96];
        snprintf(extra, sizeof extra, "\"mode\":%d,\"view\":%d,\"text\":\"%.24s\",\"reconnecting\":%s",
                 (int)s_link.mode, (int)v->kind, v->line[0] ? v->line[0] : "",
                 v->reconnecting ? "true" : "false");
        fw_mem_report("wifi_view", extra);
    }
}

/* Test: one drop, a while after first getting online. */
static void maybe_drop(uint32_t now){
#if CONFIG_FW_WIFI_DROP_AFTER_S > 0
    if (!s_dropped && s_online_seen && now - s_online_at >= CONFIG_FW_WIFI_DROP_AFTER_S * 1000u) {
        s_dropped = true;
        s_fail_left = CONFIG_FW_WIFI_DROP_FAILS;
        s_auto = false;
        esp_wifi_disconnect();
        fw_mem_report("wifi_test_drop", NULL);
    }
#else
    (void)now;
#endif
}

static void link_task(void *arg){
    (void)arg;
    for (;;) {
        FwLinkInput in;
        unsigned a;
        uint32_t joined = 0;
        memset(&in, 0, sizeof in);
        in.now = now_ms();
        in.connected = s_connected;
        in.ap_clients = fw_setup_clients(&joined);
        in.ap_joined = joined;
        in.provisioning = fw_setup_station_busy();
        in.restart_allowed = s_restart_mark != RESTART_MARK || in.now > RESTART_QUIET_MS;
        maybe_drop(in.now);
        a = fw_link_tick(&s_link, &in);
        if (a & FW_LINK_DO_DNS_CHECK) {
            /* answered on the next tick, as stock's check returns later */
            int r = dns_check();
            a &= ~(unsigned)FW_LINK_DO_DNS_CHECK;
            in.now = now_ms();
            in.dns = r;
            in.connected = s_connected;
            a |= fw_link_tick(&s_link, &in);
        }
        if (a & FW_LINK_DO_ABORT) abort_attempt();
        if (a & FW_LINK_DO_SETUP_STOP) fw_setup_stop();
        if (a & FW_LINK_DO_SETUP_START) fw_setup_start();
        if (a & FW_LINK_DO_ATTEMPT) attempt();
        if (a & FW_LINK_DO_REPAIR) repair();
        if (s_link.mode == FW_LINK_ONLINE) {
            if (!s_online_seen) { s_online_seen = true; s_online_at = in.now; }
            if (s_restart_mark == RESTART_MARK) s_restart_mark = 0;   /* FUN_42016650 */
            xEventGroupSetBits(s_events, BIT_ONLINE);
        } else {
            xEventGroupClearBits(s_events, BIT_ONLINE);
        }
        publish();
        if (a & FW_LINK_DO_RESTART) restart();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

bool fw_wifi_link_start(void){
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e;
    char extra[64];
    unsigned a;

    s_events = xEventGroupCreate();
    if (!s_events) return false;
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) return false;
    s_sta = esp_netif_create_default_wifi_sta();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    /* Keep every Wi-Fi setting in RAM: nothing here may rewrite stock's NVS. */
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;

    e = fw_saved_networks_load(&s_nets);
    snprintf(extra, sizeof extra, "\"err\":%d,\"saved\":%d,\"list\":%s,\"legacy\":%s", (int)e, s_nets.count,
             s_nets.has_nets ? "true" : "false", s_nets.has_legacy ? "true" : "false");
    fw_mem_report("wifi_saved_networks", extra);

    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START, on_event, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL, NULL);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) return false;
    xEventGroupWaitBits(s_events, BIT_STARTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
    fw_mem_report("wifi_started", NULL);

    a = fw_link_start(&s_link, now_ms(), s_nets.count);
    if (a & FW_LINK_DO_SETUP_START) fw_setup_start();
    if (a & FW_LINK_DO_ATTEMPT) attempt();
    publish();
    return xTaskCreate(link_task, "fw_link", 4096, NULL, 5, NULL) == pdPASS;
}

bool fw_wifi_link_wait_online(uint32_t ms){
    return (xEventGroupWaitBits(s_events, BIT_ONLINE, pdFALSE, pdTRUE, pdMS_TO_TICKS(ms)) & BIT_ONLINE) != 0;
}

void fw_wifi_link_view(FwLinkView *out){
    portENTER_CRITICAL(&s_mux);
    *out = s_view;
    portEXIT_CRITICAL(&s_mux);
}

unsigned fw_wifi_link_attempts(void){ return s_attempts; }
unsigned fw_wifi_link_connections(void){ return s_connections; }
