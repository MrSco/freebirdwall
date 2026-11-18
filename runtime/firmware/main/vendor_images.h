#ifndef FLIGHTWALL_VENDOR_IMAGES_H
#define FLIGHTWALL_VENDOR_IMAGES_H
/* The vendor's images on the wall (UPDATES.md "Vendor images"): the airline
 * logos, weather icons and aircraft pictures live in the vendor's LittleFS
 * partition ("spiffs"), which the vendor firmware keeps current from its own
 * signed filesystem updates. FreeBirdWall reads that partition; this keeps it
 * current the same way, so the weather icons and aircraft pictures of vendor
 * 4.1/4.2 reach walls that left the vendor firmware before them:
 *  - the manifest https://cdn.theflightwall.com/filesystem/mini-v1/manifest.json,
 *    its signature checked against the vendor's published P-256 key over the
 *    canonical text the vendor firmware signs (FUN_4202f53c);
 *  - only a strictly newer version, for this model and track, never a lower
 *    rollback epoch;
 *  - the image fetched in 128 KiB ranges straight into the partition (never
 *    into the spare app slot, which keeps the vendor firmware), then read back
 *    from flash and checked against the manifest's SHA-256 and the vendor's
 *    signature of it, and mounted to see the files are there.
 * Never formats the partition. While it is rewritten the screens use the owned
 * silhouettes and weather icons; a run cut short is finished on the next one.
 * Off with owned_config.vendor_images "off". On the feed task, between polls. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One step when due: a manifest check about two minutes after start and then
 * weekly (sooner after a failure), or the next part of a download. */
void fw_vendor_images_step(bool enabled);

typedef struct {
    char installed[16];      /* the version on the wall, "" unknown */
    char available[16];      /* the manifest's, "" not checked */
    const char *state;       /* "off", "idle", "checking", "downloading", "installed", "up_to_date", "failed" */
    const char *error;       /* why the last run failed, "" */
    uint32_t done, total;    /* bytes of the download */
} FwVendorImagesStatus;
void fw_vendor_images_status(FwVendorImagesStatus *out);

#ifdef __cplusplus
}
#endif
#endif
