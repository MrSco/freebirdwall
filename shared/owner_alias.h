#ifndef FLIGHTWALL_OWNER_ALIAS_H
#define FLIGHTWALL_OWNER_ALIAS_H

#include <stddef.h>
#include <stdint.h>

#define OA_MAX_ALIASES 16u
#define OA_TEXT_CAP 32u
#define OA_BLOB_SIZE (20u + OA_MAX_ALIASES * 68u)
#define OA_MAX_JSON 32768u
#define OA_NAMESPACE "fw_owner"
#define OA_STORAGE_KEY "aliases"
#define OA_SCHEMA_VERSION 1u

enum { OA_NAME = 1, OA_IATA = 2, OA_ICAO = 4 };
enum {
    OA_OK = 0, OA_IGNORED = 1, OA_NOT_FOUND = 2,
    OA_INVALID = -1, OA_STORAGE = -2, OA_STALE = -3,
    OA_UNINITIALIZED = -4, OA_LOCK = -5
};
typedef struct {
    uint32_t kind;
    char from[OA_TEXT_CAP], to[OA_TEXT_CAP];
} OwnerAliasEntry;
typedef struct {
    uint32_t revision, count;
    OwnerAliasEntry entries[OA_MAX_ALIASES];
} OwnerAliasTable;

/* read: 0=loaded, OA_NOT_FOUND=absent, all other values=error. Write must
 * atomically set ONE blob and commit it before returning 0. ESP-IDF adapter:
 * nvs_open("fw_owner",...), nvs_set_blob("aliases",...), nvs_commit(...).
 * Never erase the vendor namespace or initialize/erase the NVS partition here.
 * lock/unlock must be paired; both NULL is allowed ONLY for serialized callers.
 * Callbacks cannot reenter this context. Locks cover parsing and NVS operations;
 * use a task mutex, never an ISR critical section or interrupt-disabling lock. */
typedef struct {
    void *user;
    int (*read)(void *, const char *, const char *, void *, size_t *);
    int (*write)(void *, const char *, const char *, const void *, size_t);
    int (*lock)(void *);
    void (*unlock)(void *);
} OwnerAliasOps;
typedef struct {
    OwnerAliasOps ops;
    OwnerAliasTable active;
    unsigned char scratch[OA_BLOB_SIZE];
    uint32_t ready;
    uint32_t storage_fault;
} OwnerAliasContext;

/* Initialize explicitly before publishing the context; do not trust a pointer
 * in an unused firmware RAM word. No allocator, static mutable state, or BSS
 * assumptions. Seed is the optional flash fallback, revision zero. An invalid
 * seed fails closed. Storage corruption/read failure leaves the valid seed in
 * use and returns OA_STORAGE. This makes the context read-only: command writes
 * return OA_STORAGE until a fresh initialization successfully reads storage.
 * An unknown persisted revision must never be overwritten by revision 1 after
 * a transient read failure. Missing storage returns OA_OK. */
int owner_alias_init(OwnerAliasContext *, const OwnerAliasOps *,
                     const OwnerAliasEntry *seed, uint32_t seed_count);

/* Strict full-document JSON parser. Only ROOT owner_aliases is actionable;
 * other vendor fields (including legacy aliases) are structurally skipped.
 * Envelope: {"owner_aliases":{"schema_version":1,"revision":1,
 * "aliases":[{"kind":"iata","from":"DJT","to":"PBI"}]}}
 * All envelope and entry keys mandatory, unique, no unknown keys. revision
 * must increase (1..UINT32_MAX); aliases:[] explicitly clears. Non-ASCII alias
 * text, control chars, duplicate same-kind sources and invalid kinds rejected.
 * Only literal printable ASCII plus JSON quote/slash/backslash escapes are
 * accepted in alias strings; unicode escapes are not alias text. Entire input
 * must parse before any write. Runtime table changes only after write succeeds.
 * A failed write also makes the context read-only until fresh initialization;
 * storage may have committed despite a failure reported by the adapter.
 * Passing NULL/zero length or any invalid document never clears the table. */
int owner_alias_apply_json(OwnerAliasContext *, const char *, size_t);

/* Local compare-and-swap command. One mutex covers full JSON validation,
 * expected revision comparison, durable write, publication and committed copy.
 * The supplied revision must be exactly expected_revision + 1; skipped or
 * exhausted revisions return OA_INVALID, conflicts return OA_STALE. committed
 * is mandatory and is written only on OA_OK, after successful storage commit.
 * Cloud commands keep owner_alias_apply_json's increasing-revision semantics. */
int owner_alias_apply_json_expected(OwnerAliasContext *, const char *, size_t,
                                    uint32_t expected_revision,
                                    OwnerAliasTable *committed);

/* Exact, typed, display-only lookup. Copies the result while locked; no mutable
 * internal pointers escape. Only orig/dest/name/iata/icao template tokens match.
 * Code kinds also apply to an exact code in a name token (legacy behavior).
 * An explicit name-kind match takes precedence over this code fallback.
 * Names permit 1..31 printable ASCII chars; IATA/ICAO sources require exactly
 * 3/4 uppercase letters. Replacement text permits 1..31 printable ASCII chars.
 * Returns OA_OK on match, OA_NOT_FOUND otherwise; out is cleared on no match. */
int owner_alias_lookup(OwnerAliasContext *, const char *token,
                       const char *value, char out[OA_TEXT_CAP]);
int owner_alias_snapshot(OwnerAliasContext *, OwnerAliasTable *);
/* Copy table and write-readiness together under the same mutex. Both output
 * pointers are mandatory. A false storage_ready still permits fallback reads. */
int owner_alias_snapshot_state(OwnerAliasContext *, OwnerAliasTable *,
                               int *storage_ready);

#endif
