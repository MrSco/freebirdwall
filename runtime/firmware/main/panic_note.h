#ifndef FLIGHTWALL_PANIC_NOTE_H
#define FLIGHTWALL_PANIC_NOTE_H
/* A crash writes one FWPANIC line (reason, task, abort details such as a
 * stack overflow, up to eight return addresses) into the wall's log ring as
 * it happens; see panic_note.c. Nothing to call: the link wraps esp-idf's
 * panic handler (main/CMakeLists.txt). */
#endif
