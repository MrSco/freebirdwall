"""Explicit developer-only public-provider benchmark; no FlightWall access.

Print counts/memory/hash only. Live requests are opt-in and never run in CTest.
"""
import argparse,ast,json,subprocess,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def root_bytes(name):
    text=(ROOT/'shared'/name).read_text()
    return ''.join(ast.literal_eval(line.strip()) for line in text.splitlines() if line.startswith('"')).encode()
def compare(small,large):
    result={}
    services=[('routes','api.adsbdb.com','/v0/callsign/DAL42','owner_route_root.h'),
              ('positions','opendata.adsb.fi','/api/v3/lat/26.07/lon/-80.14/dist/25','owner_tls_root.h')]
    with tempfile.TemporaryDirectory(prefix='flightwall-public-tls-') as tmp:
        for label,host,path,certificate in services:
            ca=Path(tmp)/certificate;ca.write_bytes(root_bytes(certificate));samples={}
            for profile,exe in [('small',small),('full',large)]:
                proc=subprocess.run([str(exe),'--host',host,'--path',path,'--ca',str(ca),
                    '--arena-bytes','65536','--timeout-ms','15000'],capture_output=True,text=True,timeout=25)
                if proc.returncode:raise RuntimeError('Provider probe failed; no device changes were made: '+proc.stdout)
                rows=[json.loads(line) for line in proc.stdout.splitlines()];assert len(rows)==1
                sample=rows[0];assert sample['ok'] and sample['status']==200 and sample['verify_flags']==0
                assert sample['remaining_blocks']==0 and sample['remaining_payload_bytes']==0
                samples[profile]=sample
                # Respect the project's community polling minimum for benchmark requests too.
                time.sleep(5)
            samples['payload_peak_saved_bytes']=samples['full']['peak_payload_bytes']-samples['small']['peak_payload_bytes']
            result[label]=samples
    return result
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--live',action='store_true',required=True)
    p.add_argument('--small',type=Path,required=True);p.add_argument('--full',type=Path,required=True)
    p.add_argument('--output',type=Path)
    a=p.parse_args();report=compare(a.small.resolve(),a.full.resolve())
    if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
