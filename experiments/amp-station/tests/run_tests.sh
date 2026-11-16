#!/bin/bash
# Host tests of the station, no board involved.
#   tests/run_tests.sh [build-dir]
# 1. unit tests (test_units): ring with producer and consumer threads, message layout, CRC, features,
#    the protection logic of hart 4 (pfdaq_station.h) driven message by message
# 2. model_check.py: frames of the C model against an independent Python model (zlib CRC)
# 3. end to end: the three station processes, the simulated data source and fake_hart4 (the real
#    pfdaq_station.h behind a socket instead of RPMsg) run the recorded scenario in about 45 s:
#    normal operation, fault A (CRC faults -> trip -> reset), fault B (analysis stopped with SIGSTOP ->
#    deadline trip -> SIGCONT -> reset), orderly shutdown.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$HERE/build}
H=$OUT/host
FW=$HERE/../../firmware/u54_4-supervisor
WORK=${TMPDIR:-/tmp}/amp-station-test-$$
fail=0
ok()   { echo "ok   $*"; }
bad()  { echo "FAIL $*"; fail=$((fail+1)); }

mkdir -p "$H" "$WORK"
gcc -O2 -Wall -Wextra -I"$HERE" -I"$FW" -std=gnu11 -pthread -o "$H/test_units" "$HERE/tests/test_units.c" || exit 1
[ -x "$H/station" ] && [ -x "$H/fake_hart4" ] || make -C "$HERE" host OUT="$OUT" >/dev/null || exit 1

echo "== 1 unit tests"
"$H/test_units" && ok "unit tests" || bad "unit tests"
echo "== 2 independent model"
python3 "$HERE/tests/model_check.py" "$H/test_units" && ok "model_check" || bad "model_check"

echo "== 3 end to end (simulated source, fake hart 4)"
# The PC is shared with a desktop: relax the limits that a scheduling hiccup of a few ms could hit (drops,
# sequence lag); the faults the tests inject are far above them. On the board the defaults are used.
RELAX="--lim-drops 1000000 --max-lag 1000000"
D=$WORK/run
mkdir -p "$D"
wait_for() {   # wait_for <file> <regex> <seconds>
    local n=$(( $3 * 10 ))
    while [ $n -gt 0 ]; do grep -Eq "$2" "$1" 2>/dev/null && return 0; sleep 0.1; n=$((n-1)); done
    return 1
}
count() { grep -Ec "$2" "$1" 2>/dev/null; }
frames_now() { grep -E '\] frames ' "$D/tel.log" | tail -1 | sed -E 's/.*frames ([0-9]+).*/\1/'; }

"$H/station" init --dir "$D" >/dev/null
"$H/fake_hart4" --sock "$D/hart4.sock" --fabsim "$D/fabsim" >"$D/hart4.log" 2>&1 & P_H4=$!
wait_for "$D/hart4.log" "listening" 3 || bad "fake hart 4 did not start"
# roll call first, as on the board: three hellos then a listen, each its own process
for c in 0 1 2; do "$H/station" hello --dir "$D" --link "unix:$D/hart4.sock" --seq 1 --cpu $c >"$D/hello$c.log" 2>&1; done
"$H/station" listen --dir "$D" --link "unix:$D/hart4.sock" >"$D/listen.log" 2>&1
grep -q "core 4 speaks" "$D/listen.log" && ok "roll call: listen printed the message from hart 4" || bad "roll call: no speak message"
[ "$(count "$D/hart4.log" 'HELLO #1 from hart')" = 3 ] && ok "roll call: hart 4 printed three HELLO lines" || bad "roll call: hello lines"

"$H/station" tel --dir "$D" >"$D/tel.log" 2>&1 & P_TEL=$!
"$H/station" ana --dir "$D" --link "unix:$D/hart4.sock" $RELAX >"$D/ana.log" 2>&1 & P_ANA=$!
# short injection runs (20 slots): many run boundaries, the stress case for an ABORT that arrives between two runs
"$H/station" acq --dir "$D" --fabric sim --inject-slots 20 >"$D/acq.log" 2>&1 & P_ACQ=$!

