/* A line about a crash, written into the wall's log ring (RTC memory) at the
 * moment it happens: with no USB console this is what says where a crash
 * was, and the ring survives the restart, also a rollback to the other
 * firmware (same ring layout). esp-idf's panic handler and panic_abort are
 * wrapped (-Wl,--wrap, main/CMakeLists.txt); the wrappers format by hand
 * (no printf, no locks, IRAM only), then run the originals.
 *   FWPANIC {"reason":"...","task":"...","details":"...","exccause":n,
 *            "excvaddr":"0x...","pc":"0x... 0x... (up to 8)"} */
#include "panic_note.h"
#include <stdint.h>
#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "soc/cpu.h"
#include "esp_private/panic_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/xtensa_context.h"
#include "wall_log.h"

#define DEPTH 8

static DRAM_ATTR char s_details[80];      /* panic_abort's text, e.g. a stack overflow */
static DRAM_ATTR char s_line[400];

void __real_esp_panic_handler(panic_info_t *info);
void __real_panic_abort(const char *details) __attribute__((noreturn));

typedef struct { char *p, *end; } Out;

static void IRAM_ATTR text(Out *o, const char *s, size_t max){
    for (size_t i = 0; s && s[i] && i < max && o->p < o->end; ++i) {
        char c = s[i];
        *o->p++ = c == '"' || c == '\\' || (unsigned char)c < 0x20 ? ' ' : c;
    }
}
static void IRAM_ATTR lit(Out *o, const char *s){ while (*s && o->p < o->end) *o->p++ = *s++; }
static void IRAM_ATTR hex(Out *o, uint32_t v){
    static const char DRAM_ATTR digits[] = "0123456789abcdef";
    lit(o, "0x");
    for (int i = 7; i >= 0; --i) if (o->p < o->end) *o->p++ = digits[(v >> (i * 4)) & 15];
}
static void IRAM_ATTR dec(Out *o, uint32_t v){
    char b[10];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n && o->p < o->end) *o->p++ = b[--n];
}

void IRAM_ATTR __wrap_panic_abort(const char *details){
    size_t i = 0;
    if (details) for (; i + 1 < sizeof s_details && details[i]; ++i) s_details[i] = details[i];
    s_details[i] = 0;
    __real_panic_abort(details);
}

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info){
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCPU(info->core);
    Out o = {s_line, s_line + sizeof s_line - 2};
    lit(&o, "FWPANIC {\"reason\":\"");
    text(&o, info->reason, 40);
    lit(&o, "\",\"task\":\"");
    text(&o, t ? pcTaskGetName(t) : "", 16);
    lit(&o, "\",\"details\":\"");
    text(&o, s_details, sizeof s_details);
    lit(&o, "\"");
    if (f) {
        esp_backtrace_frame_t b = {.pc = (uint32_t)f->pc, .sp = (uint32_t)f->a1, .next_pc = (uint32_t)f->a0};
        lit(&o, ",\"exccause\":");
        dec(&o, (uint32_t)f->exccause);
        lit(&o, ",\"excvaddr\":\"");
        hex(&o, (uint32_t)f->excvaddr);
        lit(&o, "\",\"pc\":\"");
        for (int i = 0; i < DEPTH; ++i) {
            if (i) lit(&o, " ");
            hex(&o, esp_cpu_process_stack_pc(b.pc));
            if (!b.next_pc || !esp_backtrace_get_next_frame(&b)) break;
        }
        lit(&o, "\"");
    }
    lit(&o, "}");
    *o.p = 0;
    fw_log_panic(s_line);
    __real_esp_panic_handler(info);
}
