/* An HTTP/1.x response read in pieces; see fw_feed.h. */
#include "fw_feed.h"
#include <stdlib.h>
#include <string.h>

enum { ST_STATUS, ST_HEADERS, ST_BODY, ST_DONE, ST_ERROR };

void fw_http_init(FwHttpResponse *r, uint8_t *body, size_t cap){
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    r->body = body;
    r->cap = cap;
}

static bool prefix_ci(const char *s, const char *p){
    for (; *p; ++s, ++p) {
        char a = *s, b = *p;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}

static const char *value(const char *line, size_t name_len){
    const char *v = line + name_len;
    while (*v == ' ' || *v == '\t') ++v;
    return v;
}

/* One complete line (without CR LF). Returns false on a refusal. */
static bool line_done(FwHttpResponse *r){
    char *l = r->line;
    l[r->line_len] = 0;
    if (r->state == ST_STATUS) {
        /* HTTP/1.x NNN reason */
        if (strncmp(l, "HTTP/1.", 7) || !l[7] || l[8] != ' ') return false;
        if (l[9] < '0' || l[9] > '9' || l[10] < '0' || l[10] > '9' || l[11] < '0' || l[11] > '9') return false;
        r->status = (l[9] - '0') * 100 + (l[10] - '0') * 10 + (l[11] - '0');
        r->state = ST_HEADERS;
        return true;
    }
    if (!r->line_len) {                              /* end of headers */
        if (r->chunked || r->content_length < 1 || (size_t)r->content_length > r->cap) return false;
        r->state = ST_BODY;
        return true;
    }
    if (prefix_ci(l, "content-length:")) {
        const char *v = value(l, 15);
        char *end;
        long n = strtol(v, &end, 10);
        if (end == v || n < 0) return false;
        while (*end == ' ' || *end == '\t') ++end;
        if (*end) return false;
        r->content_length = n;
    } else if (prefix_ci(l, "content-type:")) {
        r->msgpack = prefix_ci(value(l, 13), "application/x-msgpack");
    } else if (prefix_ci(l, "transfer-encoding:")) {
        r->chunked = true;                           /* stock reads a fixed length only */
    }
    return true;
}

int fw_http_feed(FwHttpResponse *r, const uint8_t *data, size_t n){
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = data[i];
        if (r->state == ST_ERROR) return -1;
        if (r->state == ST_DONE) { r->state = ST_ERROR; return -1; }   /* more than announced */
        if (r->state == ST_BODY) {
            size_t take = n - i, left = (size_t)r->content_length - r->body_len;
            if (take > left) { r->state = ST_ERROR; return -1; }
            memcpy(r->body + r->body_len, data + i, take);
            r->body_len += take;
            if (r->body_len == (size_t)r->content_length) r->state = ST_DONE;
            break;
        }
        if (c == '\n') {
            if (r->line_len && r->line[r->line_len - 1] == '\r') --r->line_len;
            if (!line_done(r)) { r->state = ST_ERROR; return -1; }
            r->line_len = 0;
            continue;
        }
        if (r->line_len + 1 >= sizeof r->line) { r->state = ST_ERROR; return -1; }
        r->line[r->line_len++] = (char)c;
    }
    return r->state == ST_DONE ? 1 : r->state == ST_ERROR ? -1 : 0;
}
