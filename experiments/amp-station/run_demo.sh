#!/bin/bash
# Board day: run the four-core station scenario on the board and record it. Runs on the PC.
#
#   experiments/amp-station/run_demo.sh [--no-install] [--check]
#
# Needs: the demo payload running on hart 4 (see deploy_payload.sh), ssh key access to root@board,
# the FlashPro5 USB serial port of the hart 4 console, and the board binary built with `make board`.
# It does NOT write anything to the SD card, the boot configuration or the fabric. On the board it
# only copies four files to /root and uses /dev/shm.
#
# What it records (three panes, real PC timestamps, one JSON line per console line):
#   pane 0  Linux operator session: roll call, start, dashboard of the telemetry process (cpu 2),
#           and the commands the operator types
#   pane 1  Linux logs of the acquisition (cpu 0) and analysis (cpu 1) processes
#   pane 2  hart 4 console (UART, interface 3)
# Every wait has a time limit; a wait that runs out is noted in $OUT/run-info.txt and the script goes on,
# so it always ends. Exit status 0 only if every expected event happened.
#
# Environment overrides: BOARD (root@pfdaq-board), UART, OUT, and the timing variables below.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BOARD=${BOARD:-root@pfdaq-board}
UART=${UART:-$(ls /dev/serial/by-id/usb-Microsemi_Embedded_FlashPro5_*-if03-port0 2>/dev/null | head -1)}
OUT=${OUT:-$ROOT/logs/m5/station/$(date +%Y%m%d-%H%M%S)}
BIN_LOCAL=${BIN_LOCAL:-$HERE/build/board/station}
RBIN=${RBIN:-/root/station}                 # where things live on the board
RDIR=${RDIR:-/dev/shm/station}
RSTART=${RSTART:-/root/start_station.sh}
RSTOP=${RSTOP:-/root/stop_station.sh}
LINKOPT=${LINKOPT:-}                        # host test: "--link unix:PATH"
# scenario timing, seconds
T_NORMAL=${T_NORMAL:-15}                    # normal operation before fault A
T_SAFE_SHOW=${T_SAFE_SHOW:-5}               # SAFE state left on screen before the operator resets
T_RESUME=${T_RESUME:-8}                     # running again before the next step
T_FAULT_WAIT=${T_FAULT_WAIT:-12}            # longest wait for a trip after a fault
T_STOPPED=${T_STOPPED:-4}                   # fault B: time with the analysis stopped after the trip
T_FINAL=${T_FINAL:-6}                       # running before the shutdown
MAXSEC=${MAXSEC:-240}                       # hard limit for every capture process

INSTALL=1; CHECK_ONLY=0
for a in "$@"; do
    case $a in
        --no-install) INSTALL=0 ;;
        --check) CHECK_ONLY=1 ;;
        *) echo "usage: $0 [--no-install] [--check]" >&2; exit 2 ;;
    esac
done

if [ -n "${LOCAL_TEST:-}" ]; then           # host test: "the board" is this PC, no ssh, no scp
    remote() { timeout "${2:-60}" bash -c "$1"; }
    push() { :; }
else
    SSHO="-o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
    remote() { timeout "${2:-60}" ssh $SSHO "$BOARD" "$1"; }
    push() { scp -q $SSHO "$@" "$BOARD:/root/"; }
fi

mkdir -p "$OUT" || exit 1
INFO=$OUT/run-info.txt
WARNINGS=0
warn() { echo "WARN: $*" | tee -a "$INFO" >&2; WARNINGS=$((WARNINGS+1)); }
note() { echo "[$(date +%H:%M:%S)] $*"; echo "[$(date +%H:%M:%S)] $*" >>"$INFO"; }
now() { python3 -c 'import time; print("%.6f" % time.time())'; }
mark() {    # mark <pane> <text>: a command the script issues, shown as a "$ ..." line of that pane
    printf '%s\t%s\t%s\n' "$(now)" "$1" "$2" >>"$OUT/ops.raw"
}
wait_for() {   # wait_for <raw file> <regex> <seconds> <what>
    local n=$(( $3 * 10 ))
    while [ $n -gt 0 ]; do
        grep -Eq "$2" "$1" 2>/dev/null && return 0
        sleep 0.1; n=$((n-1))
    done
    warn "timed out after $3 s waiting for: $4"
    return 1
}
pids=()
cleanup() {
    for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    remote "$RSTOP" 30 >>"$OUT/cleanup.log" 2>&1
}
trap cleanup EXIT

