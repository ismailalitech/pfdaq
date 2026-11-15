#!/usr/bin/env python3
"""Check sim/lat_trace.txt: recompute every latency and all statistics of the latency unit."""
import sys, collections
def bin_of(v):
    if v < 2: return v
    m = v.bit_length() - 1
    return 2 * m + ((v >> (m - 1)) & 1)
errs = []; ended = False
rise = collections.defaultdict(list); acks = collections.defaultdict(list)
lat_last = collections.defaultdict(list); regs = collections.defaultdict(dict); hist = collections.defaultdict(dict)
order = []
for line in open(sys.argv[1]):
    p = line.split()
    if not p: continue
    if p[0] == 'I': rise[(int(p[1]), int(p[2]))].append(int(p[3]))
    elif p[0] == 'K':
        ph, ch, cyc, pend = map(int, p[1:5]); acks[(ph, ch)].append((cyc, pend)); order.append((ph, ch))
    elif p[0] == 'R':
        ph, a, v = int(p[1]), int(p[2], 16), int(p[3], 16)
        if a in (0xA0, 0xE0): lat_last[(ph, (a - 0xA0) // 0x40)].append(v)
        else: regs[ph][a] = v
    elif p[0] == 'H': hist[(int(p[1]), int(p[2]))][int(p[3])] = int(p[4])
    elif p[0] == 'END': ended = True
    else: errs.append('trace: ' + line.strip())
if not ended: errs.append('simulation did not reach END')
if regs[0].get(0x00) != 0x50464451 or regs[0].get(0x04) != 0x00050000: errs.append('stream source ID/VERSION through pfdaq_core: %r' % regs[0])
def check(ph, ch, expect_overrun, expect_spurious=0, limit=None):
    key = (ph, ch); base = 0x80 + ch * 0x40; r = regs[ph]
    good = [c for c, pend in acks[key] if pend]; spur = len(acks[key]) - len(good)
    def bad(m): errs.append('phase %d ch %d: %s' % (ph, ch, m))
    if len(rise[key]) != len(good): bad('%d interrupts, %d acknowledges' % (len(rise[key]), len(good))); return
    # the observer sees the interrupt one edge after it was set, so latency = strobe - seen + 1
    lats = [k - i + 1 for i, k in zip(rise[key], good)]
    if any(l < 1 for l in lats): bad('non-positive latency')
    if lat_last[key][:len(lats)] != lats: bad('LAT_LAST sequence differs: %s vs %s' % (lat_last[key][:6], lats[:6]))
    exp = {0x14: len(lats), 0x18: len(lats), 0x24: min(lats), 0x28: max(lats), 0x2C: sum(lats) & 0xFFFFFFFF, 0x30: sum(lats) >> 32, 0x3C: expect_spurious}
    for off, v in exp.items():
        if r.get(base + off) != v: bad('reg +0x%02x = %r, expected %d' % (off, r.get(base + off), v))
    if spur != expect_spurious: bad('%d spurious strobes seen' % spur)
    ov = r.get(base + 0x1C)
    if expect_overrun and not ov: bad('expected overruns, OVERRUN=0')
    if not expect_overrun and ov: bad('unexpected OVERRUN=%d' % ov)
    if limit is not None and len(lats) != limit: bad('%d events, COUNT_LIMIT %d' % (len(lats), limit))
    eh = collections.Counter(bin_of(l) for l in lats)
    for b in range(64):
        if hist[key].get(b) != eh.get(b, 0): bad('histogram bin %d = %r, expected %d' % (b, hist[key].get(b), eh.get(b, 0)))
    gaps = [b - a for a, b in zip(rise[key], rise[key][1:])]
    print('phase %d ch %d: %d samples, latency min/mean/max %d/%.1f/%d cycles, overrun %s, bins used %s, event gap %s..%s' % (
        ph, ch, len(lats), min(lats), sum(lats) / len(lats), max(lats), ov, sorted(eh), min(gaps) if gaps else '-', max(gaps) if gaps else '-'))
    return gaps
g0 = check(1, 0, False, limit=60); g1 = check(1, 1, False, limit=90)
if g0 and set(g0) != {400}: errs.append('phase 1 ch 0: event gaps %s, expected exactly PERIOD = 400' % sorted(set(g0))[:5])
if g1 and not (min(g1) >= 331 and max(g1) <= 331 + 63 and len(set(g1)) > 10): errs.append('phase 1 ch 1: jittered gaps %d..%d (%d distinct)' % (min(g1), max(g1), len(set(g1))))
check(2, 0, True, limit=25)
check(3, 1, False, expect_spurious=1)
print('RESULT:', 'FAIL' if errs else 'PASS')
for e in errs[:20]: print('  ', e)
sys.exit(1 if errs else 0)
