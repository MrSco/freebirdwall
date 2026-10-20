"""Generate/check the private TLS symbol namespace for the pinned configuration."""
import argparse,re,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
def symbols(nm,archives):
    names=set()
    for archive in archives:
        r=subprocess.run([nm,'-g','--defined-only',str(archive)],capture_output=True,text=True,check=True)
        names.update(re.findall(r'^\s*[0-9a-fA-F]+\s+[A-Z]\s+(\w+)\s*$',r.stdout,re.M))
    return names
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--nm',required=True)
    p.add_argument('--check',action='store_true');p.add_argument('archives',type=Path,nargs='+');a=p.parse_args()
    names=symbols(a.nm,a.archives)
    public={n for n in names if n.startswith(('mbedtls_','psa_'))}
    if a.check:
        if public:raise SystemExit('Unisolated TLS exports: '+', '.join(sorted(public)))
        print('Private TLS symbol namespace verified')
    else:
        public.update(n[3:] for n in names if n.startswith(('fw_mbedtls_','fw_psa_')))
        public.add('mbedtls_ms_time') # implementation supplied by tls_client.cpp
        header='/* Generated from pinned/configured library exports; regenerate after config changes. */\n#ifndef FLIGHTWALL_MBEDTLS_SYMBOL_PREFIX_H\n#define FLIGHTWALL_MBEDTLS_SYMBOL_PREFIX_H\n'
        header+=''.join('#define '+n+' fw_'+n+'\n' for n in sorted(public))+'#endif\n'
        (HERE/'fw_mbedtls_symbols.h').write_text(header,encoding='utf-8',newline='\n')
        print('Generated',len(public),'private TLS symbol names')