: >"$INFO"
note "output directory $OUT"
note "board $BOARD, hart 4 console $UART"

# ---- preflight ---------------------------------------------------------------------------------
[ -n "${LOCAL_TEST:-}" ] || [ -e "$UART" ] || { echo "hart 4 console $UART not found" >&2; exit 1; }
PRE="echo ssh ok; uname -r; test -d /sys/bus/rpmsg/devices && ls /sys/bus/rpmsg/devices | grep pfdaq-supervisor"
[ -n "${LOCAL_TEST:-}" ] && PRE="echo local test; uname -r"
remote "$PRE" 15 >"$OUT/preflight.txt" 2>&1 \
    || { cat "$OUT/preflight.txt" >&2; echo "preflight failed: board not reachable, or no pfdaq-supervisor channel (is the demo payload running?)" >&2; exit 1; }
cat "$OUT/preflight.txt" | tee -a "$INFO"
if [ -z "${LOCAL_TEST:-}" ] && [ $INSTALL = 1 ]; then
    [ -x "$BIN_LOCAL" ] || { echo "$BIN_LOCAL missing: run 'make -C experiments/amp-station board'" >&2; exit 1; }
    push "$BIN_LOCAL" "$HERE/board/start_station.sh" "$HERE/board/stop_station.sh" || { echo "scp failed" >&2; exit 1; }
    remote "chmod +x $RBIN $RSTART $RSTOP; sha256sum $RBIN | cut -d' ' -f1" 15 >"$OUT/remote-sha.txt" 2>&1
    L=$(sha256sum "$BIN_LOCAL" | cut -d' ' -f1)
    [ "$(tail -1 "$OUT/remote-sha.txt")" = "$L" ] || { echo "binary on the board differs from the local one" >&2; exit 1; }
    note "installed /root/station ($L), start_station.sh, stop_station.sh"
fi
[ $CHECK_ONLY = 1 ] && { note "preflight only: done"; trap - EXIT; exit 0; }

# ---- captures ----------------------------------------------------------------------------------
python3 "$HERE/capture/capture_uart.py" "$UART" "$OUT/uart.raw" --max-seconds "$MAXSEC" 2>"$OUT/uart.err" &
pids+=($!)
sleep 1
kill -0 "${pids[0]}" 2>/dev/null || { cat "$OUT/uart.err" >&2; echo "UART capture did not start" >&2; exit 1; }
: >"$OUT/p0.raw"; : >"$OUT/p1.raw"; : >"$OUT/ops.raw"
fail=0

run0() {    # run0 <command>: run on the board, show "$ command" and its output in pane 0
    mark 0 "\$ $1"
    remote "$1" 60 2>&1 | python3 "$HERE/capture/stamp.py" "$OUT/p0.raw" --max-seconds 100
}

# ---- 1 roll call -------------------------------------------------------------------------------
note "1 roll call: HELLO from cpu 0, 1, 2, then hart 4 speaks"
# taskset is used when the board has it; the program also pins itself with sched_setaffinity either way
TS=""
remote "command -v taskset" 10 >/dev/null 2>&1 && TS=1
for c in 0 1 2; do
    run0 "${TS:+taskset -c $c }$RBIN hello --cpu $c --seq 1 $LINKOPT"
    sleep 1
done
run0 "$RBIN listen $LINKOPT"
wait_for "$OUT/p0.raw" "core 4 speaks" 8 "hart 4 speaks to Linux" || fail=1
sleep 1

