/* For the dashboard stand-in: runs the board's Sync schedule validator.
 *   sync_dump <edit.json>  ->  {"ok":true,"doc":<fw_sched_json>} or {"ok":false,"why":"..."} */
#include <stdio.h>
#include <stdlib.h>
#include "fw_schedule.h"

int main(int argc, char **argv){
    static char in[16384], out[8192], why[96];
    static FwSchedule s;
    FILE *f;
    size_t n;
    if (argc != 2 || !(f = fopen(argv[1], "rb"))) return 2;
    n = fread(in, 1, sizeof in - 1, f);
    fclose(f);
    in[n] = 0;
    if (!fw_sched_parse(in, &s, why, sizeof why)) { printf("{\"ok\":false,\"why\":\"%s\"}\n", why); return 0; }
    fw_sched_json(&s, out, sizeof out);
    printf("{\"ok\":true,\"doc\":%s}\n", out);
    return 0;
}
