#!/bin/bash
# run_m09.sh and m09_ctl against the stand-in for hart 4 (fake_hart4_m09: the real sequence of pfdaq_pmp.h behind a
# Unix socket, with a model of the PMP CSRs). Two starting states: a clean PMP, and the state the sources predict for the board.
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${TMPDIR:-/tmp}/amp-m09-run-$$
mkdir -p "$W"
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
make -C "$HERE" host >/dev/null || exit 1
H=$HERE/build/host
export LOCAL_TEST=1 BIN_LOCAL=$H/m09_ctl

for st in clean board; do
    "$H/fake_hart4_m09" --sock "$W/h4-$st.sock" --state $st >"$W/h4-$st.log" 2>&1 & P=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do grep -q listening "$W/h4-$st.log" 2>/dev/null && break; sleep 0.2; done
    export LINKOPT="--link unix:$W/h4-$st.sock" OUT=$W/out-$st
    "$HERE/run_m09.sh" --yes >"$W/run-$st.log" 2>&1; rc=$?
    if [ $st = clean ]; then
        [ $rc = 0 ] && ok "state clean: run_m09.sh ends with status 0" || bad "state clean: status $rc: $(tail -4 "$W/run-$st.log")"
        grep -q "result OK" "$W/h4-$st.log" && grep -q "run: result OK (0), 3 access faults caught" "$W/out-$st/run.txt" && ok "clean: three violations caught, result OK" || bad "clean: result"
        grep -q "answered\": 20" "$W/out-$st/ping-after.txt" && ok "clean: 20 of 20 pings after the test" || bad "clean: pings after"
        grep -q "entries restored" "$W/h4-$st.log" && ok "clean: entries restored" || bad "clean: restore line"
    else
        [ $rc = 3 ] && ok "state board: run_m09.sh ends with status 3 (the test was refused: not an error)" || bad "state board: status $rc"
        grep -q "run: result REFUSED (1), 0 access faults" "$W/out-$st/run.txt" && ok "board: result REFUSED, no fault, nothing run" || bad "board: result: $(cat "$W/out-$st/run.txt")"
        grep -q "a locked entry that allows everything" "$W/out-$st/report.txt" && ok "board: the report says the PMP is locked open" || bad "board: report"
        grep -q "REFUSED: pmp0 is locked" "$W/h4-$st.log" && ok "board: hart 4 console says why" || bad "board: console line"
        grep -q "answered\": 20" "$W/out-$st/ping-after.txt" && ok "board: 20 of 20 pings after the refused test" || bad "board: pings after"
    fi
    kill $P 2>/dev/null; wait $P 2>/dev/null
done

# nobody there: the preflight stops and nothing is run
export LINKOPT="--link unix:$W/none.sock" OUT=$W/out-none
"$HERE/run_m09.sh" --yes >"$W/run-none.log" 2>&1 && bad "no hart 4: must fail" || { grep -q "hart 4 does not answer: stop here" "$W/run-none.log" && ok "no answer to the first ping: the script stops before the test"; }
# a hart 4 that hangs in RUN: stand-in that answers ping and report but never RUN
cat >"$W/hang.py" <<'PY'
import socket, struct, sys, os
p = sys.argv[1]
if os.path.exists(p): os.unlink(p)
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); s.bind(p); s.listen(2)
print("listening", flush=True)
while True:
    c, _ = s.accept()
    while True:
        d = c.recv(64)
        if len(d) != 28: break
        magic, typ = struct.unpack_from("<II", d)
        if typ == 0x41: continue                       # RUN: never answered
        c.send(struct.pack("<9I", 0x41444650, typ, 0, 0, 0, 0, 0, 0, 1))
PY
python3 "$W/hang.py" "$W/hang.sock" >"$W/hang.log" 2>&1 & P=$!
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q listening "$W/hang.log" 2>/dev/null && break; sleep 0.2; done
export LINKOPT="--link unix:$W/hang.sock" OUT=$W/out-hang RUN_TIMEOUT_MS=1500
"$HERE/run_m09.sh" --yes >"$W/run-hang.log" 2>&1 && bad "a hung RUN must fail" || { grep -q "NO ANSWER to RUN" "$W/out-hang/run.txt" && grep -q "deploy_payload.sh restore" "$W/out-hang/run.txt" && ok "a hung hart 4: the script reports it, says how to restore, and ends"; }
kill $P 2>/dev/null; wait $P 2>/dev/null
bash -n "$HERE/run_m09.sh" && ok "bash -n"
rm -rf "$W"
[ $fail = 0 ] && echo "run script test passed" || echo "$fail failure(s)"
exit $fail
