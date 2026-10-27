/* Setup mode (SETUP.md "Provisioning protocol"), started and stopped by
 * wifi_link.c when Wi-Fi fails and comes back.
 *
 * What it does, and the limits that keep it safe on a pending test image:
 *  - Access point Flightwall-<id> (WPA2, the stock password), started alongside
 *    the station so the board keeps its network. Wi-Fi storage stays RAM-only,
 *    as set at boot: no AP or station setting reaches NVS.
 *  - Plain HTTP on 80: the owned setup page (mobile-first, installable to the
 *    home screen) and the stock routes, only to clients on the access point
 *    ("softap_only"). A captive-portal DNS answer and redirects make phones
 *    open the page by themselves. Optional (CONFIG_FW_SETUP_HTTPS): the stock
 *    HTTPS server with the board's identity certificate, for the vendor app.
 *  - Networks are scanned once before the access point starts and served
 *    from that list; a rescan is explicit, because scanning moves the shared
 *    radio off the access point's channel and phones drop off.
 *  - /provision test-joins the submitted network with the station config
 *    stock builds. With CONFIG_FW_SETUP_SAVE it then saves it the way stock
 *    does: the driver config (only when it differs from the stored one), the
 *    network first in stock's saved list ("wifi"/"nets", saved_networks.c),
 *    the identity marked provisioned if needed; then it restarts after 5 s;
 *    otherwise it restores the previous config and saves nothing. /reset is
 *    refused. /status never includes the api_key.
 *  - Never logs the SSID, password, identity, certificate or key.
 */
#include "setup_server.h"
#include "wall_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_http_server.h"
#if CONFIG_FW_SETUP_HTTPS
#include "esp_https_server.h"
#endif
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "nvs.h"
#include "cJSON.h"
#include "memory_probe.h"
#include "captive_dns.h"
#include "saved_networks.h"
#include "fw_link.h"

#define JOIN_TIMEOUT_MS 25000     /* stock DAT_4200108c = 24999 ms */
#define MESSAGE_MS 2000           /* stock FUN_4201e0a4 default */
#define BODY_MAX 1024             /* stock: 413 above this */

static FwSetupInfo s_info;
static char s_id[16], s_fp[40];   /* served by /.well-known/flightwall-info, never logged */
static bool s_active;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_http, s_https;
#if CONFIG_FW_SETUP_HTTPS
static char *s_cert_pem, *s_key_pem;
#endif

static SemaphoreHandle_t s_lock;           /* message + join state */
static char s_msg1[40], s_msg2[40];
static int64_t s_msg_until;

static volatile bool s_joining, s_scanning;
static EventGroupHandle_t s_join_events;
#define JOIN_GOT_IP BIT0
static volatile uint8_t s_auth_fails, s_not_found, s_other_fails, s_last_reason;

/* ------------------------------------------------------------ display */

static void message(const char *l1, const char *l2, int ms){
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_msg1, sizeof s_msg1, "%s", l1 ? l1 : "");
    snprintf(s_msg2, sizeof s_msg2, "%s", l2 ? l2 : "");
    s_msg_until = esp_timer_get_time() + (int64_t)ms * 1000;
    xSemaphoreGive(s_lock);
}

bool fw_setup_message(char *l1, size_t c1, char *l2, size_t c2){
    bool on;
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    on = esp_timer_get_time() < s_msg_until;
    if (on) { snprintf(l1, c1, "%s", s_msg1); snprintf(l2, c2, "%s", s_msg2); }
    xSemaphoreGive(s_lock);
    return on;
}

bool fw_setup_active(void){ return s_active; }
bool fw_setup_https(void){ return s_https != NULL; }
const FwSetupInfo *fw_setup_info_get(void){ return &s_info; }
bool fw_setup_station_busy(void){ return s_joining; }

/* ------------------------------------------------------------ identity */

static bool nvs_string(nvs_handle_t h, const char *key, char *buf, size_t cap){
    size_t n = cap;
    return nvs_get_str(h, key, buf, &n) == ESP_OK;
}

/* Base64 DER (as stock stores it) -> PEM with the given label, 64 per line. */
#if CONFIG_FW_SETUP_HTTPS
static char *pem(const char *b64, const char *label){
    size_t len = strlen(b64), lines = len / 64 + 1;
    size_t cap = len + lines + 2 * strlen(label) + 64;
    char *out = malloc(cap), *p;
    size_t i;
    if (!out) return NULL;
    p = out + sprintf(out, "-----BEGIN %s-----\n", label);
    for (i = 0; i < len; i += 64) {
        size_t n = len - i < 64 ? len - i : 64;
        memcpy(p, b64 + i, n);
        p += n;
        *p++ = '\n';
    }
    sprintf(p, "-----END %s-----\n", label);
    return out;
}
#endif

