#!/bin/bash
# Board day: measure the round trip of the four signalling methods and record it. Runs on the PC.
#
#   experiments/amp-m08/run_m08.sh [--no-install] [--check] [--no-load]
#
# Needs: the M08 payload running on hart 4 (see deploy_payload.sh, then reboot), ssh key access to
# root@board, the FlashPro5 serial port of the hart 4 console (optional but recommended), and the board
# binary built with `make -C experiments/amp-m08 board`.
#
# What it does on the board: copies one binary to /root, runs it, reads two files back. The program writes
# one page of the non-cached shared window (physical 0xC9E00000, 4 KiB, no other software uses it) and
# writes the CTRL register of latency channel 1 of the fabric (the one hart 4 owns). It does NOT write
# the SD card, the boot configuration, the eNVM or the fabric configuration, and it needs no root file
# system change. Every wait has a time limit, so the script always ends.
#
# Passes (each one writes a text report, JSON lines and a CSV of every trial):
#   idle    the measurement cpu alone busy (the other cpus idle)
#   loaded  the other two Linux cpus run a busy loop (`yes > /dev/null`) while the measurement runs
#
# Environment overrides: BOARD (root@pfdaq-board), UART, OUT, CPU (measurement cpu, default 2), N (trials, 2000).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BOARD=${BOARD:-root@pfdaq-board}
UART=${UART:-$(ls /dev/serial/by-id/usb-Microsemi_Embedded_FlashPro5_*-if03-port0 2>/dev/null | head -1)}
OUT=${OUT:-$ROOT/logs/m5/m08/$(date +%Y%m%d-%H%M%S)}
BIN_LOCAL=${BIN_LOCAL:-$HERE/build/board/m08_rtt}
RBIN=${RBIN:-/root/m08_rtt}
CPU=${CPU:-2}
N=${N:-2000}
MAXSEC=${MAXSEC:-600}                # hard limit for the console capture
RUN_TIMEOUT=${RUN_TIMEOUT:-300}      # hard limit for one measurement pass on the board
EXTRA=${EXTRA:-}                     # host test: "--sim"

INSTALL=1; CHECK_ONLY=0; LOAD=1
for a in "$@"; do
    case $a in
        --no-install) INSTALL=0 ;;
        --check) CHECK_ONLY=1 ;;
        --no-load) LOAD=0 ;;
        *) echo "usage: $0 [--no-install] [--check] [--no-load]" >&2; exit 2 ;;
    esac
done

if [ -n "${LOCAL_TEST:-}" ]; then            # host test: "the board" is this PC, no ssh, no scp
    remote() { timeout "${2:-60}" bash -c "$1"; }
    push() { :; }
    RBIN=$BIN_LOCAL
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
pids=()
cleanup() {
    for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    [ -n "${LOCAL_TEST:-}" ] || remote "pkill -x yes 2>/dev/null; true" 10 >/dev/null 2>&1
}
trap cleanup EXIT

: >"$INFO"
note "output directory $OUT"
note "board $BOARD, hart 4 console ${UART:-none (no capture)}, measurement cpu $CPU, $N trials per method"

# ---- preflight ---------------------------------------------------------------------------------
PRE="echo ssh ok; uname -r; cat /sys/devices/system/cpu/online; for r in /sys/class/remoteproc/*; do echo \$r \$(cat \$r/state); done; ls /sys/bus/rpmsg/devices | grep pfdaq-supervisor; ls /dev/udmabuf-ddr-nc0; for u in /sys/class/uio/uio*/name; do cat \$u; done"
[ -n "${LOCAL_TEST:-}" ] && PRE="echo local test; uname -r"
remote "$PRE" 15 >"$OUT/preflight.txt" 2>&1 \
    || { cat "$OUT/preflight.txt" >&2; echo "preflight failed: board not reachable, or no pfdaq-supervisor channel / udmabuf device (is the M08 payload running?)" >&2; exit 1; }
tee -a "$INFO" <"$OUT/preflight.txt"
if [ -z "${LOCAL_TEST:-}" ] && [ $INSTALL = 1 ]; then
    [ -x "$BIN_LOCAL" ] || { echo "$BIN_LOCAL missing: run 'make -C experiments/amp-m08 board'" >&2; exit 1; }
    push "$BIN_LOCAL" || { echo "scp failed" >&2; exit 1; }
    remote "chmod +x $RBIN; sha256sum $RBIN | cut -d' ' -f1" 15 >"$OUT/remote-sha.txt" 2>&1
    L=$(sha256sum "$BIN_LOCAL" | cut -d' ' -f1)
    [ "$(tail -1 "$OUT/remote-sha.txt")" = "$L" ] || { echo "binary on the board differs from the local one" >&2; exit 1; }
    note "installed $RBIN ($L)"
