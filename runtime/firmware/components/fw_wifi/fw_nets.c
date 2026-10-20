/* Stock's saved Wi-Fi network list; see fw_nets.h. */
#include "fw_nets.h"
#include <string.h>
#include "cJSON.h"

bool fw_nets_usable(const char *ssid, const char *pass, bool open){
    return ssid && *ssid && (open || (pass && *pass));
}

static const char *copy(const char *s, char **p, char *end){
    size_t n = strlen(s);
    char *at = *p;
    if ((size_t)(end - at) < n + 1) return "";
    memcpy(at, s, n + 1);
    *p = at + n + 1;
    return at;
}

int fw_nets_parse(const char *json, FwNet out[FW_NETS_MAX], char *store, size_t store_cap){
    cJSON *root, *item;
    char *p = store, *end = store + store_cap;
    int n = 0;
    if (!json || !store) return 0;
    root = cJSON_Parse(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return 0;
    }
    cJSON_ArrayForEach(item, root) {
        const cJSON *jssid, *jpass, *jopen;
        const char *ssid, *pass;
        bool open;
        if (n >= FW_NETS_MAX) break;
        if (!cJSON_IsObject(item)) continue;
        jssid = cJSON_GetObjectItemCaseSensitive(item, "ssid");
        jpass = cJSON_GetObjectItemCaseSensitive(item, "pass");
        jopen = cJSON_GetObjectItemCaseSensitive(item, "open");
        ssid = cJSON_IsString(jssid) ? jssid->valuestring : "";
        pass = cJSON_IsString(jpass) ? jpass->valuestring : "";
        open = cJSON_IsTrue(jopen);
        if (!fw_nets_usable(ssid, pass, open)) continue;
        out[n].ssid = copy(ssid, &p, end);
        out[n].pass = copy(pass, &p, end);
        out[n].open = open;
        ++n;
    }
    cJSON_Delete(root);
    return n;
}

int fw_nets_legacy(const char *ssid, const char *pass, bool has_open, bool open, FwNet *out){
    if (!ssid || !*ssid) return 0;
    if (!pass) pass = "";
    if (!*pass) {
        if (!has_open) open = true;
        else if (!open) return 0;
    } else if (!has_open) {
        open = false;
    }
    out->ssid = ssid;
    out->pass = pass;
    out->open = open;
    return 1;
}

int fw_nets_add(const FwNet *list, int n, const FwNet *added, FwNet out[FW_NETS_MAX]){
    int count = 1;
    if (!added || !fw_nets_usable(added->ssid, added->pass, added->open)) return 0;
    out[0] = *added;
    for (int i = 0; i < n && count < FW_NETS_MAX; ++i)
        if (strcmp(list[i].ssid, added->ssid) != 0) out[count++] = list[i];
    return count;
}

/* ArduinoJson's TextFormatter: the escape table without '/' and '\''. */
static char escape(char c){
    static const char table[] = "\"\"\\\\b\bf\fn\nr\rt\t";
    for (const char *t = table; *t; t += 2)
        if (t[1] == c) return t[0];
    return 0;
}

typedef struct {
    char *buf;
    size_t cap, len;
} Out;

static void put(Out *o, char c){
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    ++o->len;
}

static void puts_raw(Out *o, const char *s){
    while (*s) put(o, *s++);
}

static void put_string(Out *o, const char *s){
    put(o, '"');
    for (; *s; ++s) {
        char e = escape(*s);
        if (e) { put(o, '\\'); put(o, e); }
        else put(o, *s);
    }
    put(o, '"');
}

size_t fw_nets_json(const FwNet *list, int n, char *buf, size_t cap){
    Out o = {buf, cap, 0};
    put(&o, '[');
    for (int i = 0; i < n; ++i) {
        if (i) put(&o, ',');
        puts_raw(&o, "{\"ssid\":");
        put_string(&o, list[i].ssid);
        puts_raw(&o, ",\"pass\":");
        put_string(&o, list[i].pass);
        puts_raw(&o, ",\"open\":");
        puts_raw(&o, list[i].open ? "true" : "false");
        put(&o, '}');
    }
    put(&o, ']');
    if (cap) buf[o.len < cap ? o.len : cap - 1] = '\0';
    return o.len;
}
