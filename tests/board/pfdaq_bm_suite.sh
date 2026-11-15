#!/bin/sh
t() { id=$1; shift; out=$("$@" 2>&1 | tail -1); echo "$id $out"; }
ctx=$(pfdaq_cap -L 0 2>/dev/null | grep -o '"context": "[a-z-]*"' | cut -d'"' -f4)
echo "INFO u54_4_context=$ctx remoteproc=$(cat /sys/class/remoteproc/remoteproc0/state)"
t T4-counter      pfdaq_cap -r -m counter -w 252 -f 1024 -n 32
t T4-fir          pfdaq_cap -r -m fir -S 333 -w 252 -f 1024 -n 32
t T9-crc-prbs     pfdaq_cap -r -m prbs -w 252 -f 1024 -n 20 -F crc -i 7 -e 5
t T9-deadline     pfdaq_cap -r -D 300 -P 5:900 -w 252 -f 1024 -n 12
t T7-noncached    pfdaq_cap -X 65536 -n 50
t T7-cached       pfdaq_cap -X 65536 -n 50 -c
load_on()  { for i in 1 2 3; do (while :; do :; done) & echo $! >> /tmp/lat_load.pids; done
             (while :; do pfdaq_cap -w 252 -f 1024 -n 64 >/dev/null 2>&1; done) & echo $! >> /tmp/lat_load.pids; }
load_off() { for p in $(cat /tmp/lat_load.pids 2>/dev/null); do kill $p 2>/dev/null; done; rm -f /tmp/lat_load.pids; killall pfdaq_cap 2>/dev/null; sleep 1; }
rm -f /tmp/lat_load.pids
t T8-u54_4-isr-idle pfdaq_lat -c 1 -p 200 -n 100000
load_on
t T8-u54_4-isr-load pfdaq_lat -c 1 -p 200 -n 100000
load_off
echo SUITE_DONE
