#!/bin/bash
# Host tests of M08, no board involved. About 40 s.
#   tests/run_tests.sh [build-dir]
# 1. unit tests: statistics (median, p95 by nearest rank), the window check of hart 4, the real polling
#    loop and interrupt answer of pfdaq_m08.h against a thread, the layout of the shared block
# 2. the program against the simulated hart 4: every trial is counted once; dropped answers are counted
#    as lost; delayed answers as late; a method that loses an answer on RPMsg stops; the CSV is complete
# 3. the same program and unit tests cross-compiled for RISC-V and run in qemu-riscv64 (user mode)
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$HERE/build}
H=$OUT/host
FW=$HERE/../../firmware/u54_4-supervisor
fail=0
ok()   { echo "ok   $*"; }
bad()  { echo "FAIL $*"; fail=$((fail+1)); }
mkdir -p "$H"
make -C "$HERE" host OUT="$OUT" >/dev/null || exit 1
gcc -O2 -Wall -Wextra -I"$FW" -std=gnu11 -pthread -o "$H/test_units" "$HERE/tests/test_units.c" || exit 1

echo "== 1 unit tests"
"$H/test_units" && ok "unit tests" || bad "unit tests"

echo "== 2 the program against the simulated hart 4"
W=${TMPDIR:-/tmp}/amp-m08-test-$$; mkdir -p "$W"
field() { python3 -c 'import json,sys
for l in open(sys.argv[1]):
    r=json.loads(l)
    if r["method"]==sys.argv[2]: print(r[sys.argv[3]])' "$W/$1.jsonl" "$2" "$3"; }

"$H/m08_rtt" --sim -n 400 -w 20 -d 100 -c 0 --json "$W/clean.jsonl" --csv "$W/clean.csv" >"$W/clean.txt" 2>&1
for m in rpmsg-query rpmsg-ping flag irq; do
    [ "$(field clean $m ok)" = 400 ] && [ "$(field clean $m lost)" = 0 ] && [ "$(field clean $m late)" = 0 ] && ok "clean run, $m: 400 ok, 0 late, 0 lost" || bad "clean run, $m"
done
python3 - "$W/clean.jsonl" <<'PY' && ok "clean run: min <= median <= p95 <= max; query is above 1000 us (the simulated 1 ms snapshot wait)" || bad "clean run statistics"
import json, sys
r = {x["method"]: x for x in map(json.loads, open(sys.argv[1]))}
for x in r.values():
    assert x["min_us"] <= x["median_us"] <= x["p95_us"] <= x["max_us"]
assert r["rpmsg-query"]["min_us"] >= 1000 and r["rpmsg-ping"]["median_us"] < 1000
PY
grep -q "hart 4 counted 420 answers for 420 requests" "$W/clean.txt" && ok "hart 4's own count equals the requests (flag and irq, warm-up included)" || bad "hart 4 count"
[ "$(grep -c ',ok,' "$W/clean.csv")" -ge 1600 ] && [ "$(grep -c ',warm,' "$W/clean.csv")" = 80 ] && ok "CSV: 1600 measured rows and 80 warm-up rows" || bad "CSV rows"

# dropped answers: every 10th request of flag and irq is not answered (lost, after the grace time)
"$H/m08_rtt" --sim -n 100 -w 0 -d 100 -c 0 -t 2000 -g 5 -m flag,irq --sim-drop 10 --json "$W/drop.jsonl" >"$W/drop.txt" 2>&1
for m in flag irq; do
    [ "$(field drop $m lost)" = 10 ] && [ "$(field drop $m ok)" = 90 ] && ok "dropped answers, $m: 10 lost, 90 ok" || bad "dropped answers, $m: lost $(field drop $m lost) ok $(field drop $m ok)"
done
# on RPMsg the first lost answer stops the method, and says why
"$H/m08_rtt" --sim -n 100 -w 0 -d 100 -c 0 -t 2000 -g 5 -m rpmsg-ping --sim-drop 10 >"$W/dropr.txt" 2>&1
grep -q "stopped at the first lost answer" "$W/dropr.txt" && grep -Eq "rpmsg-ping +9 +0 +1" "$W/dropr.txt" && ok "RPMsg: stops at the first lost answer (9 ok, 1 lost)" || bad "RPMsg lost handling: $(grep rpmsg-ping "$W/dropr.txt")"
# late answers: every 10th is delayed by 20 ms, the timeout is 2 ms, the grace time 100 ms
"$H/m08_rtt" --sim -n 100 -w 0 -d 100 -c 0 -t 2000 -g 100 -m flag,irq,rpmsg-ping --sim-late 10 --sim-late-us 20000 --json "$W/late.jsonl" >"$W/late.txt" 2>&1
for m in flag irq rpmsg-ping; do
    [ "$(field late $m late)" = 10 ] && [ "$(field late $m lost)" = 0 ] && [ "$(field late $m ok)" = 90 ] && ok "late answers, $m: 10 late, 90 ok, 0 lost" || bad "late answers, $m: late $(field late $m late) ok $(field late $m ok) lost $(field late $m lost)"
done
python3 - "$W/late.jsonl" <<'PY' && ok "late answers are in the maximum (>= 19 ms) and not in the median" || bad "late answers and the statistics"
import json, sys
for x in map(json.loads, open(sys.argv[1])):
    assert x["max_us"] >= 19000 and x["median_us"] < 2000, x
PY
# latency channel 1 already enabled (someone else's periodic events): the irq method must not touch it
"$H/m08_rtt" --sim -n 20 -w 0 -d 100 -c 0 -m irq --sim-chan-busy --json "$W/busy.jsonl" >"$W/busy.txt" 2>&1
grep -q "enabled or has an interrupt pending: not touched" "$W/busy.txt" && [ "$(field busy irq ok)" = 0 ] && ok "irq method refuses a channel that is not idle" || bad "irq busy refusal"
"$H/m08_rtt" --sim -n 20 -w 0 -d 100 -c 0 -m irq --sim-chan-busy --force --json "$W/busy2.jsonl" >"$W/busy2.txt" 2>&1
[ "$(field busy2 irq ok)" = 20 ] && ok "--force overrides it" || bad "--force"
"$H/m08_rtt" --sim -n 0 >/dev/null 2>&1 && bad "-n 0 must be refused" || ok "-n 0 is refused"
"$H/m08_rtt" --sim -m nonsense >/dev/null 2>&1 && bad "an unknown method must be refused" || ok "an unknown method is refused"
"$H/m08_rtt" --sim -c 999 -n 5 >/dev/null 2>&1 && bad "a cpu that does not exist must be refused" || ok "a cpu that does not exist is refused"
rm -rf "$W"

echo "== 3 RISC-V build under qemu-riscv64"
if command -v qemu-riscv64 >/dev/null; then
    RV=${RVGCC:-/usr/local/microchip/Libero_SoC_2026.1/SmartHLS/SmartHLS/swtools/binutils/riscv-gnu-toolchain/bin/riscv64-unknown-linux-gnu-gcc}
    mkdir -p "$OUT/board"
    if "$RV" -O2 -Wall -Wextra -I"$FW" -std=gnu11 -static -pthread -o "$OUT/board/test_units" "$HERE/tests/test_units.c" &&
       "$RV" -O2 -Wall -Wextra -I"$FW" -std=gnu11 -static -pthread -o "$OUT/board/m08_rtt" "$HERE/m08_rtt.c"; then
        qemu-riscv64 "$OUT/board/test_units" && ok "unit tests on RISC-V (qemu user mode)" || bad "unit tests on RISC-V"
        qemu-riscv64 "$OUT/board/m08_rtt" --sim -n 100 -w 5 -d 100 -c 0 -m rpmsg-ping,flag,irq --json /dev/null 2>&1 | grep -Eq "^irq +100 +0 +0" && ok "program on RISC-V against the simulated hart 4: irq 100 ok" || bad "program on RISC-V"
    else
        bad "RISC-V build"
    fi
else
    echo "skip qemu-riscv64 not installed"
fi
[ $fail = 0 ] && echo "ALL M08 HOST TESTS PASSED" || echo "$fail failure(s)"
exit $fail
