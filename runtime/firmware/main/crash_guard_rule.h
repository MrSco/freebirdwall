#ifndef FLIGHTWALL_CRASH_GUARD_RULE_H
#define FLIGHTWALL_CRASH_GUARD_RULE_H
/* The crash-loop guard's counting rule (crash_guard.c), kept free of SDK
 * types so tests/test_guard.c can check it on the desktop. */
#include <stdbool.h>
#include <stdint.h>

/* Consecutive crashes after this boot. remembered: the counter survived the
 * reset (RTC memory with its magic intact; a power cut clears it). crashed:
 * this reset was a panic or a watchdog. Anything else (power on, a planned
 * restart, brownout, deep sleep) starts again from 0. */
static inline uint32_t fw_crash_count_next(bool remembered, uint32_t previous, bool crashed){
    if (!remembered || !crashed) return 0;
    return previous < 1000 ? previous + 1 : previous;
}

/* Safe mode once the count reaches the limit (0 = never). */
static inline bool fw_crash_safe(uint32_t count, uint32_t limit){ return limit && count >= limit; }

#endif