static bool load_identity(void){
    nvs_handle_t h;
    char id[16] = {0}, fp[40] = {0};
    char *cert = NULL, *key = NULL;
    size_t key_der = 0;
    bool ok = false;
    if (nvs_open("identity", NVS_READONLY, &h) != ESP_OK) return false;
    cert = calloc(1, 1024);
    key = calloc(1, 512);
    if (cert && key && nvs_string(h, "id", id, sizeof id) && nvs_string(h, "fp", fp, sizeof fp) &&
        fw_setup_info(id, fp, &s_info)) {
        snprintf(s_id, sizeof s_id, "%s", id);
        snprintf(s_fp, sizeof s_fp, "%s", fp);
        ok = true;
    }
#if CONFIG_FW_SETUP_HTTPS
    if (ok && nvs_string(h, "cert_der", cert, 1024) && nvs_string(h, "priv_der", key, 512)) {
        /* stock: RSA label above 512 decoded bytes, EC otherwise (FUN_42023a98) */
        size_t b64 = strlen(key);
        key_der = b64 / 4 * 3 - (b64 && key[b64 - 1] == '=') - (b64 > 1 && key[b64 - 2] == '=');
        s_cert_pem = pem(cert, "CERTIFICATE");
        s_key_pem = pem(key, key_der > 0x200 ? "RSA PRIVATE KEY" : "EC PRIVATE KEY");
    }
#else
    (void)key_der;
#endif
    nvs_close(h);
    if (key) { memset(key, 0, 512); free(key); }
    if (cert) free(cert);
    memset(fp, 0, sizeof fp);
    return ok;
}

/* ------------------------------------------------------------ http helpers */

/* One line per request: path and status only, never bodies or parameters. */
static void log_request(httpd_req_t *req, const char *status){
    char extra[96];
    snprintf(extra, sizeof extra, "\"path\":\"%.40s\",\"status\":%.3s,\"tls\":%s", req->uri, status,
             s_https && req->handle == s_https ? "true" : "false");
    fw_mem_report("setup_request", extra);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body){
    log_request(req, status);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, body);
}

/* stock FUN_4201ee50: only clients on the access point */
static bool from_access_point(httpd_req_t *req){
    struct sockaddr_storage local;
    socklen_t len = sizeof local;
    esp_netif_ip_info_t ap;
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&local, &len) != 0) return false;
    if (esp_netif_get_ip_info(s_ap_netif, &ap) != ESP_OK) return false;
    if (local.ss_family == AF_INET)
        return ((struct sockaddr_in *)&local)->sin_addr.s_addr == ap.ip.addr;
#if LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&local;
        /* IPv4-mapped ::ffff:a.b.c.d */
        const uint32_t *w = (const uint32_t *)&a6->sin6_addr;
        return w[0] == 0 && w[1] == 0 && w[2] == PP_HTONL(0xffff) && w[3] == ap.ip.addr;
    }
#endif
    return false;
}

static bool gate(httpd_req_t *req){
    if (from_access_point(req)) return true;
    send_json(req, "403 Forbidden", "{\"error\":\"softap_only\"}");
    return false;
}

/* ------------------------------------------------------------ routes */

static esp_err_t info_get(httpd_req_t *req){
    char body[96];
    if (!gate(req)) return ESP_OK;
    snprintf(body, sizeof body, "{\"version\":\"FW2\",\"id\":\"%s\",\"fp\":\"%s\"}", s_id, s_fp);
    return send_json(req, "200 OK", body);
}

static const char *encryption_name(wifi_auth_mode_t m){
    switch (m) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WPA_PSK: return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK: case WIFI_AUTH_WPA_WPA2_PSK: return "wpa2_psk";
    case WIFI_AUTH_WPA3_PSK: case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa3_psk";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";
    case WIFI_AUTH_WAPI_PSK: return "wapi_psk";
    default: return "unknown";
    }
}

/* Networks scanned before the access point started (or on an explicit rescan). */
#define SCAN_MAX 20
static wifi_ap_record_t s_scan[SCAN_MAX];
static uint16_t s_scan_n;
static bool s_scan_ok;

static bool scan_now(void){
    wifi_scan_config_t scan = {0};
    uint16_t n = SCAN_MAX;
    char extra[48];
    s_scanning = true;
    s_scan_ok = esp_wifi_scan_start(&scan, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, s_scan) == ESP_OK;
    s_scan_n = s_scan_ok ? n : 0;
    s_scanning = false;
    snprintf(extra, sizeof extra, "\"ok\":%s,\"networks\":%u", s_scan_ok ? "true" : "false", (unsigned)s_scan_n);
    fw_mem_report("setup_scan", extra);
    return s_scan_ok;
}