echo "-- normal operation"
sleep 6
F1=$(frames_now); sleep 2; F2=$(frames_now)
[ -n "$F1" ] && [ $((F2-F1)) -ge 1900 ] && [ $((F2-F1)) -le 2100 ] && ok "about 1000 frames/s verified ($F1 -> $F2 in 2 s)" || bad "frame rate: $F1 -> $F2"
grep -q "h4 ARMED trips 0" "$D/tel.log" && ok "dashboard shows hart 4 ARMED, no trips" || bad "dashboard state"
[ "$(grep -E '\] frames ' "$D/tel.log" | tail -1 | grep -c 'bad 0 drop 0')" = 1 ] && ok "no bad frames, no drops in normal operation" || bad "bad frames in normal operation"

echo "-- fault A: CRC faults from the fabric"
echo "inject crc" > "$D/cmd"
wait_for "$D/tel.log" "TRIP reported by hart 4: reason BAD_FRAMES" 6 && ok "fault A: trip reported to the operator (BAD_FRAMES)" || bad "fault A: no trip"
grep -q "TRIP #1: BAD_FRAMES" "$D/hart4.log" && ok "fault A: hart 4 console line TRIP #1: BAD_FRAMES" || bad "fault A: hart 4 line"
wait_for "$D/acq.log" "ended by ABORT" 3 && ok "fault A: acquisition saw the source stopped" || bad "fault A: acquisition did not see the abort"
sleep 1.5; F1=$(frames_now); sleep 2; F2=$(frames_now)
[ "$F1" = "$F2" ] && ok "fault A: no frames while SAFE ($F1)" || bad "fault A: frames still arriving ($F1 -> $F2)"
grep -q "h4 SAFE trips 1" "$D/tel.log" && ok "fault A: dashboard shows SAFE" || bad "fault A: dashboard SAFE"
echo "reset" > "$D/cmd"
wait_for "$D/tel.log" "reset accepted by hart 4" 4 && ok "fault A: operator reset accepted" || bad "fault A: reset not accepted"
sleep 3; F3=$(frames_now)
[ $((F3-F2)) -ge 2000 ] && ok "fault A: frames flow again ($F2 -> $F3)" || bad "fault A: no recovery ($F2 -> $F3)"
[ "$(count "$D/hart4.log" 'TRIP #')" = 1 ] && ok "fault A: no further trip after the reset (injection was cleared)" || bad "fault A: unexpected trip"

echo "-- fault B: analysis process stopped (SIGSTOP)"
kill -STOP "$P_ANA"
wait_for "$D/hart4.log" "TRIP #2: DEADLINE" 4 && ok "fault B: hart 4 deadline check fired (ALARM, then TRIP #2: DEADLINE)" || bad "fault B: no deadline trip"
grep -q "ALARM: no message within 500 ms" "$D/hart4.log" && ok "fault B: ALARM line printed before the trip" || bad "fault B: no ALARM line"
wait_for "$D/acq.log" "run [0-9]+ ended by ABORT.*" 3 && [ "$(count "$D/acq.log" 'ended by ABORT')" = 2 ] && ok "fault B: source stopped (second abort)" || bad "fault B: source not stopped"
wait_for "$D/tel.log" "SILENT [0-9.]+s" 4 && ok "fault B: telemetry noticed the silent analysis by itself" || bad "fault B: no silent-analysis note"
grep -E "src STOP .*SILENT" "$D/tel.log" | grep -q . && ok "fault B: the dashboard shows the source stopped while the analysis is silent" || bad "fault B: no 'src STOP' with SILENT"
kill -CONT "$P_ANA"
wait_for "$D/tel.log" "TRIP reported by hart 4: reason DEADLINE" 4 && ok "fault B: trip reported once the analysis ran again" || bad "fault B: trip not reported"
echo "reset" > "$D/cmd"
wait_for "$D/tel.log" "trips so far 2" 4 && ok "fault B: reset accepted" || bad "fault B: reset"
sleep 4; F4=$(frames_now); sleep 2; F5=$(frames_now)
[ $((F5-F4)) -ge 1900 ] && ok "fault B: frames flow again ($F4 -> $F5)" || bad "fault B: no recovery ($F4 -> $F5)"
[ "$(count "$D/hart4.log" 'TRIP #')" = 2 ] && ok "fault B: exactly two trips in total" || bad "fault B: trip count $(count "$D/hart4.log" 'TRIP #')"
grep -E 'ovr [1-9]' "$D/tel.log" | grep -q . && ok "fault B: acquisition kept running and dropped into the full ring (overruns > 0)" || bad "fault B: no overruns counted"

