/* The shared 48 KiB block; see big_buffer.h. */
#include "big_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static uint8_t s_block[CONFIG_FW_TLS_ARENA_BYTES] __attribute__((aligned(8)));
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static volatile bool s_busy, s_wanted;

const size_t fw_big_bytes = CONFIG_FW_TLS_ARENA_BYTES;

void fw_big_init(void){
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
}

uint8_t *fw_big_take(uint32_t wait_ms){
    if (!s_mutex || xSemaphoreTake(s_mutex, wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms)) != pdTRUE)
        return NULL;
    s_busy = true;
    return s_block;
}

void fw_big_give(void){
    s_busy = false;
    xSemaphoreGive(s_mutex);
}

bool fw_big_busy(void){ return s_busy; }

void fw_big_want(bool wanted){ s_wanted = wanted; }
bool fw_big_wanted(void){ return s_wanted; }