static esp_err_t list_wifi_get(httpd_req_t *req){
    cJSON *root, *arr;
    char *body, query[32] = {0}, value[8] = {0};
    esp_err_t rc;
    if (!gate(req)) return ESP_OK;
    if (s_scanning || s_joining) return send_json(req, "503 Service Unavailable", "{\"error\":\"scan_in_progress\"}");
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "refresh", value, sizeof value) == ESP_OK && value[0] == '1')
        scan_now();
    if (!s_scan_ok) return send_json(req, "503 Service Unavailable", "{\"error\":\"scan_failed\"}");
    root = cJSON_CreateObject();
    arr = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < s_scan_n; ++i) {
        char bssid[18];
        cJSON *o;
        if (!s_scan[i].ssid[0]) continue;                       /* hidden networks */
        o = cJSON_CreateObject();
        snprintf(bssid, sizeof bssid, "%02x:%02x:%02x:%02x:%02x:%02x", s_scan[i].bssid[0], s_scan[i].bssid[1],
                 s_scan[i].bssid[2], s_scan[i].bssid[3], s_scan[i].bssid[4], s_scan[i].bssid[5]);
        cJSON_AddStringToObject(o, "ssid", (const char *)s_scan[i].ssid);
        cJSON_AddStringToObject(o, "bssid", bssid);
        cJSON_AddNumberToObject(o, "rssi", s_scan[i].rssi);
        cJSON_AddNumberToObject(o, "channel", s_scan[i].primary);
        cJSON_AddStringToObject(o, "encryption", encryption_name(s_scan[i].authmode));
        cJSON_AddBoolToObject(o, "secure", s_scan[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    rc = send_json(req, "200 OK", body ? body : "{\"networks\":[]}");
    free(body);
    return rc;
}

static esp_err_t status_get(httpd_req_t *req){
    wifi_ap_record_t ap;
    char body[96];
    if (!gate(req)) return ESP_OK;
    snprintf(body, sizeof body, "{\"status\":\"%s\",\"model\":\"mini_v1\",\"version\":\"owned-trial\"}",
             s_joining ? "joining" : esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? "online" : "setup");
    return send_json(req, "200 OK", body);
}

/* stock FUN_4201e8e4 */
static const char *reason_name(uint8_t r){
    if (r == 0) return "no_response";
    if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE ||
        r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_802_1X_AUTH_FAILED)
        return "auth_failed";
    if (r == WIFI_REASON_NO_AP_FOUND) return "network_not_found";
    return "association_failed";
}

static unsigned s_clients;
static uint32_t s_joined, s_left;      /* when the phones arrived / the last one left, ms */
static bool s_had_client;

static uint32_t now_ms(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

int fw_setup_clients(uint32_t *joined){
    if (joined) *joined = s_joined;
    return s_active ? (int)s_clients : 0;
}

static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data){
    char extra[48];
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_START || id == WIFI_EVENT_AP_STOP) {
        uint8_t primary = 0;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&primary, &second);
        snprintf(extra, sizeof extra, "\"channel\":%u", (unsigned)primary);
        fw_mem_report(id == WIFI_EVENT_AP_START ? "setup_ap_beaconing" : "setup_ap_stopped", extra);
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_clients++ == 0) {
            /* stock: a phone back within a minute keeps its first join time */
            s_joined = fw_link_phone_joined(now_ms(), s_joined, s_left, s_had_client);
            s_had_client = true;
        }
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_clients) {
        if (--s_clients == 0) s_left = now_ms();
    } else {
        return;
    }
    snprintf(extra, sizeof extra, "\"clients\":%u", s_clients);
    fw_mem_report(id == WIFI_EVENT_AP_STACONNECTED ? "setup_client_joined" : "setup_client_left", extra);
}

static void on_join_event(void *arg, esp_event_base_t base, int32_t id, void *data){
    (void)arg;
    if (!s_joining) return;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_join_events, JOIN_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_last_reason = d->reason;
        if (strcmp(reason_name(d->reason), "auth_failed") == 0) ++s_auth_fails;
        else if (d->reason == WIFI_REASON_NO_AP_FOUND) ++s_not_found;
        else ++s_other_fails;
        if (s_auth_fails <= 2 && s_not_found <= 2) esp_wifi_connect();   /* stock retries up to 3 */
    }
}

#if CONFIG_FW_SETUP_SAVE
static void restart_cb(void *arg){
    (void)arg;
    fw_state("{\"event\":\"provision_restart\"}");
    esp_restart();
}

