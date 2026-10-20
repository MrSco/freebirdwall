/* Stock's saved network list in NVS; see saved_networks.h. Never logs names
 * or passwords. */
#include "saved_networks.h"
#include <stdlib.h>
#include <string.h>
#include "nvs.h"

#define NAMESPACE "wifi"

void fw_saved_networks_free(FwSavedNetworks *s){
    if (s->store) {
        memset(s->store, 0, s->store_cap);
        free(s->store);
    }
    if (s->nets_json) {
        memset(s->nets_json, 0, strlen(s->nets_json));
        free(s->nets_json);
    }
    memset(s, 0, sizeof *s);
}

static bool str_len(nvs_handle_t h, const char *key, size_t *len){
    *len = 0;
    return nvs_get_str(h, key, NULL, len) == ESP_OK;
}

esp_err_t fw_saved_networks_load(FwSavedNetworks *s){
    nvs_handle_t h;
    size_t len;
    esp_err_t e;
    memset(s, 0, sizeof *s);
    e = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;      /* nothing saved yet */
    if (e != ESP_OK) return e;

    {
        size_t ls, lp;
        uint8_t open;
        s->has_legacy = str_len(h, "ssid", &ls) || str_len(h, "pass", &lp) || nvs_get_u8(h, "open", &open) == ESP_OK;
    }
    if (str_len(h, "nets", &len)) {
        s->has_nets = true;
        s->nets_json = malloc(len);
        s->store = malloc(len);
        s->store_cap = s->store ? len : 0;
        if (!s->nets_json || !s->store) e = ESP_ERR_NO_MEM;
        else e = nvs_get_str(h, "nets", s->nets_json, &len);
        if (e == ESP_OK) s->count = fw_nets_parse(s->nets_json, s->net, s->store, len);
    } else {
        /* legacy single network: only when "nets" is absent */
        size_t ls, lp;
        uint8_t open = 0;
        bool has_ssid = str_len(h, "ssid", &ls), has_pass = str_len(h, "pass", &lp);
        bool has_open = nvs_get_u8(h, "open", &open) == ESP_OK;
        if (has_ssid) {
            if (!has_pass) lp = 1;
            s->store = calloc(1, ls + lp);
            s->store_cap = s->store ? ls + lp : 0;
            if (!s->store) e = ESP_ERR_NO_MEM;
            else if ((e = nvs_get_str(h, "ssid", s->store, &ls)) == ESP_OK && has_pass)
                e = nvs_get_str(h, "pass", s->store + ls, &lp);
            if (e == ESP_OK)
                s->count = fw_nets_legacy(s->store, s->store + ls, has_open, open != 0, &s->net[0]);
        }
    }
    nvs_close(h);
    if (e != ESP_OK) {
        fw_saved_networks_free(s);
        return e;
    }
    return ESP_OK;
}

static esp_err_t erase_if_present(nvs_handle_t h, const char *key){
    esp_err_t e = nvs_erase_key(h, key);
    return e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
}

esp_err_t fw_saved_networks_add(const FwNet *added, bool *written){
    FwSavedNetworks s;
    FwNet out[FW_NETS_MAX];
    char *json;
    size_t len;
    int n;
    nvs_handle_t h;
    esp_err_t e;
    *written = false;
    if (!added || !fw_nets_usable(added->ssid, added->pass, added->open)) return ESP_ERR_INVALID_ARG;
    e = fw_saved_networks_load(&s);
    if (e != ESP_OK) return e;

    n = fw_nets_add(s.net, s.count, added, out);
    len = fw_nets_json(out, n, NULL, 0);
    if (len > FW_NETS_JSON_MAX) {                     /* stock: not saved */
        fw_saved_networks_free(&s);
        return ESP_ERR_INVALID_SIZE;
    }
    json = malloc(len + 1);
    if (!json) {
        fw_saved_networks_free(&s);
        return ESP_ERR_NO_MEM;
    }
    fw_nets_json(out, n, json, len + 1);
    memset(out, 0, sizeof out);
    if (s.has_nets && !s.has_legacy && strcmp(s.nets_json, json) == 0) {
        e = ESP_OK;                                   /* already stored exactly */
    } else if ((e = nvs_open(NAMESPACE, NVS_READWRITE, &h)) == ESP_OK) {
        e = nvs_set_str(h, "nets", json);
        if (e == ESP_OK) e = nvs_commit(h);
        if (e == ESP_OK) {
            *written = true;
            /* stock removes the legacy keys once "nets" is written */
            esp_err_t r = erase_if_present(h, "ssid");
            if (r == ESP_OK) r = erase_if_present(h, "pass");
            if (r == ESP_OK) r = erase_if_present(h, "open");
            if (r == ESP_OK) r = nvs_commit(h);
            e = r;
        }
        nvs_close(h);
    }
    memset(json, 0, len);
    free(json);
    fw_saved_networks_free(&s);
    return e;
}

void fw_station_config(wifi_config_t *c, const char *ssid, const char *pass){
    memset(c, 0, sizeof *c);
    memcpy(c->sta.ssid, ssid, strnlen(ssid, sizeof c->sta.ssid));
    memcpy(c->sta.password, pass, strnlen(pass, sizeof c->sta.password));
    c->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    c->sta.threshold.rssi = -127;
    c->sta.threshold.authmode = *pass ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c->sta.pmf_cfg.capable = true;
}
