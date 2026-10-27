/* Runs a preview scenario through the desktop build of preview/fw_preview.c
 * (tests/test_preview.py runs the same scenario through main/preview.wasm in
 * Node, tests/preview_run.mjs, and compares the frames).
 *   preview_host <scenario.json> <outdir>
 * The scenario is a JSON array of commands:
 *   {"settings":{...}} {"flights":{...}} {"aliases":[...]} {"mode":n}
 *   {"logo":["AAL","class","file.png" or ""]} {"step":[ms, unix]}
 *   {"dump":"name"}   writes <outdir>/<name>.rgba (128 x 64 x 4)
 * and prints "<name> <changed steps>" for each dump. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cJSON.h"

void *pv_alloc(size_t n);
const uint8_t *pv_frame(void);
void pv_logo(const char *al, const char *ac, const uint8_t *png, size_t len);
const char *pv_logo_wanted(int i);
void pv_aliases(const char *json);
void pv_settings(const char *json);
void pv_flights(const char *json);
void pv_mode(int m);
int pv_step(uint32_t now_ms, double unix_s);

static char *slurp(const char *path, long *len){
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

int main(int argc, char **argv){
    long n;
    char *text = argc == 3 ? slurp(argv[1], &n) : NULL;
    cJSON *root = text ? cJSON_Parse(text) : NULL, *c;
    int changed = 0;
    if (!cJSON_IsArray(root)) { fprintf(stderr, "usage: preview_host <scenario.json> <outdir>\n"); return 2; }
    cJSON_ArrayForEach(c, root) {
        cJSON *v = c->child;
        const char *k = v ? v->string : "";
        if (!strcmp(k, "settings") || !strcmp(k, "flights") || !strcmp(k, "aliases")) {
            char *j = cJSON_PrintUnformatted(v);
            if (!strcmp(k, "settings")) pv_settings(j);
            else if (!strcmp(k, "flights")) pv_flights(j);
            else pv_aliases(j);
            free(j);
        } else if (!strcmp(k, "mode")) {
            pv_mode(v->valueint);
        } else if (!strcmp(k, "logo")) {
            const char *file = cJSON_GetArrayItem(v, 2)->valuestring;
            long len = 0;
            char *png = *file ? slurp(file, &len) : NULL;
            pv_logo(cJSON_GetArrayItem(v, 0)->valuestring, cJSON_GetArrayItem(v, 1)->valuestring,
                    (const uint8_t *)png, png ? (size_t)len : 0);
            free(png);
        } else if (!strcmp(k, "step")) {
            changed += pv_step((uint32_t)cJSON_GetArrayItem(v, 0)->valuedouble, cJSON_GetArrayItem(v, 1)->valuedouble);
        } else if (!strcmp(k, "dump")) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s/%s.rgba", argv[2], v->valuestring);
            if (!(f = fopen(path, "wb"))) return 1;
            fwrite(pv_frame(), 1, 128 * 64 * 4, f);
            fclose(f);
            printf("%s %d", v->valuestring, changed);
            for (int i = 0; pv_logo_wanted(i); ++i) printf(" %s", pv_logo_wanted(i));
            printf("\n");
            changed = 0;
        }
    }
    return 0;
}