/* Names (never values) of the station fields that differ, for the log. */
static void diff_names(const wifi_sta_config_t *a, const wifi_sta_config_t *b, char *out, size_t cap){
    size_t n = 0;
    out[0] = 0;
#define FW_DIFF(cond, name) do { if ((cond) && n + sizeof name + 1 < cap) \
        n += (size_t)snprintf(out + n, cap - n, "%s" name, n ? "," : ""); } while (0)
    FW_DIFF(memcmp(a->ssid, b->ssid, sizeof a->ssid), "ssid");
    FW_DIFF(memcmp(a->password, b->password, sizeof a->password), "password");
    FW_DIFF(a->scan_method != b->scan_method, "scan_method");
    FW_DIFF(a->bssid_set != b->bssid_set, "bssid_set");
    FW_DIFF(memcmp(a->bssid, b->bssid, sizeof a->bssid), "bssid");
    FW_DIFF(a->channel != b->channel, "channel");
    FW_DIFF(a->listen_interval != b->listen_interval, "listen_interval");
    FW_DIFF(a->sort_method != b->sort_method, "sort_method");
    FW_DIFF(a->threshold.rssi != b->threshold.rssi, "rssi");
    FW_DIFF(a->threshold.authmode != b->threshold.authmode, "authmode");
    FW_DIFF(a->pmf_cfg.capable != b->pmf_cfg.capable, "pmf_capable");
    FW_DIFF(a->pmf_cfg.required != b->pmf_cfg.required, "pmf_required");
    FW_DIFF(a->rm_enabled != b->rm_enabled, "rm");
    FW_DIFF(a->btm_enabled != b->btm_enabled, "btm");
    FW_DIFF(a->mbo_enabled != b->mbo_enabled, "mbo");
    FW_DIFF(a->transition_disable != b->transition_disable, "transition_disable");
    FW_DIFF(a->sae_pwe_h2e != b->sae_pwe_h2e, "sae_pwe_h2e");
    FW_DIFF(a->failure_retry_cnt != b->failure_retry_cnt, "failure_retry_cnt");
#undef FW_DIFF
    if (!n) snprintf(out, cap, "padding");
}

/* Stock writes only when the config differs (FUN_42036dc4 compares the full
 * config first). Wi-Fi storage is FLASH only around this one call. Returns
 * ESP_OK and sets *changed. */
static esp_err_t save_station(const wifi_config_t *next, const wifi_config_t *stored, bool *changed){
    esp_err_t e;
    *changed = memcmp(&next->sta, &stored->sta, sizeof next->sta) != 0;
    if (!*changed) return ESP_OK;
    {
        char names[200], extra[240];
        diff_names(&next->sta, &stored->sta, names, sizeof names);
        snprintf(extra, sizeof extra, "\"fields\":\"%s\"", names);
        fw_mem_report("setup_station_diff", extra);
    }
    e = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_STA, (wifi_config_t *)next);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    return e;
}

/* Stock FUN_42023330: identity/provisioned = 1 after a successful setup.
 * Written only when it is not already 1. */
static esp_err_t mark_provisioned(bool *wrote){
    nvs_handle_t h;
    uint8_t v = 0;
    esp_err_t e;
    *wrote = false;
    if (nvs_open("identity", NVS_READONLY, &h) == ESP_OK) {
        e = nvs_get_u8(h, "provisioned", &v);
        nvs_close(h);
        if (e == ESP_OK && v == 1) return ESP_OK;
    }
    e = nvs_open("identity", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, "provisioned", 1);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    *wrote = e == ESP_OK;
    return e;
}
#endif

