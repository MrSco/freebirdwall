#ifndef FLIGHTWALL_WALL_LOG_H
#define FLIGHTWALL_WALL_LOG_H
/* The wall's own status lines (FWSTATE, FWMEM) kept in 7 KiB of RTC memory
 * as well as printed, so the desk can read them over Wi-Fi without a USB
 * cable (GET /v1/log on the update service, UPDATES.md). RTC memory survives
 * a crash restart, so the lines before a crash are still there; a power cut
 * clears it. Status lines carry no secrets (FEED.md, SETTINGS.md rules). */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Call first thing at start: keeps what survived, marks a new boot. */
void fw_log_init(void);

/* "FWSTATE " + the formatted JSON object, printed and kept. */
void fw_state(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A complete line (without newline), printed and kept. */
void fw_log_line(const char *line);

/* Copies kept bytes from offset `from` (0 = oldest) into out; returns the
 * count, 0 at the end. The last crash's FWPANIC line comes first, under
 * "--- last crash, in boot N ---" (the ring's own "--- boot N" marks count the
 * same boots), until the next crash replaces it. */
size_t fw_log_read(size_t from, char *out, size_t cap);

/* For memory_probe.c: the shared format buffer; give() logs and releases it. */
char *fw_log_buffer_take(size_t *cap);
void fw_log_buffer_give(void);

/* One line written from the panic handler (panic_note.c): no lock, IRAM.
 * Also kept as the last crash. */
void fw_log_panic(const char *line);

#ifdef __cplusplus
}
#endif
#endif
