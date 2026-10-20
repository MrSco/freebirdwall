#ifndef FLIGHTWALL_FEED_H
#define FLIGHTWALL_FEED_H
/* The live flight feed on the board (FEED.md): stock's vendor feed, read
 * with stock's own API key (read-only) and the owned settings
 * (settings_store.c), polled every 10 s over HTTP. The parsing is
 * components/fw_feed. */
#include <stdbool.h>
#include <stdint.h>
#include "fw_feed.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FW_FEED_OFF,          /* not started, or no API key on the board: demo screens */
    FW_FEED_NO_CONFIG,    /* no stored configuration: stock's "Configure settings" */
    FW_FEED_WAITING,      /* configured, no good response yet */
    FW_FEED_LIVE,         /* at least one good response */
} FwFeedState;

/* Reads the key and configuration and starts the worker (call once Wi-Fi is up). */
void fw_feed_start(void);

FwFeedState fw_feed_state(void);

/* A copy of the display settings from the owned settings (valid from
 * WAITING on); returns their serial, which changes when they change. */
uint32_t fw_feed_display_copy(FwFeedDisplay *out);
uint32_t fw_feed_display_serial(void);

/* The owned settings changed: rebuild the request and poll at once. */
void fw_feed_settings_changed(void);

/* Changes whenever a new good response arrives. */
uint32_t fw_feed_serial(void);

/* Copies the latest good response; returns its serial. */
uint32_t fw_feed_copy(FwFeedFrame *out);

#ifdef __cplusplus
}
#endif
#endif
