#!/bin/bash
# Board day: read hart 4's PMP, run the one deliberate-violation test, check that the supervisor still answers.
# Runs on the PC.
#
#   experiments/amp-m09/run_m09.sh [--no-install] [--check] [--yes]
#
# Needs: the M09 payload running on hart 4 (see deploy_payload.sh, then reboot), ssh key access to
# root@board, the FlashPro5 serial port of the hart 4 console (strongly recommended: the evidence is printed
# there), and the board binary built with `make -C experiments/amp-m09 board`.
#
# What it does on the board: copies one small binary to /root and runs it three times (report, run, ping).
# What `run` makes hart 4 do: it writes its own PMP registers for entries 0..3 WITHOUT the lock bit (after
# refusing if any of them is locked), enters user mode for a few microseconds with interrupts masked, loads from
# a Linux DDR address and from the L2 scratchpad of the boot monitor, stores into a read-only page of its own,
# catches the three access faults, and puts the PMP registers back. It stores nothing to memory that Linux or
# the boot monitor uses. It never sets the PMP lock bit, and the script writes nothing to the SD card, the eNVM,
# the boot configuration or the fabric. If hart 4 does not come back, see the risk list of the page (reboot,
# then deploy_payload.sh restore).
#
# Every wait has a time limit; the script always ends. Exit status: 0 the test ran and caught all three violations;
# 3 hart 4 REFUSED the test because a locked PMP entry blocks it (the answer the sources predict for this board:
# not an error, nothing was changed; the report says why); 1 anything else (no answer, wrong result, warnings).
# Environment overrides: BOARD (root@pfdaq-board), UART, OUT.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BOARD=${BOARD:-root@pfdaq-board}
UART=${UART:-$(ls /dev/serial/by-id/usb-Microsemi_Embedded_FlashPro5_*-if03-port0 2>/dev/null | head -1)}
OUT=${OUT:-$ROOT/logs/m5/m09/$(date +%Y%m%d-%H%M%S)}
BIN_LOCAL=${BIN_LOCAL:-$HERE/build/board/m09_ctl}
RBIN=${RBIN:-/root/m09_ctl}
LINKOPT=${LINKOPT:-}                  # host test: "--link unix:PATH"
RUN_TIMEOUT_MS=${RUN_TIMEOUT_MS:-10000}
MAXSEC=${MAXSEC:-300}

INSTALL=1; CHECK_ONLY=0; YES=0
for a in "$@"; do
    case $a in
        --no-install) INSTALL=0 ;;
        --check) CHECK_ONLY=1 ;;
        --yes) YES=1 ;;
        *) echo "usage: $0 [--no-install] [--check] [--yes]" >&2; exit 2 ;;
    esac
done

