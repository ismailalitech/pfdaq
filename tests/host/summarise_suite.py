#!/usr/bin/env python3
"""Summarise the output of tests/board/pfdaq_final_suite.sh (lines "<id> <json>") as a Markdown table."""
import json, sys

rows, info, done = [], [], False
for line in open(sys.argv[1], errors='replace'):
    line = line.strip()
    if not line:
        continue
    if line == 'SUITE_DONE':
        done = True; continue
    if line.startswith('INFO'):
        info.append(line[5:]); continue
    if not line.split(' ', 1)[0][:2] in ('T4', 'T5', 'T6', 'T7', 'T8', 'T9', 'AB', 'KN'):
        continue            # shell job messages and other noise
    tid, _, js = line.partition(' ')
    try:
        r = json.loads(js)
    except ValueError:
        rows.append((tid, False, 'unparsable: ' + js[:80])); continue
    m = r.get('mode')
    if 'latency_us' in r:
        l = r['latency_us']
        d = 'events %d, overrun %d, min %.2f / mean %.2f / p99<= %.2f / max %.2f us' % (
            r['events'], r['overrun'], l['min'], l['mean'], l['p99_le'], l['max'])
    elif m == 'realtime':
        d = 'slot %.1f us (%.1f MB/s offered), sent %d, dropped %d (%.1f %%), verified %d, delivered %.1f MB/s, errors %d/%d/%d' % (
            r['slot_us'], r['offered_mb_per_s'], r['sent'], r['dropped'], r['drop_pct'], r['verified'],
            r['delivered_mb_per_s'], r['bad_seq'], r['bad_payload'], r['bad_crc'])
    elif m == 'abort':
        d = 'DMA completed %s after %.3f ms, %d of %d frames real, then zero fill, aborted flag %s, nothing past the burst %s' % (
            r['dma_completed'], r['start_to_irq_s'] * 1e3, r['frames_sent'], r['frames_requested'],
            r['aborted_flag'], r['nothing_after_burst'])
    elif m == 'shm':
        d = '%s, %d words x %d, mismatches U54_4 %d / Linux %d, %.2f ms per check (%s)' % (
            r['region'], r['words'], r['iterations'], r['mismatch_seen_by_u54_4'], r['mismatch_seen_by_linux'],
            r['mean_check_ms'], r['context'])
    elif 'bursts' in r:
        d = '%d frames, %.1f MB, errors seq/pay/crc %d/%d/%d, faults %d' % (
            r['frames'], r['bytes'] / 1e6, r['bad_seq'], r['bad_payload'], r['bad_crc'], r['faults_injected'])
        if 'supervisor' in r:
            s = r['supervisor']
            d += ', supervisor %s (%d status, %d misses)' % ('ok' if s['pass'] else 'FAIL', s['status_rx'], s['deadline_miss'])
    else:
        d = ', '.join('%s=%s' % (k, v) for k, v in r.items() if k != 'pass')
    rows.append((tid, r.get('pass'), d))

print('Context: ' + '; '.join(info))
print()
print('| Test | Result | Detail |')
print('|---|---|---|')
# characterisation rows (no pass/fail): real-time rate sweep and Linux user-space latency, which
# misses events by design under load; known issues are reported as such
def kind(tid):
    if tid.startswith('KNOWN') or tid == 'T9-remoteproc':
        return 'known'
    if tid.startswith('T6-rt-') and tid not in ('T6-rt-1dest',) or tid.startswith('T8-linux-'):
        return 'char'
    return 'test'
label = {'test': lambda ok: 'PASS' if ok else 'FAIL', 'char': lambda ok: 'measured', 'known': lambda ok: 'known issue' if not ok else 'PASS'}
for tid, ok, d in rows:
    print('| %s | %s | %s |' % (tid, label[kind(tid)](ok), d))
tests = [(t, ok) for t, ok, _ in rows if kind(t) == 'test']
print()
print('%d/%d functional tests passed, %d measurements, %d known issues%s' % (
    sum(1 for _, ok in tests if ok), len(tests), sum(1 for t, _, _ in rows if kind(t) == 'char'),
    sum(1 for t, _, _ in rows if kind(t) == 'known'), '' if done else ' (suite did not finish)'))