# ---- 2 start the station -----------------------------------------------------------------------
note "2 start the station: three processes on cpu 0, 1, 2"
run0 "$RSTART"
remote "tail -q -n +1 -F $RDIR/tel.log" $((MAXSEC)) 2>&1 | python3 "$HERE/capture/stamp.py" "$OUT/p0.raw" --max-seconds "$MAXSEC" &
pids+=($!)
remote "tail -q -n +1 -F $RDIR/acq.log $RDIR/ana.log" $((MAXSEC)) 2>&1 | python3 "$HERE/capture/stamp.py" "$OUT/p1.raw" --max-seconds "$MAXSEC" &
pids+=($!)
wait_for "$OUT/p0.raw" "\] frames [0-9]+" 15 "first dashboard line" || fail=1
note "normal operation for $T_NORMAL s"
sleep "$T_NORMAL"

# ---- 3 fault A ---------------------------------------------------------------------------------
note "3 fault A: the fabric corrupts the CRC of one frame in every injection run"
mark 0 "\$ echo inject crc > $RDIR/cmd"
remote "echo inject crc > $RDIR/cmd" 10
wait_for "$OUT/uart.raw" "TRIP #1" "$T_FAULT_WAIT" "hart 4 trip #1" || fail=1
wait_for "$OUT/p0.raw" "TRIP reported by hart 4" 5 "trip reported on the dashboard" || fail=1
note "SAFE state on screen for $T_SAFE_SHOW s"
sleep "$T_SAFE_SHOW"
mark 0 "\$ echo reset > $RDIR/cmd"
remote "echo reset > $RDIR/cmd" 10
wait_for "$OUT/p0.raw" "reset accepted by hart 4" 6 "reset accepted" || fail=1
note "running again for $T_RESUME s"
sleep "$T_RESUME"

# ---- 4 fault B ---------------------------------------------------------------------------------
note "4 fault B: the analysis process is stopped (SIGSTOP)"
ANA=$(remote "cat $RDIR/ana.pid" 10 2>/dev/null | tr -dc '0-9')
if [ -n "$ANA" ]; then
    mark 1 "\$ kill -STOP $ANA     # the analysis process (cpu 1) freezes"
    remote "kill -STOP $ANA" 10
    wait_for "$OUT/uart.raw" "TRIP #2" "$T_FAULT_WAIT" "hart 4 deadline trip #2" || fail=1
    wait_for "$OUT/p0.raw" "SILENT [0-9.]+s" 8 "telemetry notices the silent analysis" || fail=1
    sleep "$T_STOPPED"
    mark 1 "\$ kill -CONT $ANA     # the analysis process continues"
    remote "kill -CONT $ANA" 10
    wait_for "$OUT/p0.raw" "reason DEADLINE" 8 "deadline trip reported on the dashboard" || fail=1
    sleep 2
    mark 0 "\$ echo reset > $RDIR/cmd"
    remote "echo reset > $RDIR/cmd" 10
    wait_for "$OUT/p0.raw" "trips so far 2" 6 "second reset accepted" || fail=1
else
    warn "no analysis pid found: fault B skipped"; fail=1
fi
note "running again for $T_FINAL s"
sleep "$T_FINAL"

# ---- 5 shutdown --------------------------------------------------------------------------------
note "5 orderly shutdown"
run0 "$RSTOP"
sleep 2
wait_for "$OUT/uart.raw" "DISARM from" 4 "hart 4 DISARM summary" || fail=1
for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null; done
pids=()
sleep 1

# ---- recording ---------------------------------------------------------------------------------
python3 "$HERE/capture/recording.py" merge -o "$OUT/recording.jsonl" --pane 0 "$OUT/p0.raw" --pane 1 "$OUT/p1.raw" --pane 2 "$OUT/uart.raw" --ops "$OUT/ops.raw" | tee -a "$INFO"
remote "cat $RDIR/telemetry.csv" 15 >"$OUT/telemetry.csv" 2>/dev/null
for f in acq ana tel; do remote "cat $RDIR/$f.log" 15 >"$OUT/$f.log" 2>/dev/null; done
note "warnings: $WARNINGS"
note "recording: $OUT/recording.jsonl"
note "to use it: cp $OUT/recording.jsonl $ROOT/docs/diagrams/recordings/m10-station.jsonl, then python3 tools/gen_diagrams.py"
[ $fail = 0 ] && [ $WARNINGS = 0 ]
