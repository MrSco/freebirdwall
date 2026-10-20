/* Airport aliases on the board; see aliases.h. Changed only from the paired
 * dashboard, through owner_alias.c's checked compare-and-swap. */
#include "aliases.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "memory_probe.h"
#include "owner_alias.h"

static OwnerAliasContext s_ctx;
static SemaphoreHandle_t s_lock;
static bool s_ready;

static int read_blob(void *user, const char *ns, const char *key, void *out, size_t *len){
    nvs_handle_t h;
    esp_err_t e;
    (void)user;
    e = nvs_open(ns, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return OA_NOT_FOUND;
    if (e != ESP_OK) return OA_STORAGE;
    e = nvs_get_blob(h, key, out, len);
    nvs_close(h);
    return e == ESP_OK ? OA_OK : e == ESP_ERR_NVS_NOT_FOUND ? OA_NOT_FOUND : OA_STORAGE;
}

static int write_blob(void *user, const char *ns, const char *key, const void *data, size_t len){
    nvs_handle_t h;
    esp_err_t e;
    (void)user;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return OA_STORAGE;
    e = nvs_set_blob(h, key, data, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? OA_OK : OA_STORAGE;
}

static int lock(void *user){ (void)user; return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE ? 0 : -1; }
static void unlock(void *user){ (void)user; xSemaphoreGive(s_lock); }

int fw_aliases_load(void){
    static const OwnerAliasOps ops = {NULL, read_blob, write_blob, lock, unlock};
    OwnerAliasTable table;
    char extra[64];
    int rc;
    if (!s_lock && !(s_lock = xSemaphoreCreateMutex())) return 0;
    rc = owner_alias_init(&s_ctx, &ops, NULL, 0);
    s_ready = rc == OA_OK;
    if (!s_ready || owner_alias_snapshot(&s_ctx, &table) != OA_OK) table.count = 0, table.revision = 0;
    snprintf(extra, sizeof extra, "\"rc\":%d,\"count\":%u,\"revision\":%u", rc, (unsigned)table.count,
             (unsigned)table.revision);
    fw_mem_report("aliases_loaded", extra);
    return s_ready ? (int)table.count : 0;
}

bool fw_aliases_lookup(void *ctx, const char *token, const char *value, char *out, size_t cap){
    char found[OA_TEXT_CAP];
    (void)ctx;
    if (!s_ready || !value || strlen(value) >= OA_TEXT_CAP) return false;   /* as the owner patch */
    if (owner_alias_lookup(&s_ctx, token, value, found) != OA_OK) return false;
    snprintf(out, cap, "%s", found);
    return true;
}

/* {"revision":n,"aliases":[{"kind":"iata","from":"DJT","to":"PBI"},...]} */
int fw_aliases_json(char *out, size_t cap){
    static const char *const kinds[] = {"", "name", "iata", "", "icao"};
    OwnerAliasTable t;
    int n;
    size_t at;
    if (!s_ready || owner_alias_snapshot(&s_ctx, &t) != OA_OK) { t.count = 0; t.revision = 0; }
    n = snprintf(out, cap, "{\"revision\":%u,\"aliases\":[", (unsigned)t.revision);
    at = n > 0 ? (size_t)n : 0;
    for (uint32_t i = 0; i < t.count && at < cap; ++i) {
        char from[2 * OA_TEXT_CAP], to[2 * OA_TEXT_CAP];
        size_t a = 0, b = 0;
        for (const char *p = t.entries[i].from; *p && a + 2 < sizeof from; ++p) {
            if (*p == '"' || *p == '\\') from[a++] = '\\';
            from[a++] = *p;
        }
        for (const char *p = t.entries[i].to; *p && b + 2 < sizeof to; ++p) {
            if (*p == '"' || *p == '\\') to[b++] = '\\';
            to[b++] = *p;
        }
        from[a] = to[b] = 0;
        n = snprintf(out + at, cap - at, "%s{\"kind\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}", i ? "," : "",
                     t.entries[i].kind < 5 ? kinds[t.entries[i].kind] : "", from, to);
        at += n > 0 ? (size_t)n : 0;
    }
    if (at + 3 > cap) return -1;
    snprintf(out + at, cap - at, "]}");
    return (int)(at + 2);
}

int fw_aliases_save(const char *aliases_array_json, uint32_t expected_revision, uint32_t *new_revision){
    OwnerAliasTable committed;
    char *doc;
    size_t n = strlen(aliases_array_json) + 128;
    int rc;
    if (!s_ready) return OA_STORAGE;
    doc = malloc(n);
    if (!doc) return OA_STORAGE;
    snprintf(doc, n, "{\"owner_aliases\":{\"schema_version\":1,\"revision\":%u,\"aliases\":%s}}",
             (unsigned)(expected_revision + 1), aliases_array_json);
    rc = owner_alias_apply_json_expected(&s_ctx, doc, strlen(doc), expected_revision, &committed);
    free(doc);
    if (rc == OA_OK && new_revision) *new_revision = committed.revision;
    {
        char extra[48];
        snprintf(extra, sizeof extra, "\"rc\":%d,\"count\":%u", rc, rc == OA_OK ? (unsigned)committed.count : 0u);
        fw_mem_report("aliases_saved", extra);
    }
    return rc;
}
