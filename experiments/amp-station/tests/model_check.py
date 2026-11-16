#!/usr/bin/env python3
"""Independent check of the frame model: builds the first three frames of a run (sequence 1000..1002,
FIR payload, 252 words, CRC-32 by zlib) in Python from the description in hw/rtl/pfdaq_stream_src.v
and compares them word for word with what station_common.h generates ("test_units dump")."""
import struct, subprocess, sys, zlib

STEP = 256
COEF = [12000, -15000, 20000, 32767, -32768, 20000, -15000, 12000]

def tri16(p):
    q = p & 0xFFFF
    t = (~q & 0x7FFF) if (q & 0x8000) else (q & 0x7FFF)
    return t - 16384

def frames(first_seq, n, payload=252):
    xh = [0] * 8
    i = 0
    out = []
    for f in range(n):
        w = [0xA55A0000 | payload, first_seq + f, 0]
        for _ in range(payload):
            xh = [tri16((i + 1) * STEP)] + xh[:7]
            i += 1
            y = sum(c * x for c, x in zip(COEF, xh))
            w.append(y & 0xFFFFFFFF)
        crc = zlib.crc32(struct.pack('<%dI' % len(w), *w)) & 0xFFFFFFFF
        w.append(crc)
        out.append(w)
    return out

def main(binary):
    got = subprocess.run([binary, 'dump'], capture_output=True, text=True, check=True).stdout.splitlines()
    exp = frames(1000, 3)
    bad = 0
    for line, e in zip(got, exp):
        w = [int(x, 16) for x in line.split()[2:]]
        if w != e:
            bad += 1
            print('MISMATCH in', line.split()[:2])
    sig = [max(abs(((v + 2**31) % 2**32) - 2**31) for v in e[3:-1]) for e in exp]
    print('model_check: %d frames compared, %d differ; peak |y| of the first frames %s' % (len(exp), bad, sig))
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
