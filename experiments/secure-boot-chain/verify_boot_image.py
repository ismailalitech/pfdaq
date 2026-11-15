#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent verifier for an HSS boot image signed by hss-payload-generator -p.

It follows the check in the Hart Software Services (services/boot/hss_boot_secure.c):
  1. take the whole boot image (bootImageLength bytes);
  2. set the 144-byte signature field to zero;
  3. check an ECDSA P-384 signature over SHA-384 of that image.
The signature field is the SHA-384 digest (48 bytes) followed by r and s (48 bytes each, big endian).

The field's offset depends on constants chosen when the HSS is built, so it is found by its own
consistency: the 8-byte image length that sits directly in front of the signature must equal the
file size, and the stored digest must match the zeroed image. Nothing is trusted from the file
except where to look.

usage: verify_boot_image.py <image.bin> <public key, PEM or DER>
exit status 0 = signature valid, 1 = not valid, 2 = could not parse
"""
import hashlib
import struct
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature

MAGIC = 0xB007C0DE
SIG_LEN = 48 + 96


def find_signature(img):
    magic, _version, header_len = struct.unpack_from('<IIQ', img, 0)
    if magic != MAGIC:
        raise ValueError('not an HSS boot image (magic 0x%08x)' % magic)
    for off in range(16, header_len - SIG_LEN, 8):
        if struct.unpack_from('<Q', img, off - 8)[0] == len(img):
            return off
    raise ValueError('no image-length field matching the file size in the header')


def verify(img, pub):
    off = find_signature(img)
    stored = img[off:off + SIG_LEN]
    zeroed = bytearray(img)
    zeroed[off:off + SIG_LEN] = b'\0' * SIG_LEN
    digest_ok = hashlib.sha384(zeroed).digest() == stored[:48]
    r = int.from_bytes(stored[48:96], 'big')
    s = int.from_bytes(stored[96:144], 'big')
    try:
        pub.verify(encode_dss_signature(r, s), bytes(zeroed), ec.ECDSA(hashes.SHA384()))
        return True, off, digest_ok
    except InvalidSignature:
        return False, off, digest_ok


def load_public(path):
    data = open(path, 'rb').read()
    return (serialization.load_pem_public_key if data.startswith(b'-----') else serialization.load_der_public_key)(data)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        ok, off, digest_ok = verify(open(sys.argv[1], 'rb').read(), load_public(sys.argv[2]))
    except ValueError as e:
        print('cannot parse: %s' % e)
        sys.exit(2)
    print('signature field at byte %d; stored digest %s the image' % (off, 'matches' if digest_ok else 'does NOT match'))
    print('ECDSA P-384 / SHA-384 signature: %s' % ('VALID' if ok else 'NOT VALID'))
    sys.exit(0 if ok else 1)
