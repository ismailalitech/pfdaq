#!/bin/bash
# Run the whole board-day script on the PC against stand-ins ("the board" is this PC: LOCAL_TEST=1,
# no ssh; hart 4 is fake_hart4 and its stdout file is the "UART"; the data source is simulated).
# Checks that the script finishes, every wait was satisfied, and that the recording it writes is in the
# replay format with the three panes and the expected events. About 45 s.
HERE=$(cd "$(dirname "$0")/.." && pwd)
H=${1:-$HERE/build}/host
W=${TMPDIR:-/tmp}/amp-station-rundemo-$$
mkdir -p "$W/out"
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
[ -x "$H/station" ] && [ -x "$H/fake_hart4" ] || make -C "$HERE" host OUT="${1:-$HERE/build}" >/dev/null || exit 1

"$H/fake_hart4" --sock "$W/hart4.sock" --fabsim "$W/fabsim" >"$W/uart.txt" 2>&1 & P=$!
sleep 0.5
export LOCAL_TEST=1 UART=$W/uart.txt OUT=$W/out BIN_LOCAL=$H/station
export RBIN=$H/station RDIR=$W/st RSTART="$HERE/board/start_station.sh --inject-slots 20 --lim-drops 1000000 --max-lag 1000000" RSTOP=$HERE/board/stop_station.sh
export LINKOPT="--link unix:$W/hart4.sock"
export STATION_DIR=$W/st STATION_BIN=$H/station STATION_LINK=unix:$W/hart4.sock STATION_FABRIC=sim STATION_FABSIM=$W/fabsim
export T_NORMAL=4 T_SAFE_SHOW=2 T_RESUME=3 T_STOPPED=2 T_FINAL=2 MAXSEC=120
"$HERE/run_demo.sh" >"$W/run.log" 2>&1
rc=$?
kill -TERM $P 2>/dev/null; wait $P 2>/dev/null
tail -12 "$W/run.log"
[ $rc = 0 ] && ok "run_demo.sh finished with status 0 (every wait satisfied)" || bad "run_demo.sh status $rc"
R=$W/out/recording.jsonl
[ -s "$R" ] && python3 "$HERE/capture/recording.py" check "$R" >/dev/null && ok "recording is valid replay format ($(wc -l <"$R") lines)" || bad "recording missing or invalid"
for pat in '"pane": 0, "text": "\$ taskset -c 0 ' '"pane": 0, "text": ".*core 4 speaks' '"pane": 0, "text": ".*TRIP reported by hart 4: reason BAD_FRAMES' \
           '"pane": 2, "text": "\[station\] TRIP #1: BAD_FRAMES' '"pane": 2, "text": "\[station\] ALARM: no message' '"pane": 2, "text": "\[station\] TRIP #2: DEADLINE' \
           '"pane": 1, "text": "\$ kill -STOP' '"pane": 1, "text": "\[ *[0-9.]* cpu0 acq\] run [0-9]* ended by ABORT' '"pane": 1, "text": "\[ *[0-9.]* cpu1 ana\] RESET' \
           '"pane": 0, "text": ".* SILENT [0-9.]+s' '"pane": 2, "text": "\[station\] DISARM from' '"pane": 0, "text": ".*summary: frames'; do
    grep -Eq "$pat" "$R" && ok "recording has: ${pat#*text\": }" || bad "recording lacks: $pat"
done
cp "$R" "$HERE/build/last-run-demo.jsonl" 2>/dev/null
[ $fail = 0 ] && rm -rf "$W" || echo "kept $W for inspection"
[ $fail = 0 ] && echo "run_demo test passed" || echo "$fail failure(s)"
exit $fail
