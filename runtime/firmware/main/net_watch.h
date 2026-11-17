#ifndef FLIGHTWALL_NET_WATCH_H
#define FLIGHTWALL_NET_WATCH_H
/* Network watchdog (net_watch_rule.h has the rule and why). Pings the router
 * every 15 s, notes low memory in the log ("heap_low" / "heap_ok" lines) and
 * restarts the wall, log kept, when it has gone deaf on a joined network. */
#ifdef __cplusplus
extern "C" {
#endif

/* About once a second, from a task with nothing else to do (app_main's last
 * loop): it may block up to 2 s on a ping. */
void fw_net_watch_tick(void);

#ifdef __cplusplus
}
#endif
#endif
