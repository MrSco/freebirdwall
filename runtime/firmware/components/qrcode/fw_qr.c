#include "fw_qr.h"
#include "qrcode.h"

int fw_qr_v3_low(const char *text, uint8_t modules[FW_QR_MAX_SIZE * FW_QR_MAX_SIZE]){
    static uint8_t buffer[106];                   /* qrcode_getBufferSize(3) */
    QRCode qr;
    int x, y;
    qrcode_initText(&qr, buffer, 3, ECC_LOW, text);   /* stock ignores the result too */
    for (y = 0; y < qr.size; ++y)
        for (x = 0; x < qr.size; ++x)
            modules[y * qr.size + x] = qrcode_getModule(&qr, (uint8_t)x, (uint8_t)y) ? 1 : 0;
    return qr.size;
}
