#ifndef FLIGHTWALL_CRASH_GUARD_H
#define FLIGHTWALL_CRASH_GUARD_H
/* Crash-loop guard (UPDATES.md "Safe mode"). A pending test image that
 * crashes is rolled back by the bootloader, but a kept image is not: crashing
 * at every start it would never reach the network again. So consecutive
 * crashes are counted in RTC memory, and after CONFIG_FW_SAFE_MODE_CRASHES
 * of them the firmware starts in safe mode: Wi-Fi and the update service
 * only (no panel, feed, dashboard, games or stats), so the desk can still
 * upload a fix or switch back to stock. Running CONFIG_FW_SAFE_MODE_HEALTHY_S
 * seconds without a crash clears the count; any reset that is not a crash
 * (power cycle, planned restart) starts the count again. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Call first thing at start. Returns true for safe mode. */
bool fw_crash_guard_boot(void);

/* Safe mode this start (also reported by GET /v1/firmware). */
bool fw_safe_mode(void);

#ifdef __cplusplus
}
#endif
#endif
