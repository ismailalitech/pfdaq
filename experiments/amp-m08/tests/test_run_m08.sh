#!/bin/bash
# run_m08.sh against stand-ins (LOCAL_TEST=1: "the board" is this PC, the program runs with --sim).
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${TMPDIR:-/tmp}/amp-m08-run-$$
mkdir -p "$W"
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
make -C "$HERE" host >/dev/null || exit 1
export LOCAL_TEST=1 EXTRA=--sim BIN_LOCAL=$HERE/build/host/m08_rtt OUT=$W/out N=200 CPU=0
"$HERE/run_m08.sh" >"$W/log" 2>&1 && ok "run_m08.sh ends with status 0 against the simulated hart 4" || { bad "run_m08.sh: $(tail -5 "$W/log")"; }
for f in preflight.txt probe.txt report-idle.txt report-loaded.txt trials-idle.csv trials-loaded.csv summary-idle.jsonl summary-loaded.jsonl after.txt run-info.txt; do
    [ -s "$W/out/$f" ] && ok "$f written" || bad "$f missing or empty"
done
grep -q "probe: hart 4 sees the shared block" "$W/out/probe.txt" && ok "probe line" || bad "probe line"
[ "$(wc -l <"$W/out/summary-idle.jsonl")" = 4 ] && ok "four JSON lines (one per method)" || bad "JSON lines"
python3 - "$W/out/summary-idle.jsonl" "$W/out/trials-idle.csv" <<'PY' && ok "JSON parses; ok+late+lost = trials; CSV has one row per trial" || bad "JSON/CSV content"
import json, sys, csv
rows = [json.loads(l) for l in open(sys.argv[1])]
assert [r["method"] for r in rows] == ["rpmsg-query", "rpmsg-ping", "flag", "irq"]
for r in rows:
    assert r["ok"] + r["late"] + r["lost"] == r["trials"] == 200, r
    assert r["min_us"] <= r["median_us"] <= r["p95_us"] <= r["max_us"], r
c = list(csv.DictReader(open(sys.argv[2])))
per = {}
for x in c:
    per[x["method"]] = per.get(x["method"], 0) + 1
assert all(v == 200 + 50 for v in per.values()), per       # 200 trials + 50 warm-up rows
PY
# a payload that is not the M08 one: the probe must fail and the script must stop before measuring
export OUT=$W/out2 EXTRA="--sim --sim-production"
"$HERE/run_m08.sh" --check >"$W/log2" 2>&1 && bad "the production firmware must fail the probe" || ok "a hart 4 that does not know the M08 types (production firmware) fails the preflight"
rm -rf "$W"
bash -n "$HERE/run_m08.sh" && ok "bash -n"
[ $fail = 0 ] && echo "run script test passed" || echo "$fail failure(s)"
exit $fail
