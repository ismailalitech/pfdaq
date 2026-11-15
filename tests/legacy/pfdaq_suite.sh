#!/bin/sh
# Board-side capture suite: one summary line per test, stops if the generator is left busy.
cd /root
r(){ devmem2 $1 w | tail -1 | sed "s/.*: //"; }
t(){ python3 pfdaq_capture.py "$@" > /tmp/last.json; python3 - "$@" <<PY
import json,sys
r=json.load(open('/tmp/last.json')); a=r['args']
n = a['frames']
mbps = r['bytes']*(n-1)/n/r['fabric_burst_s']/1e6 if r.get('fabric_burst_s') else 0
pf = r.get('per_frame_loop_us', '')
print('%-52s pass=%s bytes=%d host_s=%.6f fabric_MB/s=%.1f stall=%d detected=%s %s %s' % (' '.join(sys.argv[1:]), r['pass'], r['bytes'], r['elapsed_s'], mbps, r['gen_stall_cycles'], {k:v for k,v in r['detected'].items() if v}, r['errors'][:2] or '', pf))
PY
[ "$(r 0x40000F1C)" = "0x00000002" ] || { echo "GENERATOR NOT IDLE/DONE: $(r 0x40000F1C)"; exit 1; }; }
t --words 8 --frames 2
t --words 124 --frames 1
t --words 1020 --frames 1
t --words 500 --frames 40
t --words 2044 --frames 64
t --words 4092 --frames 500
t --words 13 --frames 20 --per-frame
t --words 252 --frames 2000 --per-frame
t --words 100 --frames 10 --fault crc --fault-frame 3
t --words 100 --frames 10 --fault seq --fault-frame 5
t --words 100 --frames 10 --fault payload --fault-frame 0
t --words 100 --frames 10 --per-frame --fault crc --fault-frame 9
t --words 8 --frames 2
echo SUITE_DONE
