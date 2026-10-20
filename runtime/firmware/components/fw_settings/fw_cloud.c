/* Following the FlightWall app: merge its configuration into the owned
 * settings one setting at a time; see fw_cloud.h and SETTINGS.md. */
#include "fw_cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "fw_settings.h"

/* The request's areas replace each other (fw_settings.c drops radius_request
 * and type when radius_requests is saved), so they are one setting. */
static const char *const AREAS[] = {"radius_request", "radius_requests", "geo_request", "geo_requests", "type", NULL};
#define AREAS_KEY "#areas"

static bool is_area(const char *section, const char *key){
    if (strcmp(section, "request_config")) return false;
    for (const char *const *a = AREAS; *a; ++a) if (!strcmp(*a, key)) return true;
    return false;
}

static uint32_t fnv(uint32_t h, const void *p, size_t n){
    const unsigned char *b = p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}
static uint32_t fnv_str(uint32_t h, const char *s){ return fnv(h, s, strlen(s) + 1); }

static uint32_t key_hash(const char *section, const char *key){
    return fnv_str(fnv_str(2166136261u, section), key);
}

/* A fingerprint of a value: its type and contents, members in order. */
static uint32_t value_hash(uint32_t h, const cJSON *v){
    const cJSON *it;
    char num[32];
    if (cJSON_IsFalse(v)) return fnv(h, "f", 1);
    if (cJSON_IsTrue(v)) return fnv(h, "t", 1);
    if (cJSON_IsNull(v)) return fnv(h, "n", 1);
    if (cJSON_IsNumber(v)) { snprintf(num, sizeof num, "d%.17g", v->valuedouble); return fnv_str(h, num); }
    if (cJSON_IsString(v)) return fnv_str(fnv(h, "s", 1), v->valuestring);
    if (cJSON_IsArray(v)) {
        h = fnv(h, "[", 1);
        cJSON_ArrayForEach(it, v) h = value_hash(h, it);
        return fnv(h, "]", 1);
    }
    if (cJSON_IsObject(v)) {
        h = fnv(h, "{", 1);
        cJSON_ArrayForEach(it, v) h = value_hash(fnv_str(h, it->string), it);
        return fnv(h, "}", 1);
    }
    return fnv(h, "?", 1);
}
static uint32_t fingerprint(const cJSON *v){
    uint32_t h = value_hash(2166136261u, v);
    return h ? h : 1;                      /* 0 means "not there" */
}

static uint32_t areas_hash(const cJSON *request){
    uint32_t h = 2166136261u;
    bool any = false;
    for (const char *const *a = AREAS; *a; ++a) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(request, *a);
        h = fnv_str(h, *a);
        if (v) { any = true; h = value_hash(h, v); } else h = fnv(h, "-", 1);
    }
    return any ? (h ? h : 1) : 0;
}

static uint32_t find(const FwCloudBase *b, uint32_t key){
    if (!b) return 0;
    for (uint32_t i = 0; i < b->count; ++i) if (b->key[i] == key) return b->value[i];
    return 0;
}

static bool add(FwCloudBase *b, uint32_t key, uint32_t value){
    if (b->count >= FW_CLOUD_UNITS) return false;
    b->key[b->count] = key;
    b->value[b->count] = value;
    ++b->count;
    return true;
}

/* Every setting in a document (owned_config left out). */
static bool units(const cJSON *root, FwCloudBase *b){
    const cJSON *m, *k;
    memset(b, 0, sizeof *b);
    b->magic = FW_CLOUD_MAGIC;
    cJSON_ArrayForEach(m, root) {
        if (!strcmp(m->string, "owned_config")) continue;
        if (!cJSON_IsObject(m)) { if (!add(b, key_hash(m->string, ""), fingerprint(m))) return false; continue; }
        cJSON_ArrayForEach(k, m)
            if (!is_area(m->string, k->string) && !add(b, key_hash(m->string, k->string), fingerprint(k))) return false;
        if (!strcmp(m->string, "request_config")) {
            uint32_t a = areas_hash(m);
            if (a && !add(b, key_hash(m->string, AREAS_KEY), a)) return false;
        }
    }
    return true;
}

bool fw_cloud_same(const FwCloudBase *a, const FwCloudBase *b){
    if (a->magic != FW_CLOUD_MAGIC || b->magic != FW_CLOUD_MAGIC || a->count != b->count) return false;
    for (uint32_t i = 0; i < a->count; ++i) if (find(b, a->key[i]) != a->value[i]) return false;
    return true;
}

static void put(cJSON *obj, const char *key, const cJSON *value){
    cJSON *copy = cJSON_Duplicate(value, 1);
    if (!copy) return;
    if (cJSON_GetObjectItemCaseSensitive(obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, copy);
    else cJSON_AddItemToObject(obj, key, copy);
}

static cJSON *section(cJSON *root, const char *key){
    cJSON *s = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(s)) {
        s = cJSON_CreateObject();
        if (cJSON_GetObjectItemCaseSensitive(root, key)) cJSON_ReplaceItemInObjectCaseSensitive(root, key, s);
        else cJSON_AddItemToObject(root, key, s);
    }
    return s;
}

