#ifndef FLIGHTWALL_ALIASES_H
#define FLIGHTWALL_ALIASES_H
/* Airport aliases: an owned feature (not in the vendor firmware). The table
 * is the one our owner patch keeps in NVS fw_owner/aliases (format and rules:
 * shared/owner_alias.c, used here as is), edited from the
 * paired dashboard. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Loads the table (missing = no aliases). Returns how many entries. */
int fw_aliases_load(void);

/* The table as JSON for the dashboard; returns the length or -1. */
int fw_aliases_json(char *out, size_t cap);

/* Replaces the table with a JSON array of {"kind","from","to"} if the stored
 * revision is still expected_revision (owner_alias.c checks every entry).
 * Returns an OA_* code; OA_STALE when someone else saved first. */
int fw_aliases_save(const char *aliases_array_json, uint32_t expected_revision, uint32_t *new_revision);

/* FwFlightSettings.alias callback. */
bool fw_aliases_lookup(void *ctx, const char *token, const char *value, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
