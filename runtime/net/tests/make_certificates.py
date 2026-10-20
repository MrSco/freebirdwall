"""Regenerate public localhost TEST credentials; requires cryptography.

These keys/certificates are intentionally public and never trusted by the board.
Normal tests use the checked-in fixtures and do not need this dependency.
"""
from datetime import datetime,timezone
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID,ExtendedKeyUsageOID
OUT=Path(__file__).with_name('fixtures');OUT.mkdir(exist_ok=True)
def date(year):return datetime(year,1,1,tzinfo=timezone.utc)
def name(s):return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,s)])
def root(s):
    key=ec.generate_private_key(ec.SECP384R1());n=name(s)
    cert=(x509.CertificateBuilder().subject_name(n).issuer_name(n).public_key(key.public_key())
        .serial_number(x509.random_serial_number()).not_valid_before(date(2020)).not_valid_after(date(2100))
        .add_extension(x509.BasicConstraints(ca=True,path_length=1),critical=True)
        .sign(key,hashes.SHA384()))
    return key,cert
key,ca=root('FlightWall PUBLIC TEST root');_,other=root('Untrusted PUBLIC TEST root')
leaf_key=ec.generate_private_key(ec.SECP256R1())
for label,start,end in [('server',2020,2100),('expired',2020,2021),('future',2100,2101)]:
    cert=(x509.CertificateBuilder().subject_name(name('flightwall.test')).issuer_name(ca.subject)
        .public_key(leaf_key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(date(start)).not_valid_after(date(end))
        .add_extension(x509.BasicConstraints(ca=False,path_length=None),critical=True)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName('flightwall.test')]),critical=False)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),critical=False)
        .sign(key,hashes.SHA384()))
    (OUT/(label+'.pem')).write_bytes(cert.public_bytes(serialization.Encoding.PEM))
(OUT/'root.pem').write_bytes(ca.public_bytes(serialization.Encoding.PEM))
(OUT/'untrusted-root.pem').write_bytes(other.public_bytes(serialization.Encoding.PEM))
(OUT/'PUBLIC-TEST-server-key.pem').write_bytes(leaf_key.private_bytes(serialization.Encoding.PEM,
    serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
