/* The owned settings on the board; see settings_store.h. Never logs the
 * document (it holds the owner's home coordinates). */
#include "settings_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "fw_settings.h"
#include "memory_probe.h"

#define NS "fw_owned"

/* NULL until fw_store_init (after Wi-Fi is up); the panel and the dashboard
 * ask earlier, so every entry point copes with "not ready yet". */
static SemaphoreHandle_t s_lock;
static char *s_doc;                 /* heap copy of the stored document */
static uint32_t s_rev;
static volatile uint32_t s_serial;
static FwPair s_pair;

uint32_t fw_store_serial(void){ return s_serial; }

static char *read_blob(const char *ns, const char *key){
    nvs_handle_t h;
    size_t n = 0;
    char *b = NULL;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return NULL;
    if (nvs_get_blob(h, key, NULL, &n) == ESP_OK && n > 0 && n <= FW_SETTINGS_MAX && (b = malloc(n + 1))) {
        if (nvs_get_blob(h, key, b, &n) == ESP_OK) b[n] = 0;
        else { free(b); b = NULL; }
    }
    nvs_close(h);
    return b;
}

static esp_err_t write_doc(const char *doc, uint32_t rev){
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "config", doc, strlen(doc));
    if (e == ESP_OK) e = nvs_set_u32(h, "config_rev", rev);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* Takes ownership of doc. */
static void publish(char *doc, uint32_t rev){
    free(s_doc);
    s_doc = doc;
    s_rev = rev;
    ++s_serial;
}

static esp_err_t import(uint32_t new_rev){
    char *vendor = read_blob("usercfg", "json");
    esp_err_t e;
    char extra[64];
    if (!vendor) return ESP_ERR_NOT_FOUND;
    e = write_doc(vendor, new_rev);
    snprintf(extra, sizeof extra, "\"bytes\":%u,\"revision\":%u,\"err\":%d", (unsigned)strlen(vendor), (unsigned)new_rev, (int)e);
    fw_mem_report("settings_imported", extra);
    if (e != ESP_OK) { free(vendor); return e; }
    publish(vendor, new_rev);
    return ESP_OK;
}

void fw_store_init(void){
    nvs_handle_t h;
    uint32_t rev = 0;
    char extra[64];
    SemaphoreHandle_t lock;
    if (s_lock) return;
    lock = xSemaphoreCreateMutex();
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);    /* held while loading: nobody sees half a store */
    s_lock = lock;
    /* paired browsers */
    {
        FwPairStore st;
        size_t n = sizeof st;
        bool ok = false;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            ok = nvs_get_blob(h, "pair", &st, &n) == ESP_OK && n == sizeof st;
            if (nvs_get_u32(h, "config_rev", &rev) != ESP_OK) rev = 0;
            nvs_close(h);
        }
        fw_pair_init(&s_pair, ok ? &st : NULL);
    }
    s_doc = read_blob(NS, "config");
    if (s_doc) {
        s_rev = rev ? rev : 1;
        ++s_serial;
        snprintf(extra, sizeof extra, "\"bytes\":%u,\"revision\":%u,\"paired\":%u", (unsigned)strlen(s_doc),
                 (unsigned)s_rev, (unsigned)s_pair.store.count);
        fw_mem_report("settings_loaded", extra);
    } else if (import(1) != ESP_OK) {
        fw_mem_report("settings_none", NULL);       /* no vendor copy either: the dashboard starts from defaults */
    }
    xSemaphoreGive(lock);
}

char *fw_store_settings(uint32_t *revision){
    char *copy = NULL;
    if (!s_lock) { if (revision) *revision = 0; return NULL; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_doc && (copy = malloc(strlen(s_doc) + 1))) strcpy(copy, s_doc);
    if (revision) *revision = s_rev;
    xSemaphoreGive(s_lock);
    return copy;
}

int fw_store_settings_copy(char *buf, size_t cap, uint32_t *revision){
    int n = 0;
    if (revision) *revision = 0;
    if (!s_lock || !cap) return 0;
    buf[0] = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_doc) {
        size_t len = strlen(s_doc);
        if (len < cap) { memcpy(buf, s_doc, len + 1); n = (int)len; }
        else n = -1;
    }
    if (revision) *revision = s_rev;
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t fw_store_save(const char *doc, uint32_t *revision){
    esp_err_t e;
    char *copy;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (*revision != s_rev) { *revision = s_rev; xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    copy = malloc(strlen(doc) + 1);
    if (!copy) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
    strcpy(copy, doc);
    e = write_doc(copy, s_rev + 1);
    if (e == ESP_OK) publish(copy, s_rev + 1); else free(copy);
    *revision = s_rev;
    xSemaphoreGive(s_lock);
    {
        char extra[48];
        snprintf(extra, sizeof extra, "\"revision\":%u,\"err\":%d", (unsigned)*revision, (int)e);
        fw_mem_report("settings_saved", extra);
    }
    return e;
}

FwPair *fw_store_pair_lock(void){
    if (!s_lock) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_pair;
}

void fw_store_pair_unlock(bool save){
    if (!s_lock) return;
    if (save) {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_set_blob(h, "pair", &s_pair.store, sizeof s_pair.store) == ESP_OK) nvs_commit(h);
            nvs_close(h);
        }
    }
    xSemaphoreGive(s_lock);
}
