#!/usr/bin/env python3
"""Prepare SDK-only N4 validation APP/assets and local certificates outside Git.

Does not connect, flash, install or alter the SDK contract. Build the generated
project using the selected SDK's ordinary `aster build --target xtensa --aot`.
"""
import argparse
from datetime import datetime, timedelta, timezone
import ipaddress
import json
import math
from pathlib import Path
import shutil
import struct
import wave


def certificates(directory, host):
    from cryptography import x509
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
    directory.mkdir(parents=True, exist_ok=True)
    now = datetime.now(timezone.utc)
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Aster N4 local test CA')])
    ca = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
          .public_key(key.public_key()).serial_number(x509.random_serial_number())
          .not_valid_before(now-timedelta(days=365)).not_valid_after(now+timedelta(days=365))
          .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
          .add_extension(x509.KeyUsage(True, False, False, False, False, True, True, False, False), True)
          .sign(key, hashes.SHA256()))
    (directory/'ca.pem').write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    for kind in ('server', 'wrong', 'expired'):
        leaf = ec.generate_private_key(ec.SECP256R1())
        san = [x509.DNSName('localhost' if kind != 'wrong' else 'wrong.invalid')]
        if host and kind != 'wrong':
            try:
                san.append(x509.IPAddress(ipaddress.ip_address(host)))
            except ValueError:
                san.append(x509.DNSName(host))
        cert = (x509.CertificateBuilder().subject_name(x509.Name([
                x509.NameAttribute(NameOID.COMMON_NAME, 'Aster N4 test peer')]))
                .issuer_name(name).public_key(leaf.public_key())
                .serial_number(x509.random_serial_number())
                .not_valid_before(now-timedelta(days=60))
                .not_valid_after(now-timedelta(days=30) if kind == 'expired' else now+timedelta(days=60))
                .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
                .add_extension(x509.SubjectAlternativeName(san), False)
                .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), False)
                .sign(key, hashes.SHA256()))
        (directory/(kind+'.pem')).write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        target = directory/(kind+'.key')
        target.write_bytes(leaf.private_bytes(serialization.Encoding.PEM,
                           serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        target.chmod(0o600)


def assets(directory):
    for name, seconds, frequency, amplitude in [('bed.wav', 2, 220, 900), ('cue.wav', .08, 660, 2000)]:
        samples = bytearray()
        count = int(16000*seconds)
        for i in range(count):
            v = int(amplitude*math.sin(i*frequency*2*math.pi/16000))
            samples.extend(struct.pack('<hh', v, v))
        with wave.open(str(directory/name), 'wb') as out:
            out.setparams((2, 2, 16000, count, 'NONE', 'not compressed'))
            out.writeframes(samples)
    pixels = b''.join(bytes((x*4, y*4, (x+y)*2)) for y in range(64) for x in range(64))
    header = struct.pack('<2sIHHI', b'BM', 54+len(pixels), 0, 0, 54)
    dib = struct.pack('<IiiHHIIiiII', 40, 64, 64, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    (directory/'tile.bmp').write_bytes(header+dib+pixels)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--fast', action='store_true')
    p.add_argument('--scene', choices=['mixed','low','full','transaction'], default='mixed',
                   help='Fixed scene for visual isolation; network modes still rotate')
    p.add_argument('--certs', type=Path)
    a = p.parse_args()
    if not a.host or len(a.host) > 253 or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-' for c in a.host):
        p.error('host must be an ASCII DNS name or IPv4')
    source = Path(__file__).parent/'network-stress'
    a.output.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, a.output, dirs_exist_ok=True)
    manifest = (source/'app.toml').read_text()
    manifest = manifest.replace("defines=['N4_HOST=\"127.0.0.1\"']",
        'defines='+json.dumps([f'N4_HOST="{a.host}"', f'N4_FAST={int(a.fast)}',
            f'N4_SCENE={dict(mixed=-1,low=0,full=1,transaction=2)[a.scene]}']))
    (a.output/'app.toml').write_text(manifest)
    assets(a.output)
    if a.certs:
        certificates(a.certs, a.host)
    print(a.output)


if __name__ == '__main__':
    main()
