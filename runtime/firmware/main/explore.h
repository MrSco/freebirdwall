#ifndef FLIGHTWALL_EXPLORE_H
#define FLIGHTWALL_EXPLORE_H
/* On-demand "Explore anywhere in the world" airspace snapshot via adsb.fi
 * (runs on the feed task using the shared TLS block; zero idle RAM). */
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Queues a live lookup around (lat, lon) with radius dist_nm (5..150 NM).
 * Returns false if rate-limited or invalid coordinates. */
bool fw_explore_request(double lat, double lon, int dist_nm);

/* Runs one pending lookup (or frees expired cached results). Feed task only. */
void fw_explore_step(void);

/* Copies the current JSON status/result into buf (like snprintf). */
size_t fw_explore_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
