#!/usr/bin/env python3
"""Add five runs to a scratch copy of the stream testbench and run make sim-assert on it, to see which
coverage holes close and whether the checks and check_trace.py still pass. The repository is not modified.

    python3 experiments/lab-assertions/extra_runs.py [scratch-dir]
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HW = os.path.join(ROOT, 'hw')
MARK = '        // FORCE_ABORT into a stalled sink'
RUNS = '''        run(30,  8,    1,    0,  0,  32'h0000_0000, 100, 0,   0, 0,  2,   0,      0,    0);   // a single frame
        run(31,  9,    6,    0,  0,  32'h0000_0000, 100, 0,   1, 0,  60,  100,    0,    0);   // real-time, one destination, odd frame length
        run(32,  40,   20,   0,  0,  32'h0000_0000, 100, 0,   1, 1,  5,   100,    0,    0);   // real-time, RATE 5 (a slot comes every 5 clocks)
        run(33,  20,   5,    0,  1,  32'h0000_0000, 100, 2,   0, 0,  2,   0,      300,  0);     // TLAST per frame, FIR payload
        run(34,  20,   50,   0,  0,  32'h0000_0000, 100, 0,   1, 0,  500, 100,    0,    100);   // abort while waiting for a real-time slot
'''
scratch = sys.argv[1] if len(sys.argv) > 1 else '/tmp/extra-runs'
os.makedirs(scratch, exist_ok=True)
text = open(os.path.join(HW, 'tb', 'tb_pfdaq_stream_src.v')).read()
assert text.count(MARK) == 1
tb = os.path.join(scratch, 'tb_extra.v')
open(tb, 'w').write(text.replace(MARK, RUNS + MARK))
out = os.path.join(scratch, 'out')
subprocess.run(['make', '-C', HW, 'sim-assert', 'OUT=' + out, 'ASSERT_TB=' + tb], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for f in ('assert_results.txt', 'assert_trace_verdict.txt'):
    print([l for l in open(os.path.join(out, f)).read().splitlines() if 'total' in l or 'RESULT' in l][-2:])
cov = open(os.path.join(out, 'coverage.txt')).read().splitlines()
print('\n'.join(l for l in cov if 'NOT HIT' in l or l.startswith('COV bins') or l.startswith('COV runs')))
