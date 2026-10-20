"""Isolated firmware update client/HTTP boundary tests; no device or saved keys."""
import hashlib,json,struct,tempfile,threading,http.client,unittest
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from unittest.mock import patch
import firmware_device as fw
import control,local_device
TOKEN='01'+'00'*31
CONNECTION={'ip':'192.168.50.20','token':TOKEN}
REAL_HTTP=http.client.HTTPConnection

def image():
    raw=bytearray(640);raw[0]=0xe9;raw[1]=1;raw[12]=9;raw[23]=1
    struct.pack_into('<II',raw,24,0x3c000020,560);struct.pack_into('<I',raw,32,0xabcd5432)
    raw[400:444]=fw.DESCRIPTOR+struct.pack('<I',1)+bytes.fromhex(TOKEN)
    checksum=0xef
    for b in raw[32:592]:checksum^=b
    raw[607]=checksum;raw[608:]=hashlib.sha256(raw[:608]).digest()
    return bytes(raw)

class Mock(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def send(self,code,obj):
        data=json.dumps(obj).encode();self.send_response(code);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def authenticated(self):return self.headers.get('Authorization')=='Bearer '+TOKEN
    def do_GET(self):
        if self.path=='/v1/log':
            if not self.authenticated():return self.send(401,{})
            if not self.server.has_log:return self.send(404,{})
            data=b'--- boot 1, reset reason 1 ---\nFWSTATE {"crashes_in_a_row":0}\n';self.send_response(200)
            self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
        self.server.reads+=1
        if self.server.error:return self.send(self.server.error,{'error':TOKEN})
        if not self.authenticated():return self.send(401,{})
        return self.send(200,{'api_version':1,'schema_version':1,'ota_supported':True,
            'running_sha256':self.server.sha,'running_slot':self.server.slot,'boot_slot':self.server.slot,
            'partition_size':0x2c0000,'rollback_supported':True,'update_busy':False,'pending_confirmation':self.server.pending})
    def do_PUT(self):
        self.server.puts+=1;raw=self.rfile.read(int(self.headers['Content-Length']))
        if self.headers.get('If-Match')!='"'+self.server.sha+'"':return self.send(409,{})
        digest=hashlib.sha256(raw).hexdigest();assert digest==self.headers['X-Firmware-SHA256']
        self.server.sha=digest;self.server.slot='app0';self.server.pending=True
        return self.send(200,{'api_version':1,'schema_version':1,'accepted':True,'sha256':digest,
            'bytes':len(raw),'target_slot':'app0','rebooting':not self.server.bad_receipt})
    def do_POST(self):
        assert self.headers['If-Match']=='"'+self.server.sha+'"'
        if self.path=='/v1/firmware/switch':
            self.server.switches+=1
            if not self.server.switchable:return self.send(404,{})
            other=self.server.other;self.server.other=self.server.sha
            target='app0' if self.server.slot=='app1' else 'app1'
            self.server.sha=other;self.server.slot=target;self.server.pending=True
            return self.send(200,{'api_version':1,'schema_version':1,'switching':True,'target_slot':target,
                'target_sha256':other,'rebooting':not self.server.bad_receipt})
        self.server.confirms+=1;assert self.path=='/v1/firmware/confirm'
        if self.server.confirm_error:return self.send(503,{})
        self.server.pending=False;return self.send(200,{'api_version':1,'schema_version':1})

class FirmwareTests(unittest.TestCase):
    def setUp(self):
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Mock)
        for key,value in dict(sha='1'*64,slot='app1',pending=False,reads=0,puts=0,confirms=0,error=0,confirm_error=False,bad_receipt=False,other='2'*64,switches=0,switchable=True,has_log=True).items():setattr(self.server,key,value)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        def connection(host,port,timeout):
            self.assertEqual((host,port),(CONNECTION['ip'],8787))
            return REAL_HTTP('127.0.0.1',self.server.server_port,timeout=timeout)
        self.patcher=patch.object(fw.http.client,'HTTPConnection',side_effect=connection);self.patcher.start()
        self.client=fw.Client(CONNECTION)
    def tearDown(self):
        self.patcher.stop();self.server.shutdown();self.server.server_close();self.thread.join()
    def test_log_over_wifi(self):
        self.assertIn('crashes_in_a_row', self.client.log())
        self.server.has_log=False
        with self.assertRaisesRegex(fw.DeviceError,'no Wi-Fi log'):self.client.log()
    def test_success_requires_reboot_and_confirmation(self):
        result=self.client.update(image(),'1'*64)
        self.assertTrue(result['device_confirmed']);self.assertTrue(result['reboot_confirmed'])
        self.assertFalse(result['firmware']['pending_confirmation'])
        self.assertEqual((self.server.puts,self.server.confirms),(1,1))
    def test_switch_boots_other_slot_and_confirms_it(self):
        result=self.client.switch_slot('1'*64)
        self.assertTrue(result['switched']);self.assertTrue(result['device_confirmed'])
        self.assertEqual((result['target_slot'],result['sha256']),('app0','2'*64))
        self.assertEqual((self.server.switches,self.server.confirms,self.server.pending),(1,1,False))
    def test_switch_refused_while_unconfirmed_or_stale(self):
        self.server.pending=True
        with self.assertRaises(fw.DeviceError):self.client.switch_slot('1'*64)
        self.server.pending=False
        with self.assertRaises(fw.DeviceError):self.client.switch_slot('3'*64)
        self.assertEqual(self.server.switches,0)
    def test_switch_bad_receipt_never_confirms(self):
        self.server.bad_receipt=True
        with self.assertRaises(fw.DeviceError):self.client.switch_slot('1'*64)
        self.assertEqual((self.server.switches,self.server.confirms),(1,0))
    def test_switch_on_firmware_without_it_is_explained(self):
        self.server.switchable=False
        with self.assertRaises(fw.DeviceError) as e:self.client.switch_slot('1'*64)
        self.assertIn('owned firmware only',str(e.exception))
    def test_stale_never_uploads(self):
        with self.assertRaises(local_device.DeviceError) as e:self.client.update(image(),'2'*64)
        self.assertEqual(e.exception.status,409);self.assertEqual(self.server.puts,0)
    def test_invalid_binary_key_and_hash_rejected_before_network(self):
        raw=image()
        for invalid in (b'\xff'*640,raw+b'\0',raw[:-1],raw[:100]+b'x'+raw[101:]):
            with self.subTest(length=len(invalid)),self.assertRaises(local_device.DeviceError):self.client.update(invalid,'1'*64)
        with self.assertRaises(local_device.DeviceError):fw.validate_image(raw,'02'+'00'*31)
        self.assertEqual((self.server.reads,self.server.puts),(0,0))
    def test_valid_stock_shaped_image_without_owner_marker_never_uploads(self):
        raw=bytearray(image());raw[400:444]=bytes(44)
        checksum=0xef
        for value in raw[32:592]:checksum^=value
        raw[607]=checksum;raw[608:]=hashlib.sha256(raw[:608]).digest()
        with self.assertRaises(local_device.DeviceError):self.client.update(bytes(raw),'1'*64)
        self.assertEqual((self.server.reads,self.server.puts),(0,0))
    def test_bad_receipt_never_confirms_or_retries(self):
        self.server.bad_receipt=True
        with self.assertRaises(local_device.DeviceError):self.client.update(image(),'1'*64)
        self.assertEqual((self.server.puts,self.server.confirms),(1,0))
    def test_rejected_or_redirected_status_is_sanitized(self):
        for code in (401,403,404,302,503):
            self.server.error=code
            with self.assertRaises(local_device.DeviceError) as e:self.client.status()
            self.assertNotIn(TOKEN,str(e.exception))
        self.assertEqual(self.server.puts,0)
    def test_confirmation_failure_never_claims_success(self):
        self.server.confirm_error=True
        with patch.object(fw,'POLL_SECONDS',.02):result=self.client.update(image(),'1'*64)
        self.assertTrue(result['accepted']);self.assertFalse(result['device_confirmed']);self.assertEqual(self.server.puts,1)
    def test_pending_image_blocks_another_upload(self):
        self.server.pending=True
        with self.assertRaises(local_device.DeviceError):self.client.update(image(),'1'*64)
        self.assertEqual(self.server.puts,0)
    def test_localhost_upload_origin_type_and_confirmed_result(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'device.json';local_device.save_connection(path,CONNECTION)
            desk=ThreadingHTTPServer(('127.0.0.1',0),control.Handler)
            worker=threading.Thread(target=desk.serve_forever,daemon=True);worker.start()
            try:
                with patch.object(control,'DEVICE_PATH',path),patch.object(control,'_request',side_effect=AssertionError('cloud forbidden')):
                    for headers,expected in [({'Content-Type':'application/json'},415),({'Content-Type':'application/octet-stream','Origin':'http://evil.invalid'},403),({'Content-Type':'application/octet-stream','If-Match':'"'+'1'*64+'"'},200)]:
                        c=REAL_HTTP('127.0.0.1',desk.server_port,timeout=5)
                        c.request('POST','/api/device/firmware',image(),headers);r=c.getresponse();data=r.read();c.close()
                        self.assertEqual(r.status,expected)
                        if expected==200:self.assertTrue(json.loads(data)['device_confirmed'])
                        self.assertNotIn(TOKEN.encode(),data)
            finally:desk.shutdown();desk.server_close();worker.join()

if __name__=='__main__':unittest.main()
