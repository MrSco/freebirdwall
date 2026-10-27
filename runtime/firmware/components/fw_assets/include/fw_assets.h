#ifndef FLIGHTWALL_FW_ASSETS_H
#define FLIGHTWALL_FW_ASSETS_H
/* Read-only access to the vendor's LittleFS asset partition (airline logos,
 * category icons, system logo). Never writes: littlefs is built with
 * LFS_READONLY and the block device has no write path. On the board every
 * call takes one mutex, so any task may call (the display, the community
 * worker, the dashboard's logo route). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Reads `size` bytes at byte `offset` of the partition. 0 = success. */
typedef int (*FwAssetsRead)(void *ctx, uint32_t offset, void *buf, uint32_t size);

/* Mounts the filesystem through `read`. Same geometry the vendor image was
 * verified with offline: 4 KiB blocks, block count from the superblock. */
bool fw_assets_mount(FwAssetsRead read, void *ctx, uint32_t partition_bytes);

#ifdef ESP_PLATFORM
/* Mounts the board's data partition labelled "spiffs" (it holds LittleFS);
 * true when it is mounted, also when it already was. */
bool fw_assets_mount_partition(void);
#endif

bool fw_assets_exists(const char *path);

/* Reads a whole file into buf. Returns its size, or -1 if missing/unreadable,
 * or -2 if it is larger than cap. */
int fw_assets_read_file(const char *path, uint8_t *buf, size_t cap);

void fw_assets_unmount(void);

#ifdef __cplusplus
}
#endif
#endif
