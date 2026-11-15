#!/bin/sh
# pfdaq final validation suite (board side, fabric v0.5.0 + FreeRTOS or bare-metal U54_4 context).
# One line per test on stdout: "<id> <json>". Summarised on the PC by tests/host/summarise_suite.py.
# usage: pfdaq_final_suite.sh [quick]
Q=${1:-full}
N=100000; [ "$Q" = quick ] && N=10000
t() { id=$1; shift; out=$("$@" 2>&1 | tail -1); echo "$id $out"; }
ctx=$(pfdaq_cap -L 0 2>/dev/null | grep -o '"context": "[a-z-]*"' | cut -d'"' -f4)
echo "INFO u54_4_context=$ctx kernel=$(uname -r) uptime_s=$(cut -d. -f1 /proc/uptime)"

# --- T4/T5: data integrity, all payload modes, burst mode, supervised ---
t T4-counter      pfdaq_cap -r -m counter -w 252 -f 1024 -n 32
t T4-prbs         pfdaq_cap -r -m prbs    -w 252 -f 1024 -n 32
t T4-fir          pfdaq_cap -r -m fir -S 333 -w 252 -f 1024 -n 32
t T4-fir-37x1004  pfdaq_cap -r -m fir -S 4097 -w 1000 -f 37 -n 16
t T5-max-burst    pfdaq_cap -r -m prbs -w 4092 -f 500 -n 4
t T5-min-frame    pfdaq_cap -r -m counter -w 1 -f 1 -n 64

# --- T9: fault injection per mode ---
for m in counter prbs fir; do
  t T9-crc-$m     pfdaq_cap -r -m $m -w 252 -f 1024 -n 20 -F crc -i 7 -e 5
  t T9-seq-$m     pfdaq_cap -r -m $m -w 252 -f 1024 -n 20 -F seq -i 0 -e 4
  t T9-pay-$m     pfdaq_cap -r -m $m -w 252 -f 1024 -n 20 -F payload -i 1023 -e 10
done
t T9-deadline     pfdaq_cap -r -D 300 -P 5:900 -w 252 -f 1024 -n 12

# --- graceful abort: 8 MB burst aborted after 2 ms, and after 50 us ---
t ABORT-2ms       pfdaq_cap -A 2000 -m prbs -w 4092 -f 500
t ABORT-50us      pfdaq_cap -A 50 -m fir -w 252 -f 8000
t ABORT-after     pfdaq_cap -r -m counter -w 252 -f 1024 -n 8

# --- T6: real-time mode, 1 KiB frames, offered rate sweep (8 ns cycles), 4 descriptors ---
for r in 125000 12500 6250 2500 1250 625; do
  t T6-rt-$r      pfdaq_cap -m prbs -w 252 -T $r -n 4000 -d 4
done
t T6-rt-fir       pfdaq_cap -m fir -S 999 -w 252 -T 6250 -n 4000 -d 4
t T6-rt-1dest     pfdaq_cap -m counter -w 252 -T 6250 -n 4000 -d 1
t T6-rt-fault     pfdaq_cap -m counter -w 252 -T 12500 -n 1000 -d 4 -F payload -i 10

# --- T7: shared memory between Linux and U54_4 ---
t T7-noncached    pfdaq_cap -X 65536 -n 50
t T7-cached       pfdaq_cap -X 65536 -n 50 -c

# --- T8: interrupt latency matrix ---
load_on()  { for i in 1 2 3; do (while :; do :; done) & echo $! >> /tmp/lat_load.pids; done
             (while :; do pfdaq_cap -w 252 -f 1024 -n 64 >/dev/null 2>&1; done) & echo $! >> /tmp/lat_load.pids; }
load_off() { for p in $(cat /tmp/lat_load.pids 2>/dev/null); do kill $p 2>/dev/null; done; rm -f /tmp/lat_load.pids
             killall pfdaq_cap 2>/dev/null; sleep 1; }
