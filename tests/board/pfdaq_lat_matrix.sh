#!/bin/sh
# Board side. Latency characterisation: FreeRTOS ISR and Linux user space, idle and under load.
# One JSON line per condition on stdout, prefixed by the condition name.
N=${1:-100000}
run() { name=$1; shift; echo "$name $(pfdaq_lat "$@")"; }
load_on() {
    for i in 1 2 3; do (while :; do :; done) & echo $! >> /tmp/lat_load.pids; done      # one busy loop per Linux CPU
    (while :; do pfdaq_cap -w 252 -f 1024 -n 64 >/dev/null 2>&1; done) & echo $! >> /tmp/lat_load.pids   # DMA + memory traffic
}
load_off() { for p in $(cat /tmp/lat_load.pids 2>/dev/null); do kill $p 2>/dev/null; done; rm -f /tmp/lat_load.pids; killall pfdaq_cap 2>/dev/null; sleep 1; }
rm -f /tmp/lat_load.pids
run freertos_isr_idle        -c 1 -p 200 -n $N
run linux_uio_idle           -c 0 -p 500 -n $N
run linux_uio_fifo_idle      -c 0 -p 500 -n $N -R 80
load_on
run freertos_isr_linux_load  -c 1 -p 200 -n $N
run linux_uio_load           -c 0 -p 2000 -n $((N / 10))
run linux_uio_fifo_load      -c 0 -p 500 -n $N -R 80
load_off
echo MATRIX_DONE
