/* The wall's status lines in RTC memory; see wall_log.h. */
#include "wall_log.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RING 7168u                      /* RTC slow memory is 8 KiB; the rest holds the guard and header */
#define MAGIC 0x46574c32u              /* "FWL2": the 7 KiB layout (a 4 KiB one starts afresh) */

static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t head;                     /* next write position */
    uint32_t used;                     /* bytes held, <= RING */
    uint32_t boots;
    char data[RING];
} s_ring;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_fmt_lock;   /* one shared format buffer, not 400 B of every task's stack */
static StaticSemaphore_t s_fmt_storage;
static char s_fmt[480];

static void put(const char *p, size_t n){
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; ++i) {
        s_ring.data[s_ring.head] = p[i];
        s_ring.head = (s_ring.head + 1) % RING;
    }
    s_ring.used = s_ring.used + n > RING ? RING : s_ring.used + n;
    portEXIT_CRITICAL(&s_mux);
}

void fw_log_init(void){
    char mark[64];
    if (s_ring.magic != MAGIC || s_ring.head >= RING || s_ring.used > RING) {
        memset(&s_ring, 0, sizeof s_ring);
        s_ring.magic = MAGIC;
    }
    ++s_ring.boots;
    s_fmt_lock = xSemaphoreCreateMutexStatic(&s_fmt_storage);
    snprintf(mark, sizeof mark, "--- boot %u, reset reason %d ---\n", (unsigned)s_ring.boots, (int)esp_reset_reason());
    put(mark, strlen(mark));
}

/* From the panic handler: no lock (the other core is stopped), IRAM only. */
void IRAM_ATTR fw_log_panic(const char *line){
    if (s_ring.magic != MAGIC || s_ring.head >= RING) return;
    for (const char *p = line; ; ++p) {
        s_ring.data[s_ring.head] = *p ? *p : '\n';
        s_ring.head = (s_ring.head + 1) % RING;
        if (s_ring.used < RING) ++s_ring.used;
        if (!*p) break;
    }
}

void fw_log_line(const char *line){
    printf("%s\n", line);
    put(line, strlen(line));
    put("\n", 1);
}

void fw_state(const char *fmt, ...){
    va_list ap;
    bool locked = s_fmt_lock && xSemaphoreTake(s_fmt_lock, portMAX_DELAY) == pdTRUE;
    va_start(ap, fmt);
    if (locked) {
        memcpy(s_fmt, "FWSTATE ", 8);
        vsnprintf(s_fmt + 8, sizeof s_fmt - 8, fmt, ap);
        fw_log_line(s_fmt);
        xSemaphoreGive(s_fmt_lock);
    } else {                          /* before init: print only */
        fputs("FWSTATE ", stdout);
        vprintf(fmt, ap);
        putchar('\n');
    }
    va_end(ap);
}

size_t fw_log_read(size_t from, char *out, size_t cap){
    size_t n = 0;
    portENTER_CRITICAL(&s_mux);
    if (from < s_ring.used) {
        size_t start = (s_ring.head + RING - s_ring.used) % RING;
        n = s_ring.used - from < cap ? s_ring.used - from : cap;
        for (size_t i = 0; i < n; ++i) out[i] = s_ring.data[(start + from + i) % RING];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* fw_mem_report (memory_probe.c) formats into the same shared buffer. */
char *fw_log_buffer_take(size_t *cap){
    if (!s_fmt_lock || xSemaphoreTake(s_fmt_lock, portMAX_DELAY) != pdTRUE) return NULL;
    *cap = sizeof s_fmt;
    return s_fmt;
}

void fw_log_buffer_give(void){
    fw_log_line(s_fmt);
    xSemaphoreGive(s_fmt_lock);
}
