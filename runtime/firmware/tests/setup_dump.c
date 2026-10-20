/* Renders the setup screens for one identity and prints what tests/test_setup.py
 * checks: the setup strings, then the 128x64 frame of each page as rows of
 * '#' (lit) and '.' (dark).
 *   setup_dump <id> <fp> */
#include <stdio.h>
#include <string.h>
#include "fw_screens.h"

#define W 128
#define H 64
static char fb[H][W];

static void put(void *ctx, int x, int y, FwRgb c){
    (void)ctx;
    fb[y][x] = (c.r || c.g || c.b) ? '#' : '.';
}

int main(int argc, char **argv){
    const FwSurface s = {NULL, W, H, put, NULL};
    FwSetupInfo info;
    int page, y;
    if (argc < 3) return 2;
    printf("ok %d\n", fw_setup_info(argv[1], argv[2], &info));
    printf("ssid %s\npassword %s\nqr %s\njoin %s\n", info.ssid, info.password, info.qr, info.join_qr);
    /* 0-2 stock pages, 3 the reconnect variant of page 0, 4 the join QR */
    for (page = 0; page < 5; ++page) {
        memset(fb, '?', sizeof fb);
        fw_screen_setup(&s, NULL, page < 3 ? page : page == 3 ? 0 : FW_SETUP_PAGE_JOIN_QR, &info, page == 3);
        printf("page %d\n", page);
        for (y = 0; y < H; ++y) printf("%.*s\n", W, fb[y]);
    }
    return 0;
}
