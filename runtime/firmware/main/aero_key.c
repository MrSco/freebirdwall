/* The owner's FlightAware AeroAPI key; see aero_key.h. */
#include "aero_key.h"
#include <string.h>
#include "nvs.h"

#define NS "fw_owned"
#define KEY "aero_key"

static volatile uint32_t s_serial;

uint32_t fw_aero_key_serial(void){ return s_serial; }

bool fw_aero_key_load(char *out, size_t cap){
    nvs_handle_t h;
    size_t n = cap;
    bool ok = false;
    out[0] = 0;
    if (cap <= FW_AERO_KEY_MAX) return false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_str(h, KEY, out, &n) == ESP_OK && n > 1;
        nvs_close(h);
    }
    if (!ok) out[0] = 0;
    return ok;
}

static bool valid(const char *k){
    size_t n = strlen(k);
    if (n < 8 || n > FW_AERO_KEY_MAX) return false;
    for (; *k; ++k)
        if (!((*k >= 'A' && *k <= 'Z') || (*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '-' || *k == '_'))
            return false;
    return true;
}

esp_err_t fw_aero_key_save(const char *key){
    nvs_handle_t h;
    esp_err_t e;
    bool remove = !key || !*key;
    if (!remove && !valid(key)) return ESP_ERR_INVALID_ARG;
    if ((e = nvs_open(NS, NVS_READWRITE, &h)) != ESP_OK) return e;
    if (remove) {
        e = nvs_erase_key(h, KEY);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    } else {
        e = nvs_set_str(h, KEY, key);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) ++s_serial;
    return e;
}
