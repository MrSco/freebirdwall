#include "owner_alias.h"

#define READY 0x31414f46u
#define MAGIC 0x31415746u /* FWA1, little endian */
#define HEADER 16u
#define STRIDE 68u

typedef struct { const char *p, *end; unsigned depth; } Json;

static void zero(void *p, size_t n) {
    unsigned char *b = p;
    while (n--) *b++ = 0;
}
static void copy(void *p, const void *q, size_t n) {
    unsigned char *d = p;
    const unsigned char *s = q;
    while (n--) *d++ = *s++;
}
static int eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(unsigned char *p, uint32_t n) {
    unsigned i;
    for (i = 0; i < 4; ++i) { p[i] = (unsigned char)n; n >>= 8; }
}
static uint32_t crc(const unsigned char *p, size_t n) {
    uint32_t v = 0xffffffffu;
    unsigned k;
    while (n--) {
        v ^= *p++;
        for (k = 0; k < 8; ++k) v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1u)));
    }
    return ~v;
}
static unsigned text_length(const char *p) {
    unsigned n;
    for (n = 0; n < OA_TEXT_CAP; ++n) {
        unsigned char c = (unsigned char)p[n];
        if (!c) return n;
        if (c < 32 || c > 126) return 0;
    }
    return 0;
}
static int valid_entry(uint32_t kind, const char *from, const char *to) {
    unsigned i, n = text_length(from);
    if (!n || !text_length(to)) return 0;
    if (kind == OA_NAME) return 1;
    if ((kind == OA_IATA && n == 3) || (kind == OA_ICAO && n == 4)) {
        for (i = 0; i < n; ++i) if (from[i] < 'A' || from[i] > 'Z') return 0;
        return 1;
    }
    return 0;
}
static int valid_blob(const unsigned char *b) {
    uint32_t i, j, n = get32(b + 12);
    if (get32(b) != MAGIC || get32(b + 4) != OA_SCHEMA_VERSION || !get32(b + 8) ||
        n > OA_MAX_ALIASES || get32(b + OA_BLOB_SIZE - 4) != crc(b, OA_BLOB_SIZE - 4))
        return 0;
    for (i = 0; i < n; ++i) {
        const unsigned char *e = b + HEADER + i * STRIDE;
        if (!valid_entry(get32(e), (const char *)e + 4, (const char *)e + 36)) return 0;
        for (j = 0; j < i; ++j) {
            const unsigned char *prior = b + HEADER + j * STRIDE;
            if (get32(e) == get32(prior) && eq((const char *)e + 4, (const char *)prior + 4))
                return 0;
        }
    }
    return 1;
}
static void publish(OwnerAliasContext *c) {
    uint32_t i;
    zero(&c->active, sizeof c->active);
    c->active.revision = get32(c->scratch + 8);
    c->active.count = get32(c->scratch + 12);
    for (i = 0; i < c->active.count; ++i) {
        const unsigned char *e = c->scratch + HEADER + i * STRIDE;
        c->active.entries[i].kind = get32(e);
        copy(c->active.entries[i].from, e + 4, OA_TEXT_CAP);
        copy(c->active.entries[i].to, e + 36, OA_TEXT_CAP);
    }
}
static int enter(OwnerAliasContext *c) {
    if (!c || c->ready != READY) return OA_UNINITIALIZED;
    return c->ops.lock && c->ops.lock(c->ops.user) ? OA_LOCK : OA_OK;
}
static void leave(OwnerAliasContext *c) {
    if (c->ops.unlock) c->ops.unlock(c->ops.user);
}