static esp_err_t provision_post(httpd_req_t *req){
    char *buf;
    cJSON *json, *jssid, *jpass, *jopen;
    wifi_config_t trial, saved;
    const char *ssid, *pass;
    bool open, joined;
    int got = 0, r;
    char body[160];
    char net_ssid[33], net_pass[64];     /* wiped before every return below */
    if (!gate(req)) return ESP_OK;
    if (req->content_len < 1 || req->content_len > BODY_MAX)
        return send_json(req, "413 Payload Too Large", "{\"error\":\"too_large\"}");
    buf = calloc(1, req->content_len + 1);
    if (!buf) return send_json(req, "500 Internal Server Error", "{\"error\":\"oom\"}");
    while (got < (int)req->content_len) {
        r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); return ESP_FAIL; }
        got += r;
    }
    json = cJSON_Parse(buf);
    memset(buf, 0, req->content_len);
    free(buf);
    if (!json) return send_json(req, "400 Bad Request", "{\"error\":\"invalid_json\"}");
    jssid = cJSON_GetObjectItem(json, "ssid");
    jpass = cJSON_GetObjectItem(json, "pass");
    jopen = cJSON_GetObjectItem(json, "open");
    ssid = cJSON_IsString(jssid) ? jssid->valuestring : "";
    pass = cJSON_IsString(jpass) ? jpass->valuestring : "";
    open = cJSON_IsBool(jopen) && cJSON_IsTrue(jopen);
    /* stock FUN_4201ff18 */
    if (strlen(ssid) < 1 || strlen(ssid) > 32 || strlen(pass) >= 64 || !(open || *pass)) {
        cJSON_Delete(json);
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid_input\"}");
    }
    if (s_joining || s_scanning) {
        cJSON_Delete(json);
        return send_json(req, "409 Conflict", "{\"error\":\"busy_or_failed\"}");
    }

    fw_station_config(&trial, ssid, pass);
    snprintf(net_ssid, sizeof net_ssid, "%s", ssid);
    snprintf(net_pass, sizeof net_pass, "%s", pass);
    cJSON_Delete(json);
    esp_wifi_get_config(WIFI_IF_STA, &saved);

    message("Connecting...", "", JOIN_TIMEOUT_MS);
    s_auth_fails = s_not_found = s_other_fails = s_last_reason = 0;
    xEventGroupClearBits(s_join_events, JOIN_GOT_IP);
    s_joining = true;
    fw_mem_report("setup_join_start", NULL);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &trial);           /* RAM storage: not persisted */
    esp_wifi_connect();
    {
        int64_t until = esp_timer_get_time() + (int64_t)JOIN_TIMEOUT_MS * 1000;
        joined = false;
        while (esp_timer_get_time() < until && s_auth_fails <= 2 && s_not_found <= 2) {
            if (xEventGroupWaitBits(s_join_events, JOIN_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(100)) & JOIN_GOT_IP) {
                joined = true;
                break;
            }
        }
    }
    {
        char extra[96];
        snprintf(extra, sizeof extra, "\"joined\":%s,\"reason\":%u,\"auth_fails\":%u,\"not_found\":%u",
                 joined ? "true" : "false", (unsigned)s_last_reason, (unsigned)s_auth_fails, (unsigned)s_not_found);
        fw_mem_report("setup_join_end", extra);
    }

#if CONFIG_FW_SETUP_SAVE
    if (joined) {
        /* Keep the joined network, save it as stock does, restart in 5 s. */
        /* Stock: WiFi.begin persists the driver config, the network goes to
         * the front of the saved list once connected (FUN_42021c4c ->
         * FUN_4202dbec), and the identity is marked provisioned. */
        bool changed = false, listed = false, marked = false;
        const FwNet net = {net_ssid, net_pass, open};
        esp_err_t e = save_station(&trial, &saved, &changed);
        esp_err_t l = e == ESP_OK ? fw_saved_networks_add(&net, &listed) : e;
        esp_err_t m = l == ESP_OK ? mark_provisioned(&marked) : l;
        char extra[160];
        static esp_timer_handle_t restart;
        const esp_timer_create_args_t args = {.callback = restart_cb, .name = "fw_prov_restart"};
        memset(&trial, 0, sizeof trial);
        memset(&saved, 0, sizeof saved);
        memset(net_ssid, 0, sizeof net_ssid);
        memset(net_pass, 0, sizeof net_pass);
        s_joining = false;
        snprintf(extra, sizeof extra, "\"err\":%d,\"changed\":%s,\"list_err\":%d,\"list_written\":%s,"
                 "\"provisioned_err\":%d,\"provisioned_written\":%s",
                 (int)e, changed ? "true" : "false", (int)l, listed ? "true" : "false", (int)m, marked ? "true" : "false");
        fw_mem_report("setup_saved", extra);
        if (e != ESP_OK || l != ESP_OK || m != ESP_OK) {
            message("Save failed", "", MESSAGE_MS);
            return send_json(req, "500 Internal Server Error", "{\"error\":\"save_failed\"}");
        }
        message("Connected", "Restarting...", 6000);
        if (esp_timer_create(&args, &restart) == ESP_OK) esp_timer_start_once(restart, 5000000);
        return send_json(req, "200 OK", "{\"status\":\"accepted\",\"restart\":true,\"restart_in_seconds\":5}");
    }
#endif
    /* Not saved: back to the network the board had. */
    memset(net_ssid, 0, sizeof net_ssid);
    memset(net_pass, 0, sizeof net_pass);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &saved);
    memset(&saved, 0, sizeof saved);
    memset(&trial, 0, sizeof trial);
    s_joining = false;
    esp_wifi_connect();

    if (joined) {
        message("Connected", "Trial: not saved", MESSAGE_MS);
        return send_json(req, "200 OK",
                         "{\"status\":\"accepted\",\"restart\":false,\"restart_in_seconds\":0,\"saved\":false}");
    }
    if (s_auth_fails > 2) {
        message("Wrong password?", "", MESSAGE_MS);
        snprintf(body, sizeof body, "{\"error\":\"connection_failed\",\"reason\":\"auth_failed\",\"wifi_reason\":%u}",
                 (unsigned)s_last_reason);
        return send_json(req, "400 Bad Request", body);
    }
    if (s_not_found > 2) {
        message("Network not found", "", MESSAGE_MS);
        snprintf(body, sizeof body,
                 "{\"error\":\"connection_failed\",\"reason\":\"network_not_found\",\"wifi_reason\":%u}",
                 (unsigned)s_last_reason);
        return send_json(req, "400 Bad Request", body);
    }
    message("Failed - Try again", "", MESSAGE_MS);
    snprintf(body, sizeof body, "{\"error\":\"connection_timeout\",\"reason\":\"%s\",\"wifi_reason\":%u}",
             reason_name(s_last_reason), (unsigned)s_last_reason);
    return send_json(req, "408 Request Timeout", body);
}