/* Copies every setting whose fingerprint moved since old; returns how many. */
static int copy_changed(cJSON *owned, const cJSON *cloud, const FwCloudBase *old){
    const cJSON *m, *k;
    int n = 0;
    cJSON_ArrayForEach(m, cloud) {
        if (!strcmp(m->string, "owned_config")) continue;
        if (!cJSON_IsObject(m)) {
            if (find(old, key_hash(m->string, "")) != fingerprint(m)) { put(owned, m->string, m); ++n; }
            continue;
        }
        cJSON_ArrayForEach(k, m) {
            if (is_area(m->string, k->string)) continue;
            if (find(old, key_hash(m->string, k->string)) != fingerprint(k)) { put(section(owned, m->string), k->string, k); ++n; }
        }
        if (!strcmp(m->string, "request_config")) {
            uint32_t a = areas_hash(m);
            if (a && find(old, key_hash(m->string, AREAS_KEY)) != a) {
                cJSON *r = section(owned, "request_config");
                for (const char *const *name = AREAS; *name; ++name) {
                    const cJSON *v = cJSON_GetObjectItemCaseSensitive(m, *name);
                    cJSON_DeleteItemFromObjectCaseSensitive(r, *name);
                    if (v) put(r, *name, v);
                }
                ++n;
            }
        }
    }
    return n;
}

/* Removes what the app had (in old) and no longer has (not in now). */
static int remove_gone(cJSON *owned, const FwCloudBase *old, const FwCloudBase *now){
    cJSON *m, *k, *next_m, *next_k;
    int n = 0;
    for (m = owned->child; m; m = next_m) {
        next_m = m->next;
        if (!strcmp(m->string, "owned_config")) continue;
        if (!cJSON_IsObject(m)) {
            uint32_t key = key_hash(m->string, "");
            if (find(old, key) && !find(now, key)) { cJSON_Delete(cJSON_DetachItemViaPointer(owned, m)); ++n; }
            continue;
        }
        for (k = m->child; k; k = next_k) {
            uint32_t key = key_hash(m->string, is_area(m->string, k->string) ? AREAS_KEY : k->string);
            next_k = k->next;
            if (find(old, key) && !find(now, key)) { cJSON_Delete(cJSON_DetachItemViaPointer(m, k)); ++n; }
        }
    }
    return n;
}

static int finish(cJSON *root, const char *owned, char **doc){
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return FW_CLOUD_TOO_LARGE;
    if (strlen(text) > FW_SETTINGS_MAX) { cJSON_free(text); return FW_CLOUD_TOO_LARGE; }
    if (strcmp(text, owned)) {
        *doc = malloc(strlen(text) + 1);
        if (!*doc) { cJSON_free(text); return FW_CLOUD_TOO_LARGE; }
        strcpy(*doc, text);
    }
    cJSON_free(text);
    return FW_CLOUD_OK;
}

int fw_cloud_merge(const char *owned, const char *cloud, const FwCloudBase *old, bool full,
                   FwCloudBase *base, char **doc, int *changed){
    cJSON *c = cJSON_Parse(cloud), *o = NULL;
    int rv = FW_CLOUD_OK;
    *doc = NULL;
    *changed = 0;
    if (old && old->magic != FW_CLOUD_MAGIC) old = NULL;
    /* as stock: an object with both sections, or it is not used */
    if (!cJSON_IsObject(c) || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(c, "request_config")) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(c, "display_config"))) { rv = FW_CLOUD_BAD_CONFIG; goto done; }
    if (!units(c, base)) { rv = FW_CLOUD_TOO_MANY; goto done; }
    if (!full && (!old || fw_cloud_same(old, base))) goto done;     /* first sight, or nothing new */
    o = cJSON_Parse(owned);
    if (!cJSON_IsObject(o)) { rv = FW_CLOUD_BAD_OWNED; goto done; }
    if (full) {
        cJSON *next = cJSON_Duplicate(c, 1);
        cJSON *mine = cJSON_DetachItemFromObjectCaseSensitive(o, "owned_config");
        if (!next) { cJSON_Delete(mine); rv = FW_CLOUD_TOO_LARGE; goto done; }
        cJSON_DeleteItemFromObjectCaseSensitive(next, "owned_config");
        if (mine) cJSON_AddItemToObject(next, "owned_config", mine);
        cJSON_Delete(o);
        o = next;
        *changed = (int)base->count;
    } else {
        *changed = copy_changed(o, c, old) + remove_gone(o, old, base);
    }
    rv = finish(o, owned, doc);
    if (rv) *changed = 0;
done:
    cJSON_Delete(o);
    cJSON_Delete(c);
    return rv;
}
