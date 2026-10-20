"""Local HTTPS acceptance, no Internet/FlightWall access. Public test keys only."""
import argparse,contextlib,hashlib,json,socketserver,ssl,subprocess,sys,threading,time,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parent;FIXTURES=HERE/'fixtures'
PROBE=None;BASELINE=None
PAYLOAD=bytes(range(256))*256

@contextlib.contextmanager
def server(cert='server',tls13=False):
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version=context.maximum_version=ssl.TLSVersion.TLSv1_3 if tls13 else ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(FIXTURES/(cert+'.pem'),FIXTURES/'PUBLIC-TEST-server-key.pem')
    context.set_ciphers('ECDHE-ECDSA-AES128-GCM-SHA256');context.set_ecdh_curve('prime256v1')
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            try:
                self.request.settimeout(3)
                with context.wrap_socket(self.request,server_side=True) as stream:
                    request=b''
                    while b'\r\n\r\n' not in request and len(request)<8192:
                        chunk=stream.recv(512)
                        if not chunk:return
                        request+=chunk
                    path=request.split(b' ')[1]
                    if path==b'/stall':time.sleep(.6);return
                    if path==b'/chunked':
                        stream.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n')
                        for offset in range(0,len(PAYLOAD),4096):
                            block=PAYLOAD[offset:offset+4096]
                            stream.sendall(('%x\r\n'%len(block)).encode()+block+b'\r\n')
                        stream.sendall(b'0\r\n\r\n');return
                    header=b'HTTP/1.1 200 OK\r\n'
                    if path==b'/compressed':header+=b'Content-Encoding: gzip\r\n'
                    if path==b'/conflict':header+=b'Transfer-Encoding: chunked\r\n'
                    length=len(PAYLOAD)+(1 if path==b'/truncated' else 0)
                    if path==b'/oversized':length=131073
                    stream.sendall(header+('Content-Length: %d\r\nConnection: close\r\n\r\n'%length).encode()+PAYLOAD)
            except (OSError,ssl.SSLError,IndexError):pass # expected rejected handshakes/disconnects
    class LocalServer(socketserver.ThreadingTCPServer):
        allow_reuse_address=True;daemon_threads=True
    with LocalServer(('127.0.0.1',0),Handler) as instance:
        thread=threading.Thread(target=instance.serve_forever,kwargs={'poll_interval':.02},daemon=True);thread.start()
        try:yield instance.server_address[1]
        finally:instance.shutdown();thread.join(timeout=3)

def probe(port,*args,executable=None,expect=True):
    result=subprocess.run([str(executable or PROBE),'--host','127.0.0.1','--port',str(port),
        '--server-name','flightwall.test','--ca',str(FIXTURES/'root.pem'),'--path','/large',*map(str,args)],
        capture_output=True,text=True,timeout=30)
    if result.returncode not in (0,1):raise AssertionError('Probe failed outside checked TLS lifecycle: '+result.stderr)
    rows=[json.loads(line) for line in result.stdout.splitlines()]
    if not rows:raise AssertionError('Missing probe results: '+result.stderr)
    for row in rows:
        assert row['remaining_blocks']==row['remaining_payload_bytes']==0,row
        assert row['close_count']==1,row
        assert row['ok'] is expect,row
        if expect:
            assert row['body_bytes']==len(PAYLOAD) and row['body_sha256']==hashlib.sha256(PAYLOAD).hexdigest(),row
            assert row['verify_flags']==0,row
    return rows

class LocalHttpsTests(unittest.TestCase):
    def test_trusted_large_records_partial_io_and_reconnects(self):
        with server() as port:
            rows=probe(port,'--rounds',10,'--io-chunk',7,'--arena-bytes',49152)
            self.assertTrue(all(r['rx_bytes']==16384 and r['tx_bytes']==2048 for r in rows))
    def test_chunked(self):
        with server() as port:probe(port,'--path','/chunked')
    def test_wrong_hostname(self):
        with server() as port:
            row=probe(port,'--server-name','wrong.test',expect=False)[0]
            self.assertNotEqual(row['verify_flags'],0)
    def test_untrusted_root(self):
        with server() as port:
            row=probe(port,'--ca',FIXTURES/'untrusted-root.pem',expect=False)[0]
            self.assertNotEqual(row['verify_flags'],0)
    def test_expired_certificate(self):
        with server('expired') as port:
            row=probe(port,expect=False)[0];self.assertNotEqual(row['verify_flags'],0)
    def test_future_certificate(self):
        with server('future') as port:
            row=probe(port,expect=False)[0];self.assertNotEqual(row['verify_flags'],0)
    def test_version_failure(self):
        with server(tls13=True) as port:probe(port,expect=False)
    def test_truncated_http(self):
        with server() as port:probe(port,'--path','/truncated',expect=False)
    def test_unsupported_compression_and_oversized_framing(self):
        with server() as port:
            for path in ['/compressed','/oversized','/conflict']:probe(port,'--path',path,expect=False)
    def test_entropy_failure(self):
        with server() as port:
            row=probe(port,'--bad-rng',expect=False)[0];self.assertEqual(row['failure'],4)
    def test_bounded_oom(self):
        with server() as port:
            row=probe(port,'--arena-bytes',8192,expect=False)[0];self.assertNotEqual(row['failure'],0)
    def test_io_failure_and_deadline(self):
        with server() as port:
            probe(port,'--fail-after',2,expect=False)
            row=probe(port,'--path','/stall','--timeout-ms',200,expect=False)[0]
            self.assertEqual(row['failure'],8)
    def test_buffer_comparison(self):
        if not BASELINE:self.skipTest('16384-byte comparison build not supplied')
        with server() as port:
            small=probe(port)[0];large=probe(port,executable=BASELINE)[0]
            self.assertEqual(large['tx_bytes'],16384)
            self.assertGreaterEqual(large['peak_payload_bytes']-small['peak_payload_bytes'],12*1024)
            print(json.dumps({'comparison_payload_peak_saved_bytes':large['peak_payload_bytes']-small['peak_payload_bytes'],
                'small_peak_payload_bytes':small['peak_payload_bytes'],'large_peak_payload_bytes':large['peak_payload_bytes']}))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--probe',type=Path,required=True);parser.add_argument('--baseline',type=Path)
    args,rest=parser.parse_known_args();PROBE=args.probe.resolve();BASELINE=args.baseline.resolve() if args.baseline else None
    unittest.main(argv=[sys.argv[0],*rest])