static esp_err_t disconnect_post(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    return send_json(req, "200 OK", "{\"status\":\"ok\"}");
}

static esp_err_t reset_post(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    /* stock erases settings and restarts; the owned firmware never wipes NVS */
    return send_json(req, "403 Forbidden", "{\"error\":\"not_supported\"}");
}

/* The owned setup page: one small self-contained file, mobile-first. With the
 * manifest and icon it can be added to the home screen. A full PWA (service
 * worker, offline) needs a trusted HTTPS origin, which a device on
 * http://192.168.4.1 cannot have. */
static const char PAGE[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'>"
    "<meta name=theme-color content=#0b1220><meta name=apple-mobile-web-app-capable content=yes>"
    "<meta name=apple-mobile-web-app-status-bar-style content=black-translucent>"
    "<link rel=manifest href=/manifest.json><link rel=icon href=/icon.svg?v=3><link rel=apple-touch-icon href=/icon.png?v=3>"
    "<title>FreeBirdWall setup</title><style>"
    ":root{--bg:#0b1220;--card:#141c2e;--line:#26314a;--text:#e8eefc;--dim:#93a0bd;--ok:#2fbf71;--bad:#ef5b5b;--acc:#ffb020}"
    "*{box-sizing:border-box}body{margin:0;font:16px/1.4 system-ui,-apple-system,sans-serif;background:var(--bg);"
    "color:var(--text);padding:max(16px,env(safe-area-inset-top)) 16px 32px}main{max-width:480px;margin:0 auto}"
    "h1{font-size:22px;margin:8px 0 4px}p{color:var(--dim);margin:0 0 16px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px;margin:12px 0}"
    ".net{display:flex;align-items:center;gap:12px;padding:12px;border-radius:10px;cursor:pointer;min-height:48px}"
    ".net:active,.net.sel{background:#1e2942}.net b{flex:1;font-weight:500;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".bars{display:flex;gap:2px;align-items:flex-end;height:16px}.bars i{width:4px;background:#3d4a66;border-radius:1px}"
    ".bars i.on{background:var(--text)}.lock{color:var(--dim);font-size:13px}"
    "label{display:block;color:var(--dim);font-size:14px;margin:12px 0 6px}"
    "input{width:100%;font:inherit;padding:14px;border-radius:10px;border:1px solid var(--line);background:#0e1626;color:var(--text)}"
    ".pw{position:relative}.pw button{position:absolute;right:6px;top:6px;bottom:6px;border:0;background:none;color:var(--dim);font:inherit}"
    ".go{width:100%;margin-top:16px;padding:16px;border:0;border-radius:12px;background:var(--acc);color:#1a1200;font:600 17px system-ui}"
    ".go:disabled{opacity:.5}.link{background:none;border:0;color:var(--dim);font:inherit;text-decoration:underline;padding:8px 0}"
    "#out{margin-top:16px;padding:14px;border-radius:12px;display:none}#out.ok{display:block;background:#123222;color:#bff0d3}"
    "#out.bad{display:block;background:#3a1717;color:#ffd0d0}#out.wait{display:block;background:var(--card);color:var(--dim)}"
    "</style></head><body><main><h1>FreeBirdWall setup</h1><p>Pick the Wi-Fi network the wall should use.</p>"
    "<div class=card id=list><div class=net>Loading networks…</div></div>"
    "<button class=link id=rescan>Scan again</button>"
    "<label for=s>Network name</label><input id=s autocomplete=off autocapitalize=none spellcheck=false>"
    "<label for=p>Password</label><div class=pw><input id=p type=password autocomplete=off>"
    "<button id=eye type=button>Show</button></div>"
    "<button class=go id=go>Connect</button><div id=out></div></main><script>"
    "const $=i=>document.getElementById(i),out=(c,t)=>{const o=$('out');o.className=c;o.textContent=t};let open={};"
    "function bars(r){const n=r>-55?4:r>-67?3:r>-78?2:1;return'<span class=bars>'+[1,2,3,4].map(i=>"
    "'<i class='+(i<=n?'on':'')+' style=height:'+i*4+'px></i>').join('')+'</span>'}"
    "function show(j){const l=$('list');l.innerHTML='';const seen={};(j.networks||[]).forEach(n=>{if(seen[n.ssid])return;"
    "seen[n.ssid]=1;open[n.ssid]=!n.secure;const d=document.createElement('div');d.className='net';"
    "d.innerHTML=bars(n.rssi)+'<b></b>'+(n.secure?'<span class=lock>🔒</span>':'<span class=lock>open</span>');"
    "d.querySelector('b').textContent=n.ssid;d.onclick=()=>{[...l.children].forEach(c=>c.classList.remove('sel'));"
    "d.classList.add('sel');$('s').value=n.ssid;$('p').focus()};l.appendChild(d)});"
    "if(!l.children.length)l.innerHTML='<div class=net>No networks found. Type the name below.</div>'}"
    "function load(r){$('list').innerHTML='<div class=net>'+(r?'Scanning… your phone may drop off for a moment.':'Loading networks…')+'</div>';"
    "fetch('/list_wifi'+(r?'?refresh=1':'')).then(x=>x.json()).then(show).catch(()=>{$('list').innerHTML="
    "'<div class=net>Could not load networks. Type the name below.</div>'})}"
    "$('rescan').onclick=()=>load(1);$('eye').onclick=()=>{const p=$('p');p.type=p.type=='password'?'text':'password';"
    "$('eye').textContent=p.type=='password'?'Show':'Hide'};"
    "const why={auth_failed:'Wrong password.',network_not_found:'Network not found. Check the name and that it is 2.4 GHz.',"
    "association_failed:'The network refused the connection.',no_response:'No answer from the network.'};"
    "$('go').onclick=()=>{const ssid=$('s').value.trim(),pass=$('p').value;if(!ssid){out('bad','Enter a network name.');return}"
    "$('go').disabled=true;out('wait','Connecting… watch the wall’s display.');"
    "fetch('/provision',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:ssid,pass:pass,open:!pass&&!!open[ssid]})}).then(r=>r.json().then(j=>{"
    "if(r.ok)out('ok',j.restart?'Saved. The wall is restarting and will join '+ssid+'. You can switch your phone back to your usual Wi-Fi.':'Connected to '+ssid+'. (Test only: not saved.)');"
    "else out('bad',why[j.reason]||(j.error=='invalid_input'?'Check the name and password.':'Could not connect: '+(j.reason||j.error)))}))"
    ".catch(()=>out('bad','Lost contact with the wall. Rejoin its network and try again.'))"
    ".finally(()=>$('go').disabled=false)};load(0);</script></body></html>";

static const char MANIFEST[] =
    "{\"name\":\"FreeBirdWall setup\",\"short_name\":\"FreeBirdWall\",\"start_url\":\"/\",\"display\":\"standalone\","
    "\"background_color\":\"#0b1220\",\"theme_color\":\"#0b1220\","
    "\"icons\":[{\"src\":\"/icon.svg?v=3\",\"sizes\":\"any\",\"type\":\"image/svg+xml\",\"purpose\":\"any maskable\"},"
    "{\"src\":\"/icon.png?v=3\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any maskable\"}]}";

/* The same icons as the dashboard (tools/make_icon.py, embedded by
 * CMakeLists.txt); bump ?v= with games_server.c when they change. */
extern const char icon_svg_start[] asm("_binary_icon_svg_start");
extern const char icon_svg_end[] asm("_binary_icon_svg_end");
extern const char icon_png_start[] asm("_binary_icon_png_start");
extern const char icon_png_end[] asm("_binary_icon_png_end");

static esp_err_t page_get(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    log_request(req, "200");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, sizeof PAGE - 1);
}

static esp_err_t manifest_get(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/manifest+json");
    return httpd_resp_send(req, MANIFEST, sizeof MANIFEST - 1);
}

static esp_err_t icon_get(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, icon_svg_start, icon_svg_end - icon_svg_start);
}

