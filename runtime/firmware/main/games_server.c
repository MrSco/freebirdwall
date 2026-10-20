/* The owned dashboard, the games controller and its WebSocket; see
 * games_server.h. Anyone on the home network can use them: they carry game
 * input, the games switch and the "show QR" request only, and reach no other
 * settings, credentials or storage. */
#include "games_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "fw_arcade.h"
#include "memory_probe.h"
#include "owned_settings.h"
#include "dashboard_api.h"
#include "cJSON.h"

#define MAX_CLIENTS 4                     /* two players and two watchers */
#define STATUS_EVERY_MS 1000              /* heartbeat even when nothing changed */
#define QR_SHOW_MS 30000                  /* "Show QR on the wall" */
#define BODY_MAX 128

extern const char games_page_start[] asm("_binary_games_page_html_start");
extern const char games_page_end[] asm("_binary_games_page_html_end");
extern const char dashboard_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_end[] asm("_binary_dashboard_html_end");
extern const char icon_start[] asm("_binary_icon_png_start");
extern const char icon_end[] asm("_binary_icon_png_end");

/* "Add to Home Screen": opens the dashboard full screen with our icon
 * (tools/make_icon.py). Served from flash; costs no RAM. iOS keeps home-screen
 * icons by address, so bump ?v= here and in both pages whenever icon.png
 * changes (v=2: the Freebird bird replaced the plane). */
static const char manifest[] =
    "{\"name\":\"FreeBirdWall\",\"short_name\":\"FreeBirdWall\",\"start_url\":\"/\",\"display\":\"standalone\","
    "\"background_color\":\"#0b0d12\",\"theme_color\":\"#0b0d12\","
    "\"icons\":[{\"src\":\"/icon.png?v=2\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any maskable\"}]}";

static httpd_handle_t s_server;
static SemaphoreHandle_t s_lock;
static FwArcade s_arcade;
static struct { int fd; int slot; } s_clients[MAX_CLIENTS];
static uint32_t s_sent_serial;
static int64_t s_sent_at;
static volatile uint32_t s_qr_until, s_qr_serial;   /* ms; the serial changes on each request */
static volatile FwQrKind s_qr_kind;

static uint32_t now_ms(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

bool fw_games_running(void){ return s_server != NULL; }

bool fw_games_url(char *buf, size_t cap){
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_server || !sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.ip.addr) return false;
    snprintf(buf, cap, "http://" IPSTR ":%d/play", IP2STR(&ip.ip), FW_GAMES_PORT);
    return true;
}

bool fw_dashboard_url(char *buf, size_t cap){
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_server || !sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.ip.addr) return false;
    snprintf(buf, cap, "http://" IPSTR ":%d/", IP2STR(&ip.ip), FW_GAMES_PORT);
    return true;
}

FwQrKind fw_games_qr_kind(void){ return s_qr_kind; }

void fw_dashboard_hint(uint32_t ms){
    s_qr_kind = FW_QR_DASHBOARD;
    s_qr_until = now_ms() + ms;
    ++s_qr_serial;
    if (!s_qr_serial) ++s_qr_serial;
}

bool fw_games_in_rotation(void){
    FwOwnedSettings st;
    fw_owned_settings_get(&st);
    return st.games_in_rotation;
}

uint32_t fw_games_qr_request(uint32_t now_ms_){
    return (int32_t)(s_qr_until - now_ms_) > 0 ? s_qr_serial : 0;
}

bool fw_games_active(void){
    bool on;
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    on = fw_arcade_active(&s_arcade);
    xSemaphoreGive(s_lock);
    return on;
}

bool fw_games_frame(const FwSurface *s, uint32_t now){
    bool on;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    on = fw_arcade_advance(&s_arcade, now);
    if (on) fw_arcade_draw(&s_arcade, s);
    xSemaphoreGive(s_lock);
    return on;
}

/* ------------------------------------------------------------ clients */

typedef struct { int fd; int slot; } Session;

static void report(const char *what, int slot){
    char extra[64];
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) n += s_clients[i].fd > 0;
    snprintf(extra, sizeof extra, "\"slot\":%d,\"clients\":%d", slot, n);
    fw_mem_report(what, extra);
}

/* Called by the server when the connection closes, from its own task. */
static void session_closed(void *ctx){
    Session *ss = ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fw_arcade_leave(&s_arcade, ss->slot, now_ms());
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (s_clients[i].fd == ss->fd) s_clients[i].fd = 0;
    xSemaphoreGive(s_lock);
    report("games_left", ss->slot);
    free(ss);
}

static void send_status_to(int fd, int slot){
    char buf[192];
    httpd_ws_frame_t f;
    memset(&f, 0, sizeof f);
    f.type = HTTPD_WS_TYPE_TEXT;
    f.payload = (uint8_t *)buf;
    f.len = fw_arcade_status(&s_arcade, slot, buf, sizeof buf);
    if (f.len < sizeof buf) httpd_ws_send_frame_async(s_server, fd, &f);
}

/* Runs on the server task (httpd_queue_work), where sending is safe. */
static void broadcast(void *arg){
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (s_clients[i].fd > 0) send_status_to(s_clients[i].fd, s_clients[i].slot);
    xSemaphoreGive(s_lock);
}

