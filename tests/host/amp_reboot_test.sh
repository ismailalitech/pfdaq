#!/bin/bash
# PC side. Warm-reboots the board N times over SSH and, per cycle, checks:
#   - Linux came back, remoteproc is attached
#   - the FreeRTOS supervisor announced its RPMsg channel (i.e. context B really started)
#   - a supervised capture (fabric -> DMA -> Linux -> RPMsg -> FreeRTOS) passes
# usage: amp_reboot_test.sh <N> <out.csv> [board_address] [freertos_console_log]
# The board address can also be given in PFDAQ_BOARD.
N=${1:-10}; OUT=${2:-/dev/stdout}; IP=${3:-${PFDAQ_BOARD:?board address: pass it as the third argument or set PFDAQ_BOARD}}; CONLOG=$4
S="ssh -o ConnectTimeout=4 -o BatchMode=yes root@$IP"
echo "cycle,up_s,remoteproc,sup_channel,capture_pass,sup_pass,freertos_console_bytes" > "$OUT"
for i in $(seq 1 $N); do
    c0=0; [ -n "$CONLOG" ] && c0=$(stat -c %s "$CONLOG")
    t0=$(date +%s.%N)
    $S reboot >/dev/null 2>&1
    sleep 20
    up=""
    for k in $(seq 1 40); do $S true 2>/dev/null && { up=$(echo "$(date +%s.%N) - $t0" | bc); break; }; sleep 2; done
    if [ -z "$up" ]; then echo "$i,TIMEOUT,,,,," >> "$OUT"; continue; fi
    sleep 3   # FreeRTOS announces its channel after Linux reports the link up
    rp=$($S cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)
    ch=$($S 'ls /sys/bus/rpmsg/devices/ | grep -c pfdaq-supervisor' 2>/dev/null)
    js=$($S 'pfdaq_cap -r -w 252 -f 1024 -n 8' 2>/dev/null)
    cp=$(echo "$js" | grep -o '^{"pass": [a-z]*' | grep -o '[a-z]*$')
    sp=$(echo "$js" | grep -o '"supervisor": {"pass": [a-z]*' | grep -o '[a-z]*$')
    cb=""; [ -n "$CONLOG" ] && cb=$(( $(stat -c %s "$CONLOG") - c0 ))
    echo "$i,$up,$rp,$ch,${cp:-none},${sp:-none},$cb" >> "$OUT"
done
echo "# supervisor channel present: $(awk -F, 'NR>1 && $4==1' "$OUT" | wc -l)/$N, full pass: $(awk -F, 'NR>1 && $5=="true" && $6=="true"' "$OUT" | wc -l)/$N" >> "$OUT"
