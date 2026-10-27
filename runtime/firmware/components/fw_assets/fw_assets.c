#include "fw_assets.h"
#include <string.h>
#include "lfs.h"
#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* littlefs is not thread-safe, and the display, the community worker and the
 * dashboard's logo route all read logos: one caller at a time. The mutex is
 * made before the scheduler starts (a constructor), so it always exists. */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
__attribute__((constructor)) static void make_lock(void){ s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf); }
#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

#define BLOCK 4096
#define CACHE 256

static lfs_t s_lfs;
static struct lfs_config s_cfg;
static bool s_mounted;
static FwAssetsRead s_read;
static void *s_read_ctx;
static uint8_t s_read_buffer[CACHE], s_prog_buffer[CACHE], s_lookahead[16], s_file_buffer[CACHE];

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buf, lfs_size_t size){
    (void)c;
    return s_read(s_read_ctx, block * BLOCK + off, buf, size) == 0 ? 0 : LFS_ERR_IO;
}
/* Unreachable with LFS_READONLY; present only so a misconfiguration fails safe. */
static int bd_refuse(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, const void *p, lfs_size_t n){
    (void)c; (void)b; (void)o; (void)p; (void)n; return LFS_ERR_IO;
}
static int bd_refuse_erase(const struct lfs_config *c, lfs_block_t b){ (void)c; (void)b; return LFS_ERR_IO; }
static int bd_sync(const struct lfs_config *c){ (void)c; return 0; }

static bool mount(FwAssetsRead read, void *ctx, uint32_t partition_bytes){
    if (s_mounted || !read || partition_bytes < BLOCK * 2) return false;
    s_read = read;
    s_read_ctx = ctx;
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.read = bd_read;
    s_cfg.prog = bd_refuse;
    s_cfg.erase = bd_refuse_erase;
    s_cfg.sync = bd_sync;
    s_cfg.read_size = 16;
    s_cfg.prog_size = 16;
    s_cfg.block_size = BLOCK;
    s_cfg.block_count = 0;               /* taken from the superblock */
    s_cfg.cache_size = CACHE;
    s_cfg.lookahead_size = sizeof s_lookahead;
    s_cfg.block_cycles = -1;
    s_cfg.read_buffer = s_read_buffer;
    s_cfg.prog_buffer = s_prog_buffer;
    s_cfg.lookahead_buffer = s_lookahead;
    if (lfs_mount(&s_lfs, &s_cfg) != 0) return false;
    /* A superblock claiming more blocks than the partition holds is not ours. */
    {
        struct lfs_fsinfo info;
        if (lfs_fs_stat(&s_lfs, &info) != 0 || (uint64_t)info.block_count * BLOCK > partition_bytes) {
            lfs_unmount(&s_lfs);
            return false;
        }
    }
    s_mounted = true;
    return true;
}

bool fw_assets_mount(FwAssetsRead read, void *ctx, uint32_t partition_bytes){
    bool ok;
    LOCK();
    ok = mount(read, ctx, partition_bytes);
    UNLOCK();
    return ok;
}

#ifdef ESP_PLATFORM
static int partition_read(void *ctx, uint32_t offset, void *buf, uint32_t size){
    return esp_partition_read((const esp_partition_t *)ctx, offset, buf, size) == ESP_OK ? 0 : -1;
}

bool fw_assets_mount_partition(void){
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    bool ok;
    LOCK();
    ok = s_mounted || (p && mount(partition_read, (void *)p, p->size));
    UNLOCK();
    return ok;
}
#endif

bool fw_assets_exists(const char *path){
    struct lfs_info info;
    bool yes;
    LOCK();
    yes = s_mounted && path && lfs_stat(&s_lfs, path, &info) == 0 && info.type == LFS_TYPE_REG;
    UNLOCK();
    return yes;
}

int fw_assets_file_size(const char *path){
    struct lfs_info info;
    int n = -1;
    LOCK();
    if (s_mounted && path && lfs_stat(&s_lfs, path, &info) == 0 && info.type == LFS_TYPE_REG) n = (int)info.size;
    UNLOCK();
    return n;
}

static int read_file(const char *path, uint8_t *buf, size_t cap){
    lfs_file_t f;
    struct lfs_file_config fc;
    lfs_soff_t size;
    lfs_ssize_t got;
    if (!s_mounted || !path || !buf) return -1;
    memset(&fc, 0, sizeof fc);
    fc.buffer = s_file_buffer;
    if (lfs_file_opencfg(&s_lfs, &f, path, LFS_O_RDONLY, &fc) != 0) return -1;
    size = lfs_file_size(&s_lfs, &f);
    if (size < 0) { lfs_file_close(&s_lfs, &f); return -1; }
    if ((size_t)size > cap) { lfs_file_close(&s_lfs, &f); return -2; }
    got = lfs_file_read(&s_lfs, &f, buf, (lfs_size_t)size);
    lfs_file_close(&s_lfs, &f);
    return got == size ? (int)size : -1;
}

int fw_assets_read_file(const char *path, uint8_t *buf, size_t cap){
    int n;
    LOCK();
    n = read_file(path, buf, cap);
    UNLOCK();
    return n;
}

void fw_assets_unmount(void){
    LOCK();
    if (s_mounted) lfs_unmount(&s_lfs);
    s_mounted = false;
    UNLOCK();
}