static esp_err_t icon_png_get(httpd_req_t *req){
    if (!gate(req)) return ESP_OK;
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, icon_png_start, icon_png_end - icon_png_start);
}

/* Captive portal: any unknown page (the phones' connectivity checks among
 * them) is redirected to the setup page, so it opens by itself. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err){
    (void)err;
    if (!from_access_point(req)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }
    log_request(req, "302");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

/* Captive portal DNS: on the access point only, every A query is answered
 * with 192.168.4.1; other types get an empty answer. */
static void dns_task(void *arg){
    struct sockaddr_in local = {0};
    esp_netif_ip_info_t ip;
    uint8_t buf[512];
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    (void)arg;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = ip.ip.addr;
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof local) != 0) {
        fw_mem_report("setup_dns_failed", NULL);
        if (fd >= 0) close(fd);
        vTaskDelete(NULL);
        return;
    }
    fw_mem_report("setup_dns_started", NULL);
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        int out = n > 0 ? fw_dns_answer(buf, n, sizeof buf, (const uint8_t *)&ip.ip.addr) : 0;
        if (out <= 0) continue;
        sendto(fd, buf, out, 0, (struct sockaddr *)&from, flen);
    }
}

static void register_routes(httpd_handle_t h){
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/.well-known/flightwall-info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/provision", .method = HTTP_POST, .handler = provision_post},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/disconnect", .method = HTTP_POST, .handler = disconnect_post},
        {.uri = "/disconnect_client", .method = HTTP_POST, .handler = disconnect_post},
        {.uri = "/reset", .method = HTTP_POST, .handler = reset_post},
        {.uri = "/list_wifi", .method = HTTP_GET, .handler = list_wifi_get},
        {.uri = "/manifest.json", .method = HTTP_GET, .handler = manifest_get},
        {.uri = "/icon.svg", .method = HTTP_GET, .handler = icon_get},
        {.uri = "/icon.png", .method = HTTP_GET, .handler = icon_png_get},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; ++i) httpd_register_uri_handler(h, &routes[i]);
}

