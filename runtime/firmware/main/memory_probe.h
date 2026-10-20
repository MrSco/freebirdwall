#pragma once
/* One-line, machine-parsable memory snapshots on the console:
 *   FWMEM {"stage":"boot","free":...,"min_free":...,"largest":...,...}
 * tools/parse_probe_log.py turns a captured log into a report. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t free_internal;      /* MALLOC_CAP_INTERNAL | 8BIT, now */
    uint32_t min_free_internal;  /* low-water mark since boot */
    uint32_t largest_internal;   /* largest single allocatable block now */
    uint32_t free_dma;           /* DMA-capable, needed later by the panel driver */
    uint32_t largest_dma;
} FwMemSnapshot;

void fw_mem_snapshot(FwMemSnapshot *out);

/* Prints one FWMEM line, including the calling task's stack high-water mark
 * in bytes. extra may be NULL or a JSON fragment like
 * "\"round\":1,\"peak_payload\":27424" (no braces). */
void fw_mem_report(const char *stage, const char *extra);

#ifdef __cplusplus
}
#endif
