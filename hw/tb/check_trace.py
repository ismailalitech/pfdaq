#!/usr/bin/env python3
"""Check sim/trace.txt from tb_pfdaq_stream_src (v0.5) against an independent model.

Frames are rebuilt from the logged stream words and compared with what the RTL should
have sent: magic, sequence numbers (including real-time drops and injected skips),
timestamps, payload (counter, PRBS or FIR, all recomputed here), CRC (zlib), TLAST,
TDEST and the counters read back over APB.
"""
import sys, zlib, struct, collections

MAGIC = 0xA55A
M32 = 0xFFFFFFFF


def xorshift32(x):
    x ^= (x << 13) & M32
    x ^= x >> 17
    x ^= (x << 5) & M32
    return x & M32


def tri16(p):
    p &= 0xFFFF
    t = (~p & 0x7FFF) if p & 0x8000 else (p & 0x7FFF)
    return t - 16384


sc, beats = {}, collections.defaultdict(list)
regs, pre, post = collections.defaultdict(dict), collections.defaultdict(dict), collections.defaultdict(dict)
errs, r99, ended = [], [], False
for line in open(sys.argv[1]):
    p = line.split()
    if not p:
        continue
    if p[0] == 'S':
        v = [int(x) if i != 5 else int(x, 16) for i, x in enumerate(p[1:])]
        keys = ['sc', 'words', 'frames', 'gap', 'tlpf', 'fault', 'rdy', 'mode', 'rt', 'tdn', 'rate', 'credits', 'step', 'abort']
        c = dict(zip(keys, v)); sc[c['sc']] = c
    elif p[0] == 'B':
        beats[int(p[1])].append((int(p[2], 16), int(p[3]), int(p[4]), int(p[5])))
    elif p[0] == 'R':
        n, a, v = int(p[1]), int(p[2], 16), int(p[3], 16)
        if a in (0x20, 0x24, 0x30, 0x40, 0x3C):
            post[n][a] = v
        regs[n][a] = v
        if n == 99:
            r99.append(v)
    elif p[0] == 'P':
        pre[int(p[1])][int(p[2], 16)] = int(p[3], 16)
    elif p[0] == 'END':
        ended = True
    else:
        errs.append('trace: ' + line.strip())

if not ended:
    errs.append('simulation did not reach END')
if regs[0].get(0x00) != 0x50464451 or regs[0].get(0x04) != 0x00050000:
    errs.append('ID/VERSION readback %r' % regs[0])
coef = [((regs[0][0x48 + 4 * k] ^ 0x8000) - 0x8000) for k in range(8)]
if coef != [12000, -15000, 20000, 32767, -32768, 20000, -15000, 12000]:
    errs.append('coefficient readback %s' % coef)
if [x & 3 for x in r99] != [1, 0] or not (r99[1] & 4):
    errs.append('FORCE_ABORT: STATUS before/after %s, expected busy then idle+aborted' % ['%x' % x for x in r99])

