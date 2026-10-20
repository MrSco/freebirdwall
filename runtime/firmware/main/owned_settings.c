/* Owned-firmware settings in NVS namespace "fw_owned"; see owned_settings.h. */
#include "owned_settings.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "memory_probe.h"

#define NAMESPACE "fw_owned"
#define KEY_GAMES_ROTATION "games_rot"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static FwOwnedSettings s_cache;
static bool s_loaded;

static void load(void){
    nvs_handle_t h;
    uint8_t v;
    FwOwnedSettings s;
    memset(&s, 0, sizeof s);
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {        /* missing namespace: defaults */
        if (nvs_get_u8(h, KEY_GAMES_ROTATION, &v) == ESP_OK) s.games_in_rotation = v != 0;
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_mux);
    s_cache = s;
    s_loaded = true;
    portEXIT_CRITICAL(&s_mux);
}

void fw_owned_settings_get(FwOwnedSettings *out){
    if (!s_loaded) load();
    portENTER_CRITICAL(&s_mux);
    *out = s_cache;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t fw_owned_set_games_in_rotation(bool on, bool *written){
    static SemaphoreHandle_t lock;
    FwOwnedSettings cur;
    nvs_handle_t h;
    esp_err_t e;
    char extra[64];
    *written = false;
    if (!lock && !(lock = xSemaphoreCreateMutex())) return ESP_ERR_NO_MEM;
    xSemaphoreTake(lock, portMAX_DELAY);
    fw_owned_settings_get(&cur);
    if (cur.games_in_rotation == on) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    e = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_u8(h, KEY_GAMES_ROTATION, on ? 1 : 0);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_cache.games_in_rotation = on;
        portEXIT_CRITICAL(&s_mux);
        *written = true;
    }
    xSemaphoreGive(lock);
    snprintf(extra, sizeof extra, "\"games_in_rotation\":%s,\"err\":%d", on ? "true" : "false", (int)e);
    fw_mem_report("owned_setting_saved", extra);
    return e;
}