if [ -n "${LOCAL_TEST:-}" ]; then
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
cleanup() { for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

: >"$INFO"
note "output directory $OUT"
note "board $BOARD, hart 4 console ${UART:-none (no capture)}"

# ---- preflight ---------------------------------------------------------------------------------
PRE="echo ssh ok; uname -r; for r in /sys/class/remoteproc/*; do echo \$r \$(cat \$r/state); done; ls /sys/bus/rpmsg/devices | grep pfdaq-supervisor"
[ -n "${LOCAL_TEST:-}" ] && PRE="echo local test; uname -r"
remote "$PRE" 15 >"$OUT/preflight.txt" 2>&1 \
    || { cat "$OUT/preflight.txt" >&2; echo "preflight failed: board not reachable, or no pfdaq-supervisor channel (is the M09 payload running?)" >&2; exit 1; }
tee -a "$INFO" <"$OUT/preflight.txt"
if [ -z "${LOCAL_TEST:-}" ] && [ $INSTALL = 1 ]; then
    [ -x "$BIN_LOCAL" ] || { echo "$BIN_LOCAL missing: run 'make -C experiments/amp-m09 board'" >&2; exit 1; }
    push "$BIN_LOCAL" || { echo "scp failed" >&2; exit 1; }
    remote "chmod +x $RBIN; sha256sum $RBIN | cut -d' ' -f1" 15 >"$OUT/remote-sha.txt" 2>&1
    L=$(sha256sum "$BIN_LOCAL" | cut -d' ' -f1)
    [ "$(tail -1 "$OUT/remote-sha.txt")" = "$L" ] || { echo "binary on the board differs from the local one" >&2; exit 1; }
    note "installed $RBIN ($L)"
fi

note "step 1: hart 4 answers a ping before anything is changed"
remote "$RBIN $LINKOPT ping 3" 30 >"$OUT/ping-before.txt" 2>&1 \
    || { cat "$OUT/ping-before.txt" >&2; echo "hart 4 does not answer: stop here, nothing was changed" >&2; exit 1; }
tee -a "$INFO" <"$OUT/ping-before.txt"
[ $CHECK_ONLY = 1 ] && { note "preflight only: done"; trap - EXIT; exit 0; }

# ---- console capture ---------------------------------------------------------------------------
if [ -n "$UART" ] && [ -e "$UART" ] && [ -z "${LOCAL_TEST:-}" ]; then
    python3 "$HERE/../amp-station/capture/capture_uart.py" "$UART" "$OUT/uart.raw" --max-seconds "$MAXSEC" 2>"$OUT/uart.err" &
    pids+=($!)
    sleep 1
    kill -0 "${pids[0]}" 2>/dev/null || { cat "$OUT/uart.err" >&2; warn "UART capture did not start: no hart 4 console record"; pids=(); }
else
    note "no hart 4 console capture (set UART=...): the evidence lines of the test would be lost"
fi

fail=0
note "step 2: PMP report (read only)"
remote "$RBIN $LINKOPT report" 30 >"$OUT/report.txt" 2>&1 || { warn "REPORT got no answer"; fail=1; }
tee -a "$INFO" <"$OUT/report.txt"

if [ $YES = 0 ] && [ -z "${LOCAL_TEST:-}" ]; then
    cat <<MSG

About to ask hart 4 to run the user-mode test (module M09, risk list). It changes only hart 4's own PMP
registers (restored afterwards, never locked) and runs a few microseconds in user mode with interrupts masked.
If hart 4 does not answer, reboot the board and, if needed, restore the proven payload with deploy_payload.sh.
Type RUN-M09 to go on:
MSG
    read -r a
    [ "$a" = "RUN-M09" ] || { note "not confirmed: nothing run"; exit 1; }
fi

note "step 3: RUN (answer within ${RUN_TIMEOUT_MS} ms, or the hang case)"
remote "$RBIN $LINKOPT run --timeout-ms $RUN_TIMEOUT_MS" $((RUN_TIMEOUT_MS / 1000 + 20)) >"$OUT/run.txt" 2>&1
RC=$?
tee -a "$INFO" <"$OUT/run.txt"
HUNG=0; REFUSED=0
grep -q "NO ANSWER to RUN" "$OUT/run.txt" && HUNG=1
grep -q "run: result REFUSED" "$OUT/run.txt" && REFUSED=1
[ $HUNG = 1 ] && { warn "hart 4 did not answer RUN: see the risk list; nothing more is run"; fail=1; }

if [ $HUNG = 0 ]; then
    note "step 4: does the supervisor still answer after the test? 20 pings"
    remote "$RBIN $LINKOPT ping 20" 60 >"$OUT/ping-after.txt" 2>&1 \
        && grep -q '"answered": 20' "$OUT/ping-after.txt" && note "20 of 20 pings answered after the test" \
        || { warn "the supervisor did not answer 20 of 20 pings after the test (see $OUT/ping-after.txt)"; fail=1; }
    note "step 5: report again: the PMP must read as before"
    remote "$RBIN $LINKOPT report" 30 >"$OUT/report-after.txt" 2>&1 || { warn "REPORT after the test got no answer"; fail=1; }
    tee -a "$INFO" <"$OUT/report-after.txt"
    cmp -s <(sed 's/^report: //' "$OUT/report.txt" | grep '^{' ) <(sed 's/^report: //' "$OUT/report-after.txt" | grep '^{') \
        && note "the counts of the report are the same before and after" || { warn "the report differs before and after the test"; fail=1; }
fi

sleep 1
for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null; done
pids=()
if [ -f "$OUT/uart.raw" ]; then
    grep -E "\[m09\]" "$OUT/uart.raw" | cut -f2- >"$OUT/console-m09-lines.txt"
    for pat in "AMP demo variant M09" "RUN requested" "installed and read back|REFUSED" "result [A-Z_]+:" ; do
        grep -Eq "$pat" "$OUT/console-m09-lines.txt" && note "console: '$pat' seen" || { warn "console: no line matching '$pat'"; fail=1; }
    done
fi
note "result of the run: exit status $RC of m09_ctl run (0 = OK; read run.txt and the console lines for anything else)"
note "warnings: $WARNINGS"
note "record: $OUT"
if [ $REFUSED = 1 ]; then
    [ $fail = 0 ] && [ $WARNINGS = 0 ] || exit 1
    note "REFUSED: a locked PMP entry of this hart blocks the test; nothing was installed or run (exit status 3)"
    exit 3
fi
[ $fail = 0 ] && [ $WARNINGS = 0 ]