fi

note "probe: does hart 4 run the M08 variant, and is latency channel 1 idle?"
remote "$RBIN $EXTRA --probe -c $CPU" 30 >"$OUT/probe.txt" 2>&1 \
    || { cat "$OUT/probe.txt" >&2; echo "probe failed: the M08 payload is not running on hart 4, or the shared window is not what the program expects" >&2; exit 1; }
tee -a "$INFO" <"$OUT/probe.txt"
[ $CHECK_ONLY = 1 ] && { note "preflight only: done"; trap - EXIT; exit 0; }

# ---- console capture ---------------------------------------------------------------------------
if [ -n "$UART" ] && [ -e "$UART" ] && [ -z "${LOCAL_TEST:-}" ]; then
    python3 "$HERE/../amp-station/capture/capture_uart.py" "$UART" "$OUT/uart.raw" --max-seconds "$MAXSEC" 2>"$OUT/uart.err" &
    pids+=($!)
    sleep 1
    kill -0 "${pids[0]}" 2>/dev/null || { cat "$OUT/uart.err" >&2; warn "UART capture did not start: no hart 4 console record"; pids=(); }
else
    note "no hart 4 console capture (set UART=...)"
fi

fail=0
others() { for c in 0 1 2; do [ "$c" != "$CPU" ] && echo "$c"; done; }
run_pass() {    # run_pass <name> <load: 0|1>
    local name=$1 load=$2 c
    note "pass $name"
    if [ "$load" = 1 ] && [ -z "${LOCAL_TEST:-}" ]; then
        for c in $(others); do remote "nohup taskset -c $c yes > /dev/null 2>&1 &" 10 >/dev/null 2>&1; done
        sleep 1
    fi
    remote "$RBIN $EXTRA -c $CPU -n $N --csv /dev/shm/m08-$name.csv --json /dev/shm/m08-$name.json" "$RUN_TIMEOUT" >"$OUT/report-$name.txt" 2>&1 \
        || { warn "pass $name did not finish cleanly (see $OUT/report-$name.txt)"; fail=1; }
    [ "$load" = 1 ] && [ -z "${LOCAL_TEST:-}" ] && remote "pkill -x yes; true" 10 >/dev/null 2>&1
    remote "cat /dev/shm/m08-$name.csv" 30 >"$OUT/trials-$name.csv" 2>/dev/null
    remote "cat /dev/shm/m08-$name.json" 30 >"$OUT/summary-$name.jsonl" 2>/dev/null
    cat "$OUT/report-$name.txt" | sed -n '/^round trip/,$p' | head -22 | tee -a "$INFO"
}
run_pass idle 0
[ $LOAD = 1 ] && run_pass loaded 1

# ---- hart 4 still alive afterwards? ------------------------------------------------------------
note "after the passes: RPMsg ping and QUERY again"
remote "$RBIN $EXTRA -c $CPU -n 20 -w 2 -m rpmsg-ping,rpmsg-query" 60 >"$OUT/after.txt" 2>&1 \
    && grep -q "rpmsg-ping *20 *0 *0" "$OUT/after.txt" && note "hart 4 answers RPMsg after the passes: 20 of 20 pings" \
    || { warn "hart 4 did not answer 20 of 20 pings after the passes: see $OUT/after.txt and the console"; fail=1; }

sleep 1
for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null; done
pids=()
if [ -f "$OUT/uart.raw" ]; then
    grep -E "\[m08\]|\[pfdaq-sup\]" "$OUT/uart.raw" | cut -f2- >"$OUT/console-m08-lines.txt"
    grep -q "\[m08\] shared block at" "$OUT/uart.raw" && note "console: shared block line seen" || { warn "console: no '[m08] shared block at' line"; fail=1; }
    grep -q "\[m08\] poll ended" "$OUT/uart.raw" && note "console: poll ended line seen" || { warn "console: no '[m08] poll ended' line"; fail=1; }
fi
note "warnings: $WARNINGS"
note "record: $OUT"
[ $fail = 0 ] && [ $WARNINGS = 0 ]
