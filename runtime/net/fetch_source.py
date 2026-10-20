"""Explicit, pinned developer dependency download; never contacts the board."""
import argparse,hashlib,json,tarfile,tempfile,urllib.request
from pathlib import Path,PurePosixPath
HERE=Path(__file__).resolve().parent

def tree_digest(folder):
    h=hashlib.sha256()
    for p in sorted(folder.rglob('*')):
        if p.is_file() and p.name!='.flightwall-source.json':
            h.update(p.relative_to(folder).as_posix().encode()+b'\0')
            h.update(hashlib.sha256(p.read_bytes()).digest())
    return h.hexdigest()

def fetch(destination):
    pin=json.loads((HERE/'mbedtls-source.json').read_text())
    destination=destination.resolve()
    if destination.exists():
        receipt=json.loads((destination/'.flightwall-source.json').read_text())
        if receipt.get('archive_sha256')!=pin['sha256'] or receipt.get('tree_sha256')!=tree_digest(destination):
            raise ValueError('Dependency changed; use a new dependency directory. Existing files preserved.')
        return destination
    destination.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent,prefix='tls-source-') as tmp:
        work=Path(tmp);archive=work/'source.tar.bz2'
        with urllib.request.urlopen(pin['url'],timeout=60) as response,archive.open('wb') as out:
            total=0;h=hashlib.sha256()
            while data:=response.read(65536):
                total+=len(data)
                if total>32*1024*1024:raise ValueError('Dependency archive exceeds size limit')
                h.update(data);out.write(data)
        if h.hexdigest()!=pin['sha256']:raise ValueError('Dependency SHA256 mismatch')
        unpack=work/'unpacked';unpack.mkdir()
        with tarfile.open(archive) as tar:
            members=tar.getmembers()
            if sum(m.size for m in members)>128*1024*1024:raise ValueError('Dependency unpacked size limit')
            for m in members:
                path=PurePosixPath(m.name)
                if path.is_absolute() or '..' in path.parts or '\\' in m.name or ':' in m.name or not(m.isfile() or m.isdir()):
                    raise ValueError('Unsupported dependency archive member')
            tar.extractall(unpack,members=members,filter='data')
        roots=list(unpack.iterdir())
        if len(roots)!=1 or not (roots[0]/'include/mbedtls/ssl.h').is_file():raise ValueError('Unexpected dependency layout')
        source=roots[0]
        receipt={'archive_sha256':pin['sha256'],'tree_sha256':tree_digest(source),'version':pin['version']}
        (source/'.flightwall-source.json').write_text(json.dumps(receipt,indent=2)+'\n')
        source.rename(destination)
    return destination

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=HERE.parent/'build/net-deps/mbedtls')
    print(fetch(p.parse_args().output))
