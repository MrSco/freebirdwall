#ifndef FLIGHTWALL_BIG_BUFFER_H
#define FLIGHTWALL_BIG_BUFFER_H
/* One block of CONFIG_FW_TLS_ARENA_BYTES (48 KiB) reserved at link time and
 * lent to one user at a time: the TLS engine for a request (community feed,
 * route probe), or the logo decoder (~44 KiB) for a decode. Reserving it once
 * keeps both away from the heap, where a 44-48 KiB contiguous block is not
 * always free once Wi-Fi and the servers are running. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

extern const size_t fw_big_bytes;

/* Call once at boot, before anything takes it. */
void fw_big_init(void);

/* The block, or NULL if another user still has it after wait_ms. */
uint8_t *fw_big_take(uint32_t wait_ms);
void fw_big_give(void);

/* True while lent out (a hint: it may change at once). */
bool fw_big_busy(void);

/* The screen sets this while a logo waits for the block; the feed then holds
 * off starting a request for a moment, so logos go first. */
void fw_big_want(bool wanted);
bool fw_big_wanted(void);

#ifdef __cplusplus
}
#endif
#endif
