"""Check the captive-portal DNS answer (main/captive_dns.h) with dnspython.

    py runtime/firmware/tests/test_captive_dns.py

Builds real queries (the names phones use for connectivity checks, plus AAAA
and HTTPS-record queries), runs them through the firmware's C function and
parses the replies: every A query must come back with 192.168.4.1, the same
id and question, recursion flags, TTL 60; other types an empty NOERROR; and
responses, multi-question packets and truncated packets are dropped.

Needs dnspython: py -m pip install --target runtime/build/deps/py-dns dnspython==2.6.1
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
sys.path.insert(0, str(FW.parent / 'build' / 'deps' / 'py-dns'))
import dns.flags  # noqa: E402
import dns.message  # noqa: E402
import dns.rdatatype  # noqa: E402

DRIVER = r'''
#include <stdio.h>
#include <string.h>
#include "captive_dns.h"
int main(void){
    char line[2048];
    static const unsigned char ip[4] = {192, 168, 4, 1};
    while (fgets(line, sizeof line, stdin)) {
        unsigned char buf[512];
        int n = 0, out, i;
        for (char *p = line; p[0] && p[1] && p[0] != '\n' && n < (int)sizeof buf; p += 2) {
            unsigned v;
            sscanf(p, "%2x", &v);
            buf[n++] = (unsigned char)v;
        }
        out = fw_dns_answer(buf, n, sizeof buf, ip);
        for (i = 0; i < out; ++i) printf("%02x", buf[i]);
        printf("\n");
    }
    return 0;
}
'''
checks = 0


def check(cond, what):
    global checks
    if not cond:
        sys.exit('FAIL: ' + what)
    checks += 1


def main():
    gcc = shutil.which('gcc')
    if not gcc:
        sys.exit('a desktop gcc is required')
    names = ['connectivitycheck.gstatic.com', 'captive.apple.com', 'www.msftconnecttest.com',
             'clients3.google.com', 'example.org', 'a.very.long.name.' + 'x' * 50 + '.test']
    cases = []
    for name in names:
        for rdtype in ('A', 'AAAA', 'HTTPS'):
            cases.append(('ok', dns.message.make_query(name, rdtype)))
    bad = []
    resp = dns.message.make_response(dns.message.make_query('x.test', 'A'))
    bad.append(resp.to_wire())                                       # a response, not a query
    two = bytearray(dns.message.make_query('x.test', 'A').to_wire())
    two[5] = 2                                                       # QDCOUNT 2
    bad.append(bytes(two))
    bad.append(dns.message.make_query('x.test', 'A').to_wire()[:15])  # truncated
    bad.append(b'\x00' * 5)
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'drv.c'
        src.write_text(DRIVER)
        exe = Path(tmp) / 'drv.exe'
        r = subprocess.run([gcc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I%s' % (FW / 'main'),
                            str(src), '-o', str(exe)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        lines = [m.to_wire().hex() for _, m in cases] + [b.hex() for b in bad]
        out = subprocess.run([str(exe)], input='\n'.join(lines) + '\n', capture_output=True, text=True,
                             check=True).stdout.split('\n')
    for (kind, q), hexreply in zip(cases, out):
        reply = dns.message.from_wire(bytes.fromhex(hexreply))
        check(reply.id == q.id, 'id kept')
        check(reply.flags & dns.flags.QR and reply.flags & dns.flags.RD and reply.flags & dns.flags.RA, 'flags')
        check(reply.rcode() == 0, 'NOERROR')
        check(reply.question == q.question, 'question echoed')
        if q.question[0].rdtype == dns.rdatatype.A:
            check(len(reply.answer) == 1, 'one answer')
            rr = reply.answer[0]
            check(rr.name == q.question[0].name and rr.ttl == 60, 'answer name and TTL')
            check([r.address for r in rr] == ['192.168.4.1'], 'answer is the access point')
        else:
            check(len(reply.answer) == 0, 'no answer for %s' % dns.rdatatype.to_text(q.question[0].rdtype))
    for hexreply in out[len(cases):len(cases) + len(bad)]:
        check(hexreply == '', 'bad packet dropped')
    print('PASS: %d captive DNS checks' % checks)


if __name__ == '__main__':
    main()
