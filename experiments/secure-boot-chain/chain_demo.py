#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A toy chain of trust, to show how secure boot works. It runs on a PC and boots nothing.

    ROM  --verifies-->  stage 1  --verifies-->  stage 2  --verifies-->  application

Each link is checked with the same algorithms the PolarFire SoC boot monitor uses (SHA-384 and
ECDSA P-384). The only thing that has to be trusted from the start is the ROM, which holds the
root public key and the lowest version it will accept. Every other image carries the public key
for the next link inside the part that its own signature covers.

Run it:  python3 chain_demo.py        (needs the Python 'cryptography' package)
"""
import struct

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

PUB = serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint
HDR = '<4sII'          # magic, version, payload length
KEY_LEN = 97           # an uncompressed P-384 point: 0x04 + 48 + 48 bytes


def new_key():
    return ec.generate_private_key(ec.SECP384R1())


def pub_bytes(key):
    return key.public_key().public_bytes(*PUB)


def sign_image(signing_key, magic, version, payload):
    body = struct.pack(HDR, magic, version, len(payload)) + payload
    r, s = decode_dss_signature(signing_key.sign(body, ec.ECDSA(hashes.SHA384())))
    return body + r.to_bytes(48, 'big') + s.to_bytes(48, 'big')


class Refused(Exception):
    pass


def check_image(blob, trusted_pub, magic, min_version, name):
    body, sig = blob[:-96], blob[-96:]
    m, version, n = struct.unpack_from(HDR, body)
    if m != magic or len(body) != struct.calcsize(HDR) + n:
        raise Refused('%s: malformed header' % name)
    pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP384R1(), trusted_pub)
    try:
        pub.verify(encode_dss_signature(int.from_bytes(sig[:48], 'big'), int.from_bytes(sig[48:], 'big')),
                   body, ec.ECDSA(hashes.SHA384()))
    except InvalidSignature:
        raise Refused('%s: signature is not valid' % name)
    if version < min_version:
        raise Refused('%s: version %d is older than the minimum %d (rollback)' % (name, version, min_version))
    return body[struct.calcsize(HDR):], version


def boot(rom_root_pub, rom_min_version, stage1, stage2, app):
    log = []
    try:
        p1, v = check_image(stage1, rom_root_pub, b'ST1.', rom_min_version, 'stage 1')
        log.append('ROM: stage 1 (version %d) is genuine, starting it' % v)
        p2, v = check_image(stage2, p1[:KEY_LEN], b'ST2.', 1, 'stage 2')
        log.append('stage 1: stage 2 (version %d) is genuine, starting it' % v)
        pa, v = check_image(app, p2[:KEY_LEN], b'APP.', 1, 'application')
        log.append('stage 2: application (version %d) is genuine, starting it' % v)
        log.append('application running: %r' % pa.decode())
        return True, log
    except Refused as e:
        log.append('HALT  %s' % e)
        return False, log


def build(root, k1, k2, stage1_version=2):
    stage1 = sign_image(root, b'ST1.', stage1_version, pub_bytes(k1))
    stage2 = sign_image(k1, b'ST2.', 1, pub_bytes(k2))
    app = sign_image(k2, b'APP.', 1, b'hello from the application')
    return stage1, stage2, app


def flip_bit(blob, position):
    b = bytearray(blob)
    b[position] ^= 1
    return bytes(b)


def scenario(title, result):
    ok, log = result
    print('\n== %s' % title)
    for line in log:
        print('   ' + line)
    print('   -> %s' % ('BOOTED' if ok else 'stopped, nothing further was run'))
    return ok


def main():
    root, k1, k2, attacker = new_key(), new_key(), new_key(), new_key()
    rom_pub, rom_min = pub_bytes(root), 2
    stage1, stage2, app = build(root, k1, k2)
    results = []

    results.append(scenario('1. Everything is genuine', boot(rom_pub, rom_min, stage1, stage2, app)) is True)
    results.append(scenario('2. One bit of the application is flipped',
                            boot(rom_pub, rom_min, stage1, stage2, flip_bit(app, 20))) is False)
    forged_app = sign_image(attacker, b'APP.', 1, b'malicious application')
    results.append(scenario('3. An application signed by someone else',
                            boot(rom_pub, rom_min, stage1, stage2, forged_app)) is False)
    forged_s2 = sign_image(attacker, b'ST2.', 1, pub_bytes(attacker))
    results.append(scenario('4. A replacement stage 2 that names the attacker\'s key',
                            boot(rom_pub, rom_min, stage1, forged_s2, forged_app)) is False)
    old_s1, _, _ = build(root, k1, k2, stage1_version=1)
    results.append(scenario('5. An old, genuinely signed stage 1 (rollback)',
                            boot(rom_pub, rom_min, old_s1, stage2, app)) is False)
    results.append(scenario('6. Stage 1 with its embedded key swapped',
                            boot(rom_pub, rom_min, flip_bit(stage1, 20), stage2, app)) is False)

    print('\n%d of %d scenarios behaved as a chain of trust should.' % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