echo "-- shutdown"
kill -TERM "$P_ACQ" "$P_ANA" "$P_TEL"
for p in $P_ACQ $P_ANA $P_TEL; do wait $p; rc=$?; [ $rc = 0 ] && ok "process $p exited 0" || bad "process $p exit status $rc"; done
grep -q "summary: frames" "$D/tel.log" && ok "telemetry printed its summary" || bad "no summary"
grep -q "DISARM from ARMED" "$D/hart4.log" && ok "hart 4 got DISARM and printed its summary" || bad "no DISARM"
kill -TERM $P_H4; wait $P_H4 2>/dev/null
[ -s "$D/telemetry.csv" ] && ok "telemetry.csv has $(wc -l < "$D/telemetry.csv") lines" || bad "no csv"

echo "== 4 board start and stop scripts, run on the PC against the same stand-ins"
SD=$WORK/scripts; mkdir -p "$SD"
"$H/fake_hart4" --sock "$SD/hart4.sock" --fabsim "$SD/fabsim" >"$SD/hart4.log" 2>&1 & P_H4=$!
wait_for "$SD/hart4.log" "listening" 3
export STATION_DIR=$SD/st STATION_BIN=$H/station STATION_LINK=unix:$SD/hart4.sock STATION_FABRIC=sim STATION_FABSIM=$SD/fabsim
OUTS=$("$HERE/board/start_station.sh" $RELAX 2>&1); rc=$?
[ $rc = 0 ] && echo "$OUTS" | grep -q "station running" && ok "start_station.sh: $OUTS" || bad "start_station.sh rc=$rc: $OUTS"
"$HERE/board/start_station.sh" >/dev/null 2>&1 && bad "a second start_station.sh should refuse" || ok "second start_station.sh refused while pid files exist"
sleep 3; echo "status" > "$STATION_DIR/cmd"; sleep 0.5
grep -q "status: source running, hart 4 ARMED" "$STATION_DIR/tel.log" && ok "operator command through the FIFO answered" || bad "FIFO status command"
kill -STOP "$(cat "$STATION_DIR/ana.pid")"; sleep 0.3
OUTS=$("$HERE/board/stop_station.sh" 2>&1); rc=$?
[ $rc = 0 ] && echo "$OUTS" | grep -q "^frames " && ok "stop_station.sh stopped even a SIGSTOPped process: $OUTS" || bad "stop_station.sh rc=$rc: $OUTS"
ls "$STATION_DIR"/*.pid >/dev/null 2>&1 && bad "pid files left" || ok "no pid files left"
kill -TERM $P_H4; wait $P_H4 2>/dev/null
unset STATION_DIR STATION_BIN STATION_LINK STATION_FABRIC STATION_FABSIM

# every console line must fit the replay figure (108 characters)
LONG=$(cat "$D"/tel.log "$D"/acq.log "$D"/ana.log "$D"/hart4.log | grep -v "^\[fake hart 4\]\|commands are read from\|config: limits" | awk 'length($0) > 108' | wc -l)
[ "$LONG" = 0 ] && ok "no console line is longer than 108 characters" || { bad "$LONG console lines are longer than 108 characters"; cat "$D"/tel.log "$D"/acq.log "$D"/ana.log "$D"/hart4.log | awk 'length($0) > 108' | head -5; }
mkdir -p "$OUT/last-e2e"; cp "$D"/*.log "$D"/telemetry.csv "$OUT/last-e2e/" 2>/dev/null
rm -rf "$WORK"
if [ $fail = 0 ]; then echo "ALL TESTS PASSED"; else echo "$fail TEST(S) FAILED (logs in $OUT/last-e2e)"; fi
exit $fail
