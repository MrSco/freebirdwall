#ifndef FLIGHTWALL_FW_QR_H
#define FLIGHTWALL_FW_QR_H
/* The vendor's QR generator (ricmoo/QRCode) behind a small API: its header
 * defines its own C "bool", so only fw_qr.c includes it. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FW_QR_MAX_SIZE 29   /* version 3 */

/* Encodes text as a version-3, low-correction symbol, as the stock setup
 * screen does. modules[y * size + x] is 1 for a dark module. Returns size. */
int fw_qr_v3_low(const char *text, uint8_t modules[FW_QR_MAX_SIZE * FW_QR_MAX_SIZE]);

#ifdef __cplusplus
}
#endif
#endif
