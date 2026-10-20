/* Mounts a LittleFS partition image through fw_assets (read-only littlefs),
 * reads every file named in a list and compares it with the copy extracted
 * independently by littlefs-python; checks the image never changes and the
 * logo resolver works against the real filesystem.
 *   test_assets <partition.img> <fixtures-dir> <list.txt> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_assets.h"
#include "fw_logo.h"

static unsigned char *img, *orig;
static long img_size;
static int checks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)

static int mem_read(void *ctx, uint32_t off, void *buf, uint32_t n){
    (void)ctx;
    if ((long)off + (long)n > img_size) return -1;
    memcpy(buf, img + off, n);
    return 0;
}
static bool exists(void *ctx, const char *p){ (void)ctx; return fw_assets_exists(p); }

static unsigned char *slurp(const char *path, long *n){
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)*n);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

int main(int argc, char **argv){
    static uint8_t buf[8192];
    char line[512], path[600], fixture[1024], out[FW_LOGO_PATH_MAX];
    FILE *list;
    int files = 0;
    if (argc != 4) return 2;
    img = slurp(argv[1], &img_size);
    CHECK(img != NULL);
    orig = malloc((size_t)img_size);
    memcpy(orig, img, (size_t)img_size);

    CHECK(!fw_assets_exists("/images/airlines/square/AAL.png"));   /* not mounted yet */
    CHECK(fw_assets_mount(mem_read, NULL, (uint32_t)img_size));
    CHECK(!fw_assets_mount(mem_read, NULL, (uint32_t)img_size));   /* no double mount */

    list = fopen(argv[3], "r");
    CHECK(list != NULL);
    while (fgets(line, sizeof line, list)) {
        long fn;
        unsigned char *want;
        int got;
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        snprintf(path, sizeof path, "/images/airlines/square/%s", line);
        snprintf(fixture, sizeof fixture, "%s/%s", argv[2], line);
        want = slurp(fixture, &fn);
        CHECK(want != NULL);
        CHECK(fw_assets_exists(path));
        got = fw_assets_read_file(path, buf, sizeof buf);
        CHECK(got == fn);
        CHECK(memcmp(buf, want, (size_t)fn) == 0);
        CHECK(fw_assets_read_file(path, buf, (size_t)fn - 1) == -2);   /* too small: refused, not truncated */
        free(want);
        ++files;
    }
    fclose(list);

    CHECK(!fw_assets_exists("/images/airlines/square/ZZZ.png"));
    CHECK(!fw_assets_exists("/images/airlines/square"));            /* a directory is not a file */
    CHECK(fw_assets_read_file("/nope.png", buf, sizeof buf) == -1);

    /* Resolver against the real filesystem, as the stock firmware resolves. */
    CHECK(strcmp(fw_logo_resolve("AAL", NULL, exists, NULL, out, sizeof out), "/images/airlines/square/AAL.png") == 0);
    CHECK(strcmp(fw_logo_resolve("aal", NULL, exists, NULL, out, sizeof out), "/images/airlines/square/AAL.png") == 0);
    CHECK(strcmp(fw_logo_resolve("ZZZ", "Heli", exists, NULL, out, sizeof out), "/images/airlines/square/heli.png") == 0);
    CHECK(strcmp(fw_logo_resolve(NULL, "Smalljet", exists, NULL, out, sizeof out), "/images/airlines/square/smalljet.png") == 0);
    CHECK(strcmp(fw_logo_resolve("ZZZ", "Unknown", exists, NULL, out, sizeof out), FW_LOGO_DEFAULT_PATH) == 0);
    CHECK(strcmp(fw_logo_resolve("AA", NULL, exists, NULL, out, sizeof out), FW_LOGO_DEFAULT_PATH) == 0);
    CHECK(strcmp(fw_logo_resolve("A/L", NULL, exists, NULL, out, sizeof out), FW_LOGO_DEFAULT_PATH) == 0);
    CHECK(strcmp(fw_logo_resolve("AALX", NULL, exists, NULL, out, sizeof out), FW_LOGO_DEFAULT_PATH) == 0);
    CHECK(fw_logo_resolve("AAL", NULL, exists, NULL, out, 8) == NULL);   /* buffer too small */

    fw_assets_unmount();
    CHECK(!fw_assets_exists("/images/airlines/square/AAL.png"));
    CHECK(memcmp(img, orig, (size_t)img_size) == 0);                  /* never written */
    printf("PASS: %d asset checks, %d files byte-identical to the independent extraction, image unchanged\n",
           checks, files);
    return 0;
}
