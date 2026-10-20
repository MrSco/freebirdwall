#pragma once
/* HUB75 panel driver bring-up with the configuration recovered from the vendor
 * firmware (see ../PANEL.md). Skeleton stage: a diagnostic test pattern only. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Allocates the DMA frame buffer, starts output, draws the test pattern and
 * starts a small animation task. Prints FWMEM lines before and after so the
 * driver's memory cost is measured. Returns false (and leaves the panels dark)
 * if the driver could not start; the rest of the firmware carries on. */
bool fw_display_start(void);

/* Bytes of stack the animation task has never touched, 0 if not running. */
uint32_t fw_display_task_stack_free(void);

/* Frames the animation task has drawn: proves the panel task keeps running
 * while Wi-Fi and TLS are busy. */
uint32_t fw_display_frames(void);

#ifdef __cplusplus
}
#endif
