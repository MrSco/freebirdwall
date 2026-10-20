/* The board check; see board_check.h. */
#include "board_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"
#include "nvs.h"
#include "wall_log.h"

/* PSRAM_CAP is in later IDF tables only: EFUSE_BLK1 bits 131..132 (IDF 5.1
 * esp32s3 esp_efuse_table.csv): 0 none, 1 8 MB, 2 2 MB. */
static const esp_efuse_desc_t PSRAM_CAP_BITS = {EFUSE_BLK1, 131, 2};
static const esp_efuse_desc_t *PSRAM_CAP[] = {&PSRAM_CAP_BITS, NULL};

/* The FlightWall mini-v1 table, read back from the board (partitions.csv). */
static const struct { const char *label; esp_partition_type_t type; uint32_t addr, size; } TABLE[] = {
    {"nvs", ESP_PARTITION_TYPE_DATA, 0x9000, 0x5000},
    {"otadata", ESP_PARTITION_TYPE_DATA, 0xe000, 0x2000},
    {"app0", ESP_PARTITION_TYPE_APP, 0x10000, 0x2c0000},
    {"app1", ESP_PARTITION_TYPE_APP, 0x2d0000, 0x2c0000},
    {"spiffs", ESP_PARTITION_TYPE_DATA, 0x590000, 0x270000},   /* the vendor's assets */
};

static FwBoard s_board;

const FwBoard *fw_board(void){ return &s_board; }

static bool table_matches(void){
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; ++i) {
        const esp_partition_t *p = esp_partition_find_first(TABLE[i].type, ESP_PARTITION_SUBTYPE_ANY, TABLE[i].label);
        if (!p || p->address != TABLE[i].addr || p->size != TABLE[i].size) return false;
    }
    return true;
}

/* display_config.model from the vendor app's stored settings (read only). */
static void vendor_model(char *out, size_t cap){
    nvs_handle_t h;
    size_t n = 0;
    char *b;
    out[0] = 0;
    if (nvs_open("usercfg", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "json", NULL, &n) == ESP_OK && n > 0 && n <= 4096 && (b = malloc(n + 1))) {
        if (nvs_get_blob(h, "json", b, &n) == ESP_OK) {
            const char *m;
            b[n] = 0;
            if ((m = strstr(b, "\"model\"")) != NULL) {     /* "model" : "mini-v1", spaces allowed */
                size_t i = 0;
                m += 7;
                while (*m == ' ' || *m == '\t') ++m;
                if (*m == ':') {
                    ++m;
                    while (*m == ' ' || *m == '\t') ++m;
                    if (*m == '"') {
                        ++m;
                        while (m[i] && m[i] != '"' && i + 1 < cap) { out[i] = m[i]; ++i; }
                    }
                }
                out[i] = 0;
            }
        }
        free(b);
    }
    nvs_close(h);
}

bool fw_board_check(FwBoard *out){
    FwBoard b;
    uint8_t cap = 0;
    memset(&b, 0, sizeof b);
    b.flash_octal = esp_efuse_read_field_bit(ESP_EFUSE_FLASH_TYPE) ? 1 : 0;
    if (esp_efuse_read_field_blob(PSRAM_CAP, &cap, 2) == ESP_OK) b.psram_cap = cap;
    b.flash_mb = (int)(spi_flash_get_chip_size() >> 20);
    b.partitions = table_matches();
    vendor_model(b.model, sizeof b.model);
    if (b.flash_octal) snprintf(b.why, sizeof b.why, "octal flash: GPIO 33-37 are not free");
    else if (b.psram_cap == 1) snprintf(b.why, sizeof b.why, "octal PSRAM: GPIO 33-37 are not free");
    else if (b.flash_mb != 8) snprintf(b.why, sizeof b.why, "flash is %d MB, not 8 MB", b.flash_mb);
    else if (!b.partitions) snprintf(b.why, sizeof b.why, "partition table is not the FlightWall's");
    else if (b.model[0] && strcmp(b.model, "mini-v1")) snprintf(b.why, sizeof b.why, "vendor model is not mini-v1");
    b.ok = !b.why[0];
    fw_state("{\"board_ok\":%s,\"why\":\"%s\",\"flash_octal\":%d,\"psram_cap\":%d,\"flash_mb\":%d,\"partitions\":%s,"
             "\"model\":\"%s\"}", b.ok ? "true" : "false", b.why, b.flash_octal, b.psram_cap, b.flash_mb,
             b.partitions ? "true" : "false", b.model);
    s_board = b;
    if (out) *out = b;
    return b.ok;
}
