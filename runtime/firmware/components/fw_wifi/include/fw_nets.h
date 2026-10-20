#ifndef FW_NETS_H
#define FW_NETS_H
/* Stock's saved Wi-Fi network list (SETUP.md "Saved networks").
 *
 * Stock keeps up to five networks in NVS namespace "wifi", key "nets": a
 * compact JSON array [{"ssid":..,"pass":..,"open":bool},..], newest first,
 * written by ArduinoJson. Older stock builds kept one network in the keys
 * "ssid", "pass" and "open" of the same namespace; stock still reads those
 * when "nets" is absent and removes them once "nets" is written.
 *
 * Portable C: the NVS access stays in the caller. */
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FW_NETS_MAX 5
#define FW_NETS_JSON_MAX 1024   /* stock does not write a longer "nets" value */

typedef struct {
    const char *ssid;
    const char *pass;
    bool open;
} FwNet;

/* Stock's rule for a usable entry (FUN_4202d6c4 on read, FUN_4202dbec on
 * add): a name, and a password unless the network is marked open. */
bool fw_nets_usable(const char *ssid, const char *pass, bool open);

/* Reads a "nets" value as stock does (FUN_4202d6c4): a JSON array; for each
 * object, "ssid" and "pass" when they are strings (else empty) and "open"
 * when it is a boolean (else false); unusable entries are skipped and at most
 * FW_NETS_MAX are kept. Strings are copied into store, which needs
 * strlen(json) + 1 bytes. Returns the number of entries (0 when the value is
 * not a JSON array). */
int fw_nets_parse(const char *json, FwNet out[FW_NETS_MAX], char *store, size_t store_cap);

/* The single legacy entry (keys "ssid", "pass", "open"), read when "nets" is
 * absent: needs a name; with an empty password it is used when "open" is
 * missing (stock then treats it as open) or nonzero. Returns 0 or 1. */
int fw_nets_legacy(const char *ssid, const char *pass, bool has_open, bool open, FwNet *out);

/* Stock FUN_4202dbec: the new network first, then the existing ones with a
 * different name, at most FW_NETS_MAX in all. Returns the count, or 0 when
 * the new network is not usable (stock then saves nothing). */
int fw_nets_add(const FwNet *list, int n, const FwNet *added, FwNet out[FW_NETS_MAX]);

/* Serialises the list the way stock's ArduinoJson writes it: compact, keys in
 * the order ssid, pass, open; '"' '\\' and \b \f \n \r \t escaped, every
 * other byte written as is. Returns the full length (like snprintf); the
 * output is complete when the result is below cap. */
size_t fw_nets_json(const FwNet *list, int n, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
