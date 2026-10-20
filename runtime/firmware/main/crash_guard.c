/* Crash-loop guard; see crash_guard.h. */
#include "crash_guard.h"
#include "wall_log.h"
#include "crash_guard_rule.h"
#include <stdio.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define MAGIC 0x46574352u            /* "FWCR": the counter below is ours and intact */

static RTC_NOINIT_ATTR uint32_t s_magic, s_count;
static bool s_safe;

bool fw_safe_mode(void){ return s_safe; }

static void healthy(void *arg){
    (void)arg;
    s_count = 0;
    fw_state("{\"event\":\"crash_count_cleared\"}");
}

bool fw_crash_guard_boot(void){
    esp_reset_reason_t r = esp_reset_reason();
    bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    bool remembered = s_magic == MAGIC;
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = healthy, .name = "fw_healthy"};
    s_count = fw_crash_count_next(remembered, s_count, crashed);
    s_magic = MAGIC;
#if CONFIG_FW_SAFE_MODE_TEST
    s_safe = true;                    /* test builds only: see safe mode work */
#else
    s_safe = fw_crash_safe(s_count, CONFIG_FW_SAFE_MODE_CRASHES);
#endif
    fw_state("{\"crashes_in_a_row\":%u,\"safe_mode\":%s}", (unsigned)s_count, s_safe ? "true" : "false");
    /* No "healthy" timer in safe mode; any restart that is not a crash (a
     * power cycle, the desk installing a fix or switching) tries normally. */
    if (!s_safe && esp_timer_create(&a, &t) == ESP_OK)
        esp_timer_start_once(t, (uint64_t)CONFIG_FW_SAFE_MODE_HEALTHY_S * 1000000ull);
    return s_safe;
}
