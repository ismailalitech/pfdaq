#!/usr/bin/env python3
"""Recount a few of the coverage totals from the stream trace, without the Verilog monitors:
accepted beats, packets, frame headers, and packets per TDEST. The numbers should equal the line
"COV runs started ..., packets ..., frame headers ..., accepted beats ..." and bins 35 to 38 of coverage.txt.

    python3 experiments/lab-assertions/recount_trace.py build/sim/sim/trace.txt
"""
import collections, sys

beats, words, scen = set(), collections.defaultdict(list), {}
for line in open(sys.argv[1]):
    p = line.split()
    if p[0] == 'S':
        scen[int(p[1])] = int(p[2])                      # payload words of the scenario
    elif p[0] == 'B':                                    # B scenario word last time tdest
        sc, w, last, t, d = int(p[1]), int(p[2], 16), int(p[3]), int(p[4]), int(p[5])
        beats.add((sc, t))                               # two words with one time are one beat
        words[sc].append((w, last, d))
packets, headers, dest = 0, 0, collections.Counter()
for sc, ws in words.items():
    flen, pos = scen[sc] + 4, 0
    for w, last, d in ws:
        if pos == 0 and w >> 16 == 0xA55A:
            headers += 1
        pos = 0 if last else (pos + 1) % flen
        if last:
            packets += 1
            dest[d] += 1
print('accepted beats %d, packets %d, frame headers %d' % (len(beats), packets, headers))
print('packets per TDEST: ' + ', '.join('%d: %d' % (d, dest[d]) for d in range(4)))