/* ------------------------------------------------------------ start / stop */

static bool s_ready;

/* Once per boot: identity, event handlers, the access point interface and
 * the captive DNS task (it answers only while the access point is up). */
static bool setup_init(void){
    if (s_ready) return true;
    s_lock = xSemaphoreCreateMutex();
    s_join_events = xEventGroupCreate();
    if (!s_lock || !s_join_events) return false;
    if (!load_identity()) {
        message("WiFi", "Identity error", 60000);
        fw_mem_report("setup_identity_error", NULL);
        return false;
    }
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_ap_event, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, on_ap_event, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_join_event, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_event, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_join_event, NULL, NULL);
    s_ap_netif = esp_netif_create_default_wifi_ap();      /* 192.168.4.1/24, DHCP server */
    xTaskCreate(dns_task, "fw_dns", 3072, NULL, 4, NULL);
    s_ready = true;
    return true;
}

bool fw_setup_start(void){
    wifi_config_t ap;
    char extra[96];
    if (s_active) return true;
    if (!setup_init()) return false;
    scan_now();                                           /* before the AP: no phone to knock off */
    memset(&ap, 0, sizeof ap);
    memcpy(ap.ap.ssid, s_info.ssid, strlen(s_info.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_info.ssid);
    memcpy(ap.ap.password, s_info.password, strlen(s_info.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        memset(&ap, 0, sizeof ap);
        message("WiFi", "AP start failed", 60000);
        fw_mem_report("setup_ap_failed", NULL);
        return false;
    }
    memset(&ap, 0, sizeof ap);
    s_clients = 0;
    fw_mem_report("setup_ap_started", NULL);

    {
        httpd_config_t c = HTTPD_DEFAULT_CONFIG();
        c.server_port = 80;
        c.ctrl_port = 32770;
        c.stack_size = 6144;
        c.max_open_sockets = 3;
        c.lru_purge_enable = true;
        c.max_uri_handlers = 12;
        if (httpd_start(&s_http, &c) == ESP_OK) {
            register_routes(s_http);
            httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, not_found);
        }
    }
#if CONFIG_FW_SETUP_HTTPS
    {
        httpd_ssl_config_t c = HTTPD_SSL_CONFIG_DEFAULT();
        c.httpd.ctrl_port = 32771;
        c.httpd.stack_size = 8192;
        c.httpd.max_open_sockets = 2;
        c.cacert_pem = (const uint8_t *)s_cert_pem;
        c.cacert_len = strlen(s_cert_pem) + 1;
        c.prvtkey_pem = (const uint8_t *)s_key_pem;
        c.prvtkey_len = strlen(s_key_pem) + 1;
        c.httpd.max_uri_handlers = 12;
        if (s_cert_pem && s_key_pem && httpd_ssl_start(&s_https, &c) == ESP_OK) register_routes(s_https);
    }
#endif
    snprintf(extra, sizeof extra, "\"http\":%s,\"https\":%s", s_http ? "true" : "false", s_https ? "true" : "false");
    fw_mem_report("setup_servers", extra);
    s_active = true;
    return true;
}

void fw_setup_stop(void){
    if (!s_active) return;
    s_active = false;
    if (s_http) { httpd_stop(s_http); s_http = NULL; }
#if CONFIG_FW_SETUP_HTTPS
    if (s_https) { httpd_ssl_stop(s_https); s_https = NULL; }
#endif
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_clients = 0;
    fw_mem_report("setup_stopped", NULL);
}