rm -f /tmp/lat_load.pids
pfdaq_cap -L 0 >/dev/null
t T8-u54_4-isr-idle        pfdaq_lat -c 1 -p 200 -n $N
t T8-linux-uio-idle        pfdaq_lat -c 0 -p 500 -n $N
t T8-linux-uio-fifo-idle   pfdaq_lat -c 0 -p 500 -n $N -R 80
load_on
t T8-u54_4-isr-load        pfdaq_lat -c 1 -p 200 -n $N
t T8-linux-uio-fifo-load   pfdaq_lat -c 0 -p 500 -n $N -R 80
load_off
if [ "$ctx" = freertos ]; then
  pfdaq_cap -L 1 >/dev/null
  t T8-u54_4-task-idle     pfdaq_lat -c 1 -p 200 -n $N
  load_on
  t T8-u54_4-task-load     pfdaq_lat -c 1 -p 200 -n $N
  load_off
  pfdaq_cap -L 0 >/dev/null
fi
# kernel handler: rebind the device to pfdaq_uio for the measurement, then back to uio_pdrv_genirq
D=40000f00.uio
if modprobe pfdaq_uio 2>/dev/null && [ -e /sys/bus/platform/devices/$D ]; then
  echo $D > /sys/bus/platform/drivers/uio_pdrv_genirq/unbind
  echo pfdaq_uio > /sys/bus/platform/devices/$D/driver_override
  echo $D > /sys/bus/platform/drivers/pfdaq_uio/bind
  udevadm settle 2>/dev/null; sleep 1
  t T8-linux-kernel-isr-idle pfdaq_lat -k -p 200 -n $N
  load_on
  t T8-linux-kernel-isr-load pfdaq_lat -k -p 200 -n $N
  load_off
  echo $D > /sys/bus/platform/drivers/pfdaq_uio/unbind
  echo uio_pdrv_genirq > /sys/bus/platform/devices/$D/driver_override
  echo $D > /sys/bus/platform/drivers/uio_pdrv_genirq/bind
  udevadm settle 2>/dev/null; sleep 1
  echo > /sys/bus/platform/devices/$D/driver_override
else
  echo "T8-linux-kernel-isr-idle {\"pass\": false, \"error\": \"pfdaq_uio module not available\"}"
fi

# --- T9: consumer killed mid-run, supervisor must alarm, a new run must work ---
( pfdaq_cap -r -D 300 -w 252 -f 1024 -n 100000 >/dev/null 2>&1 ) &
P=$!; sleep 2; pkill -9 -f 'pfdaq_cap -r -D 300'; wait $P 2>/dev/null; sleep 1
t T9-after-kill   pfdaq_cap -r -w 252 -f 1024 -n 16 2>/dev/null

# --- T9: stop and restart the U54_4 context through remoteproc ---
R=/sys/class/remoteproc/remoteproc0
s0=$(cat $R/state)
echo stop > $R/state 2>/dev/null; sleep 1; s1=$(cat $R/state)
fw=$(ls /lib/firmware/polarfire-soc-*amp-demo.elf /usr/lib/firmware/polarfire-soc-*amp-demo.elf 2>/dev/null | head -1)
[ -n "$fw" ] && basename "$fw" > $R/firmware
echo start > $R/state 2>/dev/null; sleep 3; s2=$(cat $R/state)
ch=$(ls /sys/bus/rpmsg/devices/ | grep -c pfdaq-supervisor)
after=$(pfdaq_cap -r -w 252 -f 1024 -n 8 2>&1 | tail -1 | grep -o '^{"pass": [a-z]*' | grep -o '[a-z]*$')
ok=false; [ "$s1" = offline ] && [ "$s2" = running ] && [ "$ch" = 1 ] && [ "$after" = true ] && ok=true
echo "T9-remoteproc {\"pass\": $ok, \"state_before\": \"$s0\", \"after_stop\": \"$s1\", \"firmware\": \"$(basename "$fw" 2>/dev/null)\", \"after_start\": \"$s2\", \"supervisor_channel\": $ch, \"capture_after\": \"$after\"}"

# --- known issue, run last because it can disturb later transfers until a reboot ---
t KNOWN-odd-frames pfdaq_cap -O -m counter -w 1001 -f 37 -n 4

echo SUITE_DONE
