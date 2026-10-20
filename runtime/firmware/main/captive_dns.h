#ifndef FLIGHTWALL_CAPTIVE_DNS_H
#define FLIGHTWALL_CAPTIVE_DNS_H
/* Captive-portal DNS answer, in place: every A query gets the given IPv4
 * address (TTL 60), other types an empty NOERROR answer. Pure C so the
 * desktop test (tests/test_captive_dns.py) runs the same code.
 * Returns the reply length, or 0 to drop the packet. */
#include <stdint.h>
#include <string.h>

static inline int fw_dns_answer(uint8_t *buf, int n, int cap, const uint8_t ip[4]){
    static const uint8_t ans[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
    int q = 12, out;
    uint16_t qtype;
    if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) return 0;   /* one question, queries only */
    while (q < n && buf[q]) {
        if (buf[q] & 0xc0) return 0;                                         /* no compression in questions */
        q += buf[q] + 1;
    }
    if (q + 5 > n) return 0;
    qtype = (uint16_t)(buf[q + 1] << 8 | buf[q + 2]);
    out = q + 5;                                                             /* end of the question */
    buf[2] = (uint8_t)(0x80 | (buf[2] & 0x01));                              /* response, keep RD */
    buf[3] = 0x80;                                                           /* RA, NOERROR */
    memset(buf + 6, 0, 6);                                                   /* no AN/NS/AR yet */
    if (qtype == 1 && out + (int)sizeof ans + 4 <= cap) {
        buf[7] = 1;
        memcpy(buf + out, ans, sizeof ans);
        memcpy(buf + out + sizeof ans, ip, 4);
        out += (int)sizeof ans + 4;
    }
    return out;
}
#endif