static void status_task(void *arg){
    (void)arg;
    for (;;) {
        bool due;
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        due = s_arcade.serial != s_sent_serial || esp_timer_get_time() - s_sent_at >= STATUS_EVERY_MS * 1000LL;
        if (due) {
            s_sent_serial = s_arcade.serial;
            s_sent_at = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
        if (due) httpd_queue_work(s_server, broadcast, NULL);
    }
}

static esp_err_t ws_handler(httpd_req_t *req){
    Session *ss = req->sess_ctx;
    if (req->method == HTTP_GET) {
        /* handshake done: a new phone */
        int fd = httpd_req_to_sockfd(req), slot;
        ss = calloc(1, sizeof *ss);
        if (!ss) return ESP_ERR_NO_MEM;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot = fw_arcade_join(&s_arcade, now_ms());
        for (int i = 0; i < MAX_CLIENTS; ++i)
            if (s_clients[i].fd <= 0) { s_clients[i].fd = fd; s_clients[i].slot = slot; break; }
        xSemaphoreGive(s_lock);
        ss->fd = fd;
        ss->slot = slot;
        req->sess_ctx = ss;
        req->free_ctx = session_closed;
        report("games_joined", slot);
        return ESP_OK;
    }
    {
        httpd_ws_frame_t f;
        uint8_t buf[24];
        memset(&f, 0, sizeof f);
        if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
        /* the page sends only short text frames: anything else closes the connection */
        if (f.type != HTTPD_WS_TYPE_TEXT || f.len >= sizeof buf) return ESP_FAIL;
        f.payload = buf;
        if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) return ESP_FAIL;
        if (ss) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            fw_arcade_message(&s_arcade, ss->slot, (const char *)buf, f.len, now_ms());
            xSemaphoreGive(s_lock);
        }
        return ESP_OK;
    }
}

static esp_err_t send_page(httpd_req_t *req, const char *start, const char *end){
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, end - start);
}

static esp_err_t dashboard_handler(httpd_req_t *req){ return send_page(req, dashboard_start, dashboard_end); }
static esp_err_t play_handler(httpd_req_t *req){ return send_page(req, games_page_start, games_page_end); }

static esp_err_t icon_handler(httpd_req_t *req){
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, icon_start, icon_end - icon_start);
}

static esp_err_t manifest_handler(httpd_req_t *req){
    httpd_resp_set_type(req, "application/manifest+json");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, manifest, sizeof manifest - 1);
}

static esp_err_t send_state(httpd_req_t *req){
    static const char *const games[] = {"none", "pong", "breakout"};
    char body[160];
    int players = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < FW_ARCADE_PLAYERS; ++i) players += s_arcade.joined[i];
    snprintf(body, sizeof body, "{\"in_rotation\":%s,\"qr_showing\":%s,\"game\":\"%s\",\"players\":%d}",
             fw_games_in_rotation() ? "true" : "false", fw_games_qr_request(now_ms()) ? "true" : "false",
             games[s_arcade.game], players);
    xSemaphoreGive(s_lock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t api_get(httpd_req_t *req){ return send_state(req); }

/* {"in_rotation": bool} and/or {"show_qr": true} */
static esp_err_t api_post(httpd_req_t *req){
    char buf[BODY_MAX + 1];
    int got = 0, r;
    cJSON *json, *rot, *qr;
    if (req->content_len < 2 || req->content_len > BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    while (got < (int)req->content_len) {
        r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    buf[got] = 0;
    json = cJSON_Parse(buf);
    rot = cJSON_GetObjectItemCaseSensitive(json, "in_rotation");
    qr = cJSON_GetObjectItemCaseSensitive(json, "show_qr");
    if (!cJSON_IsObject(json) || (!cJSON_IsBool(rot) && !cJSON_IsTrue(qr))) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected in_rotation or show_qr");
        return ESP_OK;
    }
    if (cJSON_IsBool(rot)) {
        bool written;
        if (fw_owned_set_games_in_rotation(cJSON_IsTrue(rot), &written) != ESP_OK) {
            cJSON_Delete(json);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not saved");
            return ESP_OK;
        }
    }
    if (cJSON_IsTrue(qr)) {
        s_qr_kind = FW_QR_PLAY;
        s_qr_until = now_ms() + QR_SHOW_MS;
        ++s_qr_serial;
        if (!s_qr_serial) ++s_qr_serial;
        fw_mem_report("games_qr_requested", NULL);
    }
    cJSON_Delete(json);
    return send_state(req);
}

bool fw_games_start(void){
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = dashboard_handler},
        {.uri = "/play", .method = HTTP_GET, .handler = play_handler},
        {.uri = "/icon.png", .method = HTTP_GET, .handler = icon_handler},
        {.uri = "/manifest.webmanifest", .method = HTTP_GET, .handler = manifest_handler},
        {.uri = "/api/games", .method = HTTP_GET, .handler = api_get},
        {.uri = "/api/games", .method = HTTP_POST, .handler = api_post},
    };
    static const httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    if (s_server) return true;
    if (!s_lock && !(s_lock = xSemaphoreCreateMutex())) return false;
    fw_arcade_init(&s_arcade, 128, 64, (uint32_t)esp_timer_get_time());
    c.server_port = FW_GAMES_PORT;
    c.ctrl_port = 32772;
    c.max_open_sockets = MAX_CLIENTS + 1;
    c.max_uri_handlers = 24;
    c.stack_size = 6144;                      /* settings edits parse JSON */
    c.lru_purge_enable = true;
    if (httpd_start(&s_server, &c) != ESP_OK) {
        s_server = NULL;
        fw_mem_report("games_server_failed", NULL);
        return false;
    }
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; ++i) httpd_register_uri_handler(s_server, &routes[i]);
    httpd_register_uri_handler(s_server, &ws);
    fw_dashboard_api_register(s_server);
    xTaskCreate(status_task, "fw_games", 2560, NULL, 4, NULL);
    fw_mem_report("games_server_started", NULL);
    return true;
}
