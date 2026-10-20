#ifndef FLIGHTWALL_BOARD_CHECK_H
#define FLIGHTWALL_BOARD_CHECK_H
/* Is this the FlightWall board the panel pins were recovered for (PANEL.md)?
 * Checked at every start before any panel pin is touched: driving GPIO 33-37
 * as panel outputs on a module whose octal flash or PSRAM uses them, or the
 * pin map on a different controller board, could damage it. On a mismatch
 * the panel is never started and the firmware runs as in safe mode (Wi-Fi
 * and the update service), reporting why (UPDATES.md "Board check").
 *
 * Needs NVS initialised (for the vendor's model name); reads only. */
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    char why[48];          /* first failed check, "" when ok */
    int flash_octal;       /* eFuse FLASH_TYPE: 0 quad, 1 octal */
    int psram_cap;         /* eFuse PSRAM_CAP: 0 none, 1 8 MB (octal), 2 2 MB (quad) */
    int flash_mb;
    bool partitions;       /* the FlightWall partition table, exactly */
    char model[16];        /* vendor settings' display_config.model, "" if none */
} FwBoard;

bool fw_board_check(FwBoard *out);
const FwBoard *fw_board(void);   /* the last result (for the status reply) */

#ifdef __cplusplus
}
#endif
#endif
