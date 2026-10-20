#include "memory_probe.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wall_log.h"

void fw_mem_snapshot(FwMemSnapshot *out){
    const uint32_t internal=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    out->free_internal=(uint32_t)heap_caps_get_free_size(internal);
    out->min_free_internal=(uint32_t)heap_caps_get_minimum_free_size(internal);
    out->largest_internal=(uint32_t)heap_caps_get_largest_free_block(internal);
    out->free_dma=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    out->largest_dma=(uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
}

void fw_mem_report(const char *stage,const char *extra){
    FwMemSnapshot s;
    UBaseType_t words=uxTaskGetStackHighWaterMark(NULL);
    fw_mem_snapshot(&s);
    size_t cap;
    char *line = fw_log_buffer_take(&cap);       /* shared buffer: no 480 B on each task's stack */
    if (!line) return;
    /* One line so lines from concurrent tasks cannot interleave; also kept for
     * GET /v1/log (wall_log.h). */
    snprintf(line, cap, "FWMEM {\"stage\":\"%s\",\"t_ms\":%llu,\"task\":\"%s\",\"stack_free\":%u,"
           "\"free\":%u,\"min_free\":%u,\"largest\":%u%s%s}",
           stage,(unsigned long long)(esp_timer_get_time()/1000),pcTaskGetName(NULL),
           (unsigned)(words*sizeof(StackType_t)),
           (unsigned)s.free_internal,(unsigned)s.min_free_internal,(unsigned)s.largest_internal,
           extra?",":"",extra?extra:"");
    fw_log_buffer_give();
}
