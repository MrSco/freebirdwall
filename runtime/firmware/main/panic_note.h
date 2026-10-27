#ifndef FLIGHTWALL_PANIC_NOTE_H
#define FLIGHTWALL_PANIC_NOTE_H
/* A crash writes one FWPANIC line (reason, task, abort details such as a
 * stack overflow, up to eight return addresses) into the wall's log ring as
 * it happens; see panic_note.c. The link wraps esp-idf's panic handler
 * (main/CMakeLists.txt). After the task watchdog the line also says which
 * CPU's idle task was starved and what each CPU was running. */

/* Idle hooks on both CPUs, for that watchdog report. Once, early. */
void fw_panic_note_init(void);
#endif
