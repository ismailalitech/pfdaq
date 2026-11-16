#!/bin/bash
# deploy_payload.sh against a plain file that stands in for /dev/mmcblk0p2 (LOCAL_TEST=1: no ssh, no
# board, no block device). Checks the order of the steps and the refusals.
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
W=${TMPDIR:-/tmp}/amp-m08-deploy-$$
mkdir -p "$W"
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
export LOCAL_TEST=1 PART=$W/part2.img BACKUPS=$W/backups
D=$HERE/deploy_payload.sh
PROVEN=$ROOT/images/pfdaq-v051/payload.bin
DEMO=$ROOT/build/amp-m08/payload.bin
[ -f "$DEMO" ] || { echo "build the M08 payload first (build_firmware.sh)"; exit 1; }
n_p=$(stat -c %s "$PROVEN"); n_d=$(stat -c %s "$DEMO")
mk_part() { { cat "$PROVEN"; head -c 1048576 /dev/zero; } >"$PART"; }

mk_part
"$D" status 2>&1 | grep -q "= the proven payload" && ok "status recognises the proven payload" || bad "status"
"$D" install --yes >"$W/o1" 2>&1 && bad "install without a backup must refuse" || { grep -q "no verified backup" "$W/o1" && ok "install refuses without a backup"; }
"$D" backup >"$W/o2" 2>&1 && ok "backup succeeds" || bad "backup: $(tail -3 "$W/o2")"
B=$(ls -1d "$BACKUPS"/*/ | tail -1 | sed 's:/$::')
grep -q "matches_proven_payload=yes" "$B/MANIFEST" && ok "backup is byte-identical to the proven payload" || bad "manifest: $(cat "$B/MANIFEST")"
[ "$(stat -c %s "$B/payload-on-board.bin")" = "$n_p" ] && [ "$(stat -c %s "$B/partition.img")" = "$(stat -c %s "$PART")" ] && ok "backup holds the payload and the whole partition image" || bad "backup sizes"
"$D" install --yes >"$W/o3" 2>&1 && ok "install writes the demo payload" || bad "install: $(tail -4 "$W/o3")"
[ "$(head -c "$n_d" "$PART" | sha256sum | cut -d' ' -f1)" = "$(sha256sum "$DEMO" | cut -d' ' -f1)" ] && ok "partition now starts with the demo payload ($n_d bytes)" || bad "partition content"
[ "$(stat -c %s "$PART")" -ge "$n_d" ] && ok "the partition keeps its size" || bad "partition size changed"
grep -q "written and read back" "$W/o3" && ok "read-back verified" || bad "no read-back line"
"$D" restore --yes >"$W/o4" 2>&1 && ok "restore (proven) succeeds" || bad "restore: $(tail -3 "$W/o4")"
[ "$(head -c "$n_p" "$PART" | sha256sum | cut -d' ' -f1)" = "$(sha256sum "$PROVEN" | cut -d' ' -f1)" ] && ok "partition starts with the proven payload again" || bad "restore content"
"$D" install --yes >/dev/null 2>&1; "$D" restore --yes --from "$B" >"$W/o5" 2>&1 && [ "$(head -c "$n_p" "$PART" | sha256sum | cut -d' ' -f1)" = "$(sha256sum "$B/payload-on-board.bin" | cut -d' ' -f1)" ] && ok "restore from the saved backup" || bad "restore --from backup"
# refusals
printf 'garbage' | dd of="$PART" bs=1 conv=notrunc 2>/dev/null
"$D" backup >"$W/o6" 2>&1 && bad "a partition without the HSS magic must be refused" || { grep -q "not the HSS payload magic" "$W/o6" && ok "refuses a partition that does not start with the HSS magic"; }
mk_part; sed -i 's/^payload_sha256=.*/payload_sha256=0000/' "$B/MANIFEST"
"$D" install --yes >"$W/o7" 2>&1 && bad "tampered manifest must refuse" || { grep -q "does not match its manifest" "$W/o7" && ok "refuses a backup whose manifest does not match"; }
mk_part
echo no | "$D" backup >/dev/null 2>&1; B2=$(ls -1d "$BACKUPS"/*/ | tail -1 | sed 's:/$::')
echo nope | "$D" install --backup "$B2" >"$W/o8" 2>&1 && bad "wrong confirmation must refuse" || { grep -q "not confirmed" "$W/o8" && ok "install without the typed confirmation writes nothing"; }
[ "$(head -c "$n_p" "$PART" | sha256sum | cut -d' ' -f1)" = "$(sha256sum "$PROVEN" | cut -d' ' -f1)" ] && ok "and the partition is unchanged" || bad "partition changed without confirmation"
bash -n "$D" && ok "bash -n"
rm -rf "$W"
[ $fail = 0 ] && echo "deploy test passed" || echo "$fail failure(s)"
exit $fail
