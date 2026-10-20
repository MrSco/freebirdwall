/* The crash-loop guard's counting rule (main/crash_guard_rule.h). */
#include <stdio.h>
#include "crash_guard_rule.h"

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void){
    uint32_t n = 0;
    /* a crash loop: three crashes in a row reach safe mode */
    n = fw_crash_count_next(false, 12345, true);   CHECK(n == 0);   /* counter not ours: start at 0 */
    n = fw_crash_count_next(true, n, true);        CHECK(n == 1 && !fw_crash_safe(n, 3));
    n = fw_crash_count_next(true, n, true);        CHECK(n == 2 && !fw_crash_safe(n, 3));
    n = fw_crash_count_next(true, n, true);        CHECK(n == 3 && fw_crash_safe(n, 3));
    n = fw_crash_count_next(true, n, true);        CHECK(n == 4 && fw_crash_safe(n, 3));
    /* anything but a crash starts again: planned restart, power cycle */
    CHECK(fw_crash_count_next(true, 4, false) == 0);
    CHECK(fw_crash_count_next(false, 4, true) == 0);
    /* crashes spread out never add up when a reset in between was planned */
    n = fw_crash_count_next(true, 0, true);
    n = fw_crash_count_next(true, n, false);
    n = fw_crash_count_next(true, n, true);        CHECK(n == 1);
    /* bounded; 0 disables */
    CHECK(fw_crash_count_next(true, 1000, true) == 1000);
    CHECK(!fw_crash_safe(500, 0));
    CHECK(fw_crash_safe(1, 1));
    printf("%s: %d guard checks\n", failed ? "FAIL" : "PASS", checks);
    return failed != 0;
}