int owner_alias_init(OwnerAliasContext *c, const OwnerAliasOps *ops,
                     const OwnerAliasEntry *seed, uint32_t n) {
    uint32_t i, j;
    size_t length = OA_BLOB_SIZE;
    int status;
    if (!c) return OA_INVALID;
    zero(c, sizeof *c);
    if (!ops || !ops->read || !ops->write || (!!ops->lock != !!ops->unlock) ||
        n > OA_MAX_ALIASES || (n && !seed)) return OA_INVALID;
    copy(&c->ops, ops, sizeof c->ops);
    for (i = 0; i < n; ++i) {
        if (!valid_entry(seed[i].kind, seed[i].from, seed[i].to)) return OA_INVALID;
        for (j = 0; j < i; ++j)
            if (seed[i].kind == seed[j].kind && eq(seed[i].from, seed[j].from)) return OA_INVALID;
    }
    c->active.count = n;
    for (i = 0; i < n; ++i) copy(c->active.entries + i, seed + i, sizeof *seed);
    status = ops->read(ops->user, OA_NAMESPACE, OA_STORAGE_KEY, c->scratch, &length);
    c->ready = READY;
    if (status == OA_NOT_FOUND) return OA_OK;
    if (status || length != OA_BLOB_SIZE || !valid_blob(c->scratch)) {
        c->storage_fault = 1;
        return OA_STORAGE;
    }
    publish(c);
    return OA_OK;
}

static void ws(Json *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) ++j->p;
}
static int take(Json *j, char c) {
    ws(j);
    if (j->p == j->end || *j->p != c) return 0;
    ++j->p;
    return 1;
}
/* out==NULL structurally validates arbitrary JSON strings. Keys and alias text
 * use bounded printable ASCII; escaped unicode is permitted only when skipped. */
static int string(Json *j, char *out, unsigned cap) {
    unsigned n = 0, i;
    if (!take(j, '"')) return 0;
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') { if (out) out[n] = 0; return 1; }
        if (c < 32) return 0;
        if (c == '\\') {
            if (j->p == j->end) return 0;
            c = (unsigned char)*j->p++;
            if (c == 'u' && !out) {
                for (i = 0; i < 4; ++i) {
                    if (j->p == j->end) return 0;
                    c = (unsigned char)*j->p++;
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
                }
                continue;
            }
            if (c != '"' && c != '\\' && c != '/' &&
                (out || (c != 'b' && c != 'f' && c != 'n' && c != 'r' && c != 't'))) return 0;
        }
        if (!out && c >= 128) {
            unsigned remaining, minimum, point;
            if (c >= 0xc2 && c <= 0xdf) { remaining = 1; minimum = 0x80; point = c & 31; }
            else if (c >= 0xe0 && c <= 0xef) { remaining = 2; minimum = 0x800; point = c & 15; }
            else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; minimum = 0x10000; point = c & 7; }
            else return 0;
            while (remaining--) {
                if (j->p == j->end) return 0;
                c = (unsigned char)*j->p++;
                if (c < 0x80 || c > 0xbf) return 0;
                point = (point << 6) | (c & 63);
            }
            if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return 0;
        }
        if (out) {
            if (c > 126 || n + 1 >= cap) return 0;
            out[n++] = (char)c;
        }
    }
    return 0;
}
static int literal(Json *j, const char *p) {
    while (*p) { if (j->p == j->end || *j->p++ != *p++) return 0; }
    return 1;
}
static int digit(Json *j) { return j->p < j->end && *j->p >= '0' && *j->p <= '9'; }
static int number(Json *j) {
    if (j->p < j->end && *j->p == '-') ++j->p;
    if (!digit(j)) return 0;
    if (*j->p == '0') ++j->p;
    else while (digit(j)) ++j->p;
    if (j->p < j->end && *j->p == '.') {
        ++j->p;
        if (!digit(j)) return 0;
        while (digit(j)) ++j->p;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        ++j->p;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) ++j->p;
        if (!digit(j)) return 0;
        while (digit(j)) ++j->p;
    }
    return 1;
}
static int skip(Json *j) {
    char close;
    ws(j);
    if (j->p == j->end) return 0;
    if (*j->p == '"') return string(j, 0, 0);
    if (*j->p == 't') return literal(j, "true");
    if (*j->p == 'f') return literal(j, "false");
    if (*j->p == 'n') return literal(j, "null");
    if (*j->p != '[' && *j->p != '{') return number(j);
    if (++j->depth > 12) return 0;
    close = *j->p++ == '[' ? ']' : '}';
    if (!take(j, close)) {
        do {
            if (close == '}' && (!string(j, 0, 0) || !take(j, ':'))) return 0;
            if (!skip(j)) return 0;
        } while (take(j, ','));
        if (!take(j, close)) return 0;
    }
    --j->depth;
    return 1;
}
static int uint32_json(Json *j, uint32_t *out) {
    uint32_t v = 0;
    unsigned d;
    ws(j);
    if (!digit(j)) return 0;
    if (*j->p == '0') { ++j->p; *out = 0; return 1; }
    while (digit(j)) {
        d = (unsigned)(*j->p++ - '0');
        if (v > (0xffffffffu - d) / 10u) return 0;
        v = v * 10u + d;
    }
    *out = v;
    return 1;
}
static int entry(Json *j, unsigned char *e) {
    unsigned fields = 0, bit;
    char key[8], kind[8];
    if (!take(j, '{')) return 0;
    do {
        if (!string(j, key, sizeof key) || !take(j, ':')) return 0;
        if (eq(key, "kind")) {
            bit = 1;
            if (!string(j, kind, sizeof kind)) return 0;
            put32(e, eq(kind, "name") ? OA_NAME : eq(kind, "iata") ? OA_IATA : eq(kind, "icao") ? OA_ICAO : 0);
        } else if (eq(key, "from")) {
            bit = 2;
            if (!string(j, (char *)e + 4, OA_TEXT_CAP)) return 0;
        } else if (eq(key, "to")) {
            bit = 4;
            if (!string(j, (char *)e + 36, OA_TEXT_CAP)) return 0;
        } else return 0;
        if (fields & bit) return 0;
        fields |= bit;
    } while (take(j, ','));
    return fields == 7 && take(j, '}') && valid_entry(get32(e), (char *)e + 4, (char *)e + 36);
}
static int envelope(Json *j, unsigned char *b) {
    unsigned fields = 0, bit;
    uint32_t n = 0, v;
    char key[16];
    if (!take(j, '{')) return 0;
    do {
        if (!string(j, key, sizeof key) || !take(j, ':')) return 0;
        if (eq(key, "schema_version")) {
            bit = 1;
            if (!uint32_json(j, &v) || v != OA_SCHEMA_VERSION) return 0;
            put32(b + 4, v);
        } else if (eq(key, "revision")) {
            bit = 2;
            if (!uint32_json(j, &v) || !v) return 0;
            put32(b + 8, v);
        } else if (eq(key, "aliases")) {
            bit = 4;
            if (!take(j, '[')) return 0;
            if (!take(j, ']')) {
                do {
                    if (n == OA_MAX_ALIASES || !entry(j, b + HEADER + n * STRIDE)) return 0;
                    ++n;
                } while (take(j, ','));
                if (!take(j, ']')) return 0;
            }
            put32(b + 12, n);
        } else return 0;
        if (fields & bit) return 0;
        fields |= bit;
    } while (take(j, ','));
    return fields == 7 && take(j, '}');
}
/* The caller already validated the JSON string. Decode ASCII unicode escapes
 * here too, so duplicate reserved keys cannot hide behind escaped spellings. */
