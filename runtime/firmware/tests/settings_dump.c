/* Runs fw_settings_apply on two files, for tests/test_settings.py.
 *   settings_dump <current.json> <edit.json>  -> {"rc":..,"field":..,"why":..,"doc":<new document or null>} */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_settings.h"

static char *slurp(const char *path){
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) return NULL;
    b[n] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv){
    char *cur, *edit, *out = NULL;
    FwSettingsError err;
    int rc;
    if (argc != 3 || !(cur = slurp(argv[1])) || !(edit = slurp(argv[2]))) return 2;
    rc = fw_settings_apply(cur, edit, "2026-10-19T21:40:00Z", &out, &err);
    printf("{\"rc\":%d,\"field\":\"%s\",\"why\":\"%s\",\"doc\":%s}\n", rc, err.field, err.why, out ? out : "null");
    free(out);
    return 0;
}
