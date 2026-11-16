#!/usr/bin/env python3
"""Summarise the output of run_all.sh (one "<label> <json>" line per run) on the PC.

For every label (mover / source > destination / size) it prints the number of passes and the minimum, mean and
maximum over the passes of: the mean time per transfer, the throughput, the CPU time of the program per transfer
(thread CPU clock), the share of the transfer time that the program was on a CPU, and the cores that were not idle
over the whole loop. It also prints the largest number of wrong words the check found (own view = through the
destination's own window; ddr view = through the non-cached window after the processor read 8 MiB elsewhere) and
the number of failed transfers.

    python3 summarise.py matrix.txt > summary.txt
"""
import json
import sys
from collections import OrderedDict


def stat(v):
    v = [x for x in v if x is not None]
    if not v:
        return 'n/a'
    return '%.4g (%.4g to %.4g)' % (sum(v) / len(v), min(v), max(v))


def main(path):
    runs = OrderedDict()
    idle = []
    for line in open(path):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        label, _, js = line.partition(' ')
        try:
            r = json.loads(js)
        except ValueError:
            print('unreadable line:', line[:100], file=sys.stderr)
            continue
        if r.get('mode') == 'idle':
            idle.append(r['non_idle_cores'])
            continue
        runs.setdefault(label, []).append(r)
    print('# summary of %s: mean (min to max) over the passes' % path.split('/')[-1])
    print('# idle floor, cores not idle: %s over %d measurements of 2 s' % (stat(idle), len(idle)))
    print('# label | passes | reps per pass (first pass) | ms per transfer | MB/s | ms per MiB | program CPU ms per transfer (clock calls subtracted) | program CPU share | cores not idle | max wrong words own view / ddr view | failed transfers')
    for label, rs in runs.items():
        print(' | '.join([
            label,
            str(len(rs)),
            str(rs[0]['reps']),
            stat([r['ms_mean'] for r in rs]),
            stat([r['mb_per_s'] for r in rs]),
            stat([r['ms_per_mib'] for r in rs]),
            stat([r['cpu_ms_net'] for r in rs]),
            stat([r['cpu_share'] for r in rs]),
            stat([r['non_idle_cores'] for r in rs]),
            '%d / %d of %d words' % (max(r['bad_words_own_view'] for r in rs), max(r['bad_words_ddr_view'] for r in rs), rs[0]['counted_words']),
            str(sum(r['fails'] for r in rs) + sum(0 if r['op_ok'] else 1 for r in rs)),
        ]))


if __name__ == '__main__':
    main(sys.argv[1])
