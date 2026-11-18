/* Network watchdog; see net_watch.h and net_watch_rule.h. */
#include "net_watch.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/sockets.h"
#include "memory_probe.h"
#include "net_watch_rule.h"
#include "wall_log.h"

#define PING_ID 0x4657u                   /* "FW" */
#define PING_WAIT_MS 2000

static FwNetWatch s_watch;
static uint32_t s_ping_at, s_sent, s_answered;
static uint16_t s_seq;
static bool s_noting, s_started;
static uint32_t s_lowest, s_low_at;

static uint32_t now_ms(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

/* Joined with an address; *gw the router (network byte order). */
static bool joined(uint32_t *gw){
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_wifi_sta_get_ap_info(&ap) != ESP_OK || esp_netif_get_ip_info(sta, &ip) != ESP_OK) return false;
    *gw = ip.gw.addr;
    return ip.ip.addr && ip.gw.addr;
}

/* One ICMP echo to the router; true when it answered within PING_WAIT_MS.
 * Every failure (no socket, no memory to send, no answer) is a false. */
static bool ping_once(uint32_t gw, uint16_t seq){
    struct { struct icmp_echo_hdr h; uint8_t data[8]; } pkt;
    uint8_t rx[64];
    struct sockaddr_in to;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
    int64_t until;
    bool ok = false;
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) return false;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = gw;
    memset(&pkt, 0, sizeof pkt);
    ICMPH_TYPE_SET(&pkt.h, ICMP_ECHO);
    pkt.h.id = PING_ID;
    pkt.h.seqno = lwip_htons(seq);
    memcpy(pkt.data, "fwwatch!", sizeof pkt.data);
    pkt.h.chksum = inet_chksum(&pkt, sizeof pkt);
    until = esp_timer_get_time() + PING_WAIT_MS * 1000LL;
    if (sendto(s, &pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) == (int)sizeof pkt) {
        while (!ok && esp_timer_get_time() < until) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            const struct ip_hdr *iph = (const struct ip_hdr *)rx;
            const struct icmp_echo_hdr *e;
            int n = recvfrom(s, rx, sizeof rx, 0, (struct sockaddr *)&from, &fl);
            if (n < (int)sizeof(struct ip_hdr) || from.sin_addr.s_addr != gw) continue;   /* nothing yet, or another host */
            if (n < IPH_HL_BYTES(iph) + (int)sizeof *e) continue;
            e = (const struct icmp_echo_hdr *)(rx + IPH_HL_BYTES(iph));
            ok = e->type == ICMP_ER && e->id == PING_ID && e->seqno == lwip_htons(seq);
        }
    }
    close(s);
    return ok;
}

/* "heap_low" when free memory first drops under FW_NET_WATCH_NOTE_LOW,
 * "heap_ok" (with the lowest seen and how long) once it is back over
 * FW_NET_WATCH_NOTE_OK: what used the memory shows in the lines around them. */
static void note_heap(uint32_t now, uint32_t free_heap){
    char extra[64];
    if (!s_noting) {
        if (free_heap >= FW_NET_WATCH_NOTE_LOW) return;
        s_noting = true;
        s_lowest = free_heap;
        s_low_at = now;
        fw_mem_report("heap_low", NULL);
        return;
    }
    if (free_heap < s_lowest) s_lowest = free_heap;
    if (free_heap < FW_NET_WATCH_NOTE_OK) return;
    s_noting = false;
    snprintf(extra, sizeof extra, "\"lowest\":%u,\"for_ms\":%u", (unsigned)s_lowest, (unsigned)(now - s_low_at));
    fw_mem_report("heap_ok", extra);
}

void fw_net_watch_tick(void){
    FwMemSnapshot m;
    FwNetVerdict v;
    uint32_t now = now_ms(), gw = 0;
    bool on = joined(&gw);
    fw_mem_snapshot(&m);
    note_heap(now, m.free_internal);
    if (!s_started) {
        s_started = true;
        s_ping_at = now;                  /* first ping right away */
    }
    if (on && (int32_t)(now - s_ping_at) >= 0) {
        s_ping_at = now + FW_NET_WATCH_PING_MS;
        ++s_sent;
        if (ping_once(gw, ++s_seq)) {
            ++s_answered;
            fw_net_watch_reply(&s_watch, now_ms());
        }
        now = now_ms();
    }
    v = fw_net_watch_check(&s_watch, now, on, m.free_internal, m.largest_internal);
    if (v == FW_NET_OK) return;
    fw_mem_snapshot(&m);
    {   /* kept like a crash note, so the reason survives the lines after the restart */
        static char line[256];
        snprintf(line, sizeof line, "FWSTATE {\"event\":\"network_watchdog\",\"why\":\"%s\",\"silent_s\":%u,\"pings\":%u,"
                 "\"answered\":%u,\"free\":%u,\"min_free\":%u,\"largest\":%u,\"t_ms\":%u,\"note\":\"restarting; this log is kept\"}",
             v == FW_NET_HEAP_EXHAUSTED ? "heap" : "router_silent",
             s_watch.replied ? (unsigned)((now - s_watch.reply_at) / 1000) : 0u, (unsigned)s_sent,
             (unsigned)s_answered, (unsigned)m.free_internal, (unsigned)m.min_free_internal,
             (unsigned)m.largest_internal, (unsigned)now);
        fw_log_keep(line);
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}