static int root_owner_key(const char *p, const char *end) {
    const char *expected = "owner_aliases";
    ++p; --end; /* quotes */
    while (p < end) {
        unsigned c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == 'u') {
                unsigned i;
                c = 0;
                for (i = 0; i < 4; ++i) {
                    unsigned h = (unsigned char)*p++;
                    c = (c << 4) + (h <= '9' ? h - '0' : h <= 'F' ? h - 'A' + 10 : h - 'a' + 10);
                }
            } else return 0; /* Reserved key contains no escaped punctuation. */
        }
        if (!*expected || c != (unsigned char)*expected++) return 0;
    }
    return !*expected;
}
static int parse_document(const char *json, size_t length, unsigned char *b) {
    Json j;
    unsigned found = 0;
    if (!json || !length || length > OA_MAX_JSON) return OA_INVALID;
    j.p = json; j.end = json + length; j.depth = 0;
    if (!take(&j, '{')) return OA_INVALID;
    if (!take(&j, '}')) {
        do {
            /* Arbitrary length vendor keys remain valid and bounded by input. */
            const char *start;
            int owner;
            ws(&j); start = j.p;
            if (!string(&j, 0, 0)) return OA_INVALID;
            owner = root_owner_key(start, j.p);
            if (!take(&j, ':')) return OA_INVALID;
            if (owner) {
                if (found++ || !envelope(&j, b)) return OA_INVALID;
            } else if (!skip(&j)) return OA_INVALID;
        } while (take(&j, ','));
        if (!take(&j, '}')) return OA_INVALID;
    }
    ws(&j);
    if (j.p != j.end) return OA_INVALID;
    return found ? OA_OK : OA_IGNORED;
}
/* Caller owns the context mutex across parse, compare, storage and snapshot. */
static int apply_locked(OwnerAliasContext *c, const char *json, size_t length,
                        int compare, uint32_t expected, OwnerAliasTable *committed) {
    int status;
    zero(c->scratch, sizeof c->scratch);
    put32(c->scratch, MAGIC);
    status = parse_document(json, length, c->scratch);
    if (status == OA_OK) {
        put32(c->scratch + OA_BLOB_SIZE - 4, crc(c->scratch, OA_BLOB_SIZE - 4));
        if (c->storage_fault) status = OA_STORAGE;
        else if (!valid_blob(c->scratch)) status = OA_INVALID;
        else if (compare && expected != c->active.revision) status = OA_STALE;
        else if (compare && (expected == UINT32_MAX || get32(c->scratch + 8) != expected + 1u)) status = OA_INVALID;
        else if (get32(c->scratch + 8) <= c->active.revision) status = OA_STALE;
        else if (c->ops.write(c->ops.user, OA_NAMESPACE, OA_STORAGE_KEY, c->scratch, OA_BLOB_SIZE)) {
            /* A failed commit can leave durability uncertain. Require a fresh
             * read before accepting any subsequent revision. */
            c->storage_fault = 1;
            status = OA_STORAGE;
        }
        else {
            publish(c);
            if (committed) copy(committed, &c->active, sizeof *committed);
        }
    }
    return status;
}
int owner_alias_apply_json(OwnerAliasContext *c, const char *json, size_t length) {
    int status = enter(c);
    if (status) return status;
    status = apply_locked(c, json, length, 0, 0, 0);
    leave(c);
    return status;
}
int owner_alias_apply_json_expected(OwnerAliasContext *c, const char *json,
                                    size_t length, uint32_t expected,
                                    OwnerAliasTable *committed) {
    int status;
    if (!committed) return OA_INVALID;
    status = enter(c);
    if (status) return status;
    status = apply_locked(c, json, length, 1, expected, committed);
    leave(c);
    return status;
}
int owner_alias_lookup(OwnerAliasContext *c, const char *token,
                       const char *value, char out[OA_TEXT_CAP]) {
    uint32_t kind, i, pass;
    int status;
    if (!out) return OA_INVALID;
    out[0] = 0;
    status = enter(c);
    if (status) return status;
    kind = (eq(token, "{orig}") || eq(token, "{dest}")) ? OA_NAME :
           (eq(token, "{orig_iata}") || eq(token, "{dest_iata}")) ? OA_IATA :
           (eq(token, "{orig_icao}") || eq(token, "{dest_icao}")) ? OA_ICAO : 0;
    status = OA_NOT_FOUND;
    for (pass = 0; pass < (kind == OA_NAME ? 2u : 1u) && status == OA_NOT_FOUND; ++pass) {
        for (i = 0; kind && i < c->active.count; ++i) {
            OwnerAliasEntry *e = c->active.entries + i;
            int eligible = pass ? (e->kind == OA_IATA || e->kind == OA_ICAO) : e->kind == kind;
            if (eligible && eq(e->from, value)) {
                copy(out, e->to, OA_TEXT_CAP);
                status = OA_OK;
                break;
            }
        }
    }
    leave(c);
    return status;
}
int owner_alias_snapshot_state(OwnerAliasContext *c, OwnerAliasTable *out,
                               int *storage_ready) {
    int status;
    if (!out || !storage_ready) return OA_INVALID;
    status = enter(c);
    if (status) return status;
    copy(out, &c->active, sizeof *out);
    *storage_ready = !c->storage_fault;
    leave(c);
    return OA_OK;
}
int owner_alias_snapshot(OwnerAliasContext *c, OwnerAliasTable *out) {
    int ready;
    return owner_alias_snapshot_state(c, out, &ready);
}