for n in sorted(sc):
    c = sc[n]; b = beats[n]
    def bad(m):
        errs.append('scenario %d: %s' % (n, m))
    flen = c['words'] + 4
    fl, fidx = c['fault'], c['fault'] >> 16
    b_all = b
    # burst-mode abort: the real frames are followed by zero fill up to the run's full length
    fillabort = bool(c['abort'] and not c['rt'] and not c['tlpf'])
    if fillabort:
        nreal = (post[n][0x20] - pre[n][0x20]) & M32
        if len(b) != c['frames'] * flen:
            bad('aborted burst sent %d words, the run is %d' % (len(b), c['frames'] * flen))
        if any(x[0] for x in b[nreal * flen:]):
            bad('fill words after frame %d are not all zero' % nreal)
        if any(x[1] for x in b[:-1]) or not (b and b[-1][1]):
            bad('TLAST is not exactly on the last word of the aborted burst')
        b = b[:nreal * flen]
    if len(b) % flen:
        bad('%d words is not a whole number of %d-word frames' % (len(b), flen)); continue
    nfr = len(b) // flen
    status = regs[n].get(0x1C, 0)
    aborted = bool(status & 4)
    if status & 8:
        bad('FIR underrun flag set')
    if c['abort']:
        if not aborted: bad('abort requested but STATUS.aborted is clear')
    elif aborted:
        bad('STATUS.aborted set without an abort')
    if not c['rt'] and not c['abort'] and nfr != c['frames']:
        bad('%d frames sent, %d requested' % (nfr, c['frames']))
    if c['abort'] and nfr >= c['frames']:
        bad('abort did not shorten the run (%d frames)' % nfr)

    exp_seq = pre[n][0x30]
    fir_i = 0                                   # FIR sample index since START
    xs = []                                     # FIR input history
    det = {'crc': [], 'seq': [], 'pay': []}
    gaps = 0
    ts = []
    for i in range(nfr):
        fr = b[i * flen:(i + 1) * flen]; w = [x[0] for x in fr]
        if w[0] != (MAGIC << 16 | c['words']):
            bad('frame %d magic %08x' % (i, w[0]))
        skip = 1 if (fl & 2) and i == fidx else 0
        exp_seq += skip
        if c['rt']:
            if w[1] < exp_seq: bad('frame %d seq %d went backwards (expected >= %d)' % (i, w[1], exp_seq))
            gaps += w[1] - exp_seq
        elif w[1] != exp_seq:
            det['seq'].append(i)
        if skip: det['seq'].append(i)
        exp_seq = w[1] + 1
        # payload model
        if c['mode'] == 0:
            pay = [((w[1] & 0xFFFF) << 16) | k for k in range(c['words'])]
        elif c['mode'] == 1:
            s = w[1] ^ 0x9E3779B9 or 1
            pay = []
            for k in range(c['words']):
                s = xorshift32(s); pay.append(s)
        else:
            pay = []
            for k in range(c['words']):
                xs.append(tri16((fir_i + 1) * c['step'])); fir_i += 1
                y = sum(coef[t] * xs[-1 - t] for t in range(8) if len(xs) - 1 - t >= 0)
                pay.append(y & M32)
        good = list(pay)
        if (fl & 4) and i == fidx:
            pay[0] ^= 0x100
        if w[3:-1] != pay:
            bad('frame %d payload differs at word %d' % (i, next(k for k in range(len(pay)) if w[3 + k] != pay[k])))
        if w[3:-1] != good:
            det['pay'].append(i)
        if zlib.crc32(struct.pack('<%dI' % (flen - 1), *w[:-1])) != w[-1]:
            det['crc'].append(i)
        # TLAST and TDEST
        last_frame = i == nfr - 1
        exp_last = 1 if (c['tlpf'] or c['rt'] or (last_frame and not fillabort)) else 0
        for k, x in enumerate(fr):
            if x[1] != (exp_last if k == flen - 1 else 0):
                bad('frame %d word %d TLAST=%d' % (i, k, x[1])); break
        exp_dst = (i % (c['tdn'] + 1)) if c['rt'] else 0
        if any(x[3] != exp_dst for x in fr):
            bad('frame %d TDEST %d, expected %d' % (i, fr[0][3], exp_dst))
        ts.append(w[2])
    exp_det = {'crc': [fidx] if (fl & 5) and fidx < nfr else [], 'seq': [fidx] if (fl & 2) and fidx < nfr else [],
               'pay': [fidx] if (fl & 4) and fidx < nfr else []}
    if det != exp_det:
        bad('detected %s, expected %s' % (det, exp_det))
    if any(t2 <= t1 for t1, t2 in zip(ts, ts[1:])):
        bad('timestamps not increasing')
    if c['rt'] and len(ts) > 1 and any(((t - ts[0]) & M32) % c['rate'] for t in ts):
        bad('real-time timestamps are not on the %d-cycle slot grid' % c['rate'])

    d = lambda a: (post[n][a] - pre[n][a]) & M32
    if d(0x20) != nfr: bad('FRAMES_SENT delta %d, frames seen %d' % (d(0x20), nfr))
    if d(0x24) != len(b_all): bad('WORDS_SENT delta %d, words seen %d' % (d(0x24), len(b_all)))
    drops = d(0x40)
    if not c['rt'] and drops: bad('DROPS %d outside real-time mode' % drops)
    if c['rt']:
        # drops between frames show as sequence gaps, drops after the last frame only in NEXT_SEQ
        if gaps > drops: bad('sequence gaps %d exceed DROPS %d' % (gaps, drops))
        if not c['abort'] and nfr + drops != c['frames']:
            bad('sent %d + dropped %d != %d slots' % (nfr, drops, c['frames']))
        if post[n][0x3C] != c['credits'] - nfr:
            bad('credits left %d, expected %d' % (post[n][0x3C], c['credits'] - nfr))
    tail_seq = exp_seq + (drops - gaps if c['rt'] else 0)
    if post[n][0x30] != (tail_seq & M32):
        bad('NEXT_SEQ %d, expected %d' % (post[n][0x30], tail_seq))
    extra = ''
    if n == 13 and (nfr, drops) != (12, 18): bad('credit-limited run: sent %d dropped %d, expected 12/18' % (nfr, drops))
    if n == 14 and not (nfr > 0 and drops > 0): bad('busy run should both send and drop (sent %d, dropped %d)' % (nfr, drops))
    if n == 15 and drops: bad('unexpected drops %d' % drops)
    print('scenario %2d: %3d frames x %3d words, mode %d%s, ready %3d%%, sent %3d dropped %3d, detected crc=%s seq=%s pay=%s%s' % (
        n, c['frames'], flen, c['mode'], ' rt/%d dest rate %d' % (c['tdn'] + 1, c['rate']) if c['rt'] else '',
        c['rdy'], nfr, drops, det['crc'], det['seq'], det['pay'],
        (' ABORTED, then %d zero words' % (len(b_all) - len(b)) if fillabort else ' ABORTED') if aborted else ''))

print('RESULT:', 'FAIL' if errs else 'PASS')
for e in errs[:30]:
    print('  ', e)
sys.exit(1 if errs else 0)
