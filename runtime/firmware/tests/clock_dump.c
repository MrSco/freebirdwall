/* Reads "utc_seconds offset_minutes dst_rule" lines and prints the local time
 * fw_clock computes for each: "year mon mday hour min sec wday dst".
 * Used by tests/test_clock.py to compare against the tz database. */
#include <stdio.h>
#include "fw_clock.h"

int main(void){
    long long utc;
    int offset, rule;
    while (scanf("%lld %d %d", &utc, &offset, &rule) == 3) {
        FwZone z = {(int16_t)offset, (uint8_t)rule};
        FwTime t;
        if (!fw_local_time(utc, z, &t)) { puts("fail"); continue; }
        printf("%d %d %d %d %d %d %d %d\n", t.year + 1900, t.mon + 1, t.mday, t.hour, t.min, t.sec, t.wday, t.dst);
    }
    return 0;
}
