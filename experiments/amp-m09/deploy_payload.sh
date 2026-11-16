#!/bin/bash
# Put the M09 demo boot payload (U-Boot on harts 1-3, hart 4 firmware that reads the PMP and runs one fenced user-mode test) on the running board
# and put the proven payload back afterwards. Runs on the PC. It is for board day, under supervision,
# when nothing else is using the board (a reboot ends every other experiment).
#
#   deploy_payload.sh status                  read-only: what the board's payload partition holds
#   deploy_payload.sh backup                  MANDATORY first step: read the payload partition into a file on the PC
#   deploy_payload.sh install                 write build/amp-m09/payload.bin (needs a verified backup)
#   deploy_payload.sh restore [--from DIR]    write the proven payload (default) or the file saved by `backup`
#   options: --yes (skip the typed confirmation), --backup DIR (use this backup), --board root@host
#
# What it touches on the board: only /dev/mmcblk0p2, the raw payload partition of the SD card (module B05),
# and files in /root. It never touches the fatfs partition 1, the root file system, the eNVM, boot-mode
# fuses, keys or any lock (project rule: the board is never locked); it does not reboot the board.
#
# After `install` the board must be rebooted by you (`ssh root@board reboot`) and the demo needs
# about a minute to come up. If it does not come up, power off, take the SD card out, and write the
# saved or the proven payload to partition 2 of the card from the PC by hand:
#     lsblk -o NAME,SIZE,LABEL,MODEL /dev/sdX                (find the card; check it is the SD card)
#     sudo dd if=<file> of=/dev/sdX2 bs=1M conv=fsync        (X2 is the second partition; read the lsblk output first)
# This script never does that by itself.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BOARD=${BOARD:-root@pfdaq-board}
DEMO=${DEMO:-$ROOT/build/amp-m09/payload.bin}
DEMO_ELF=${DEMO_ELF:-$ROOT/build/amp-m09/polarfire-soc-freertos-amp-demo.elf}
PROVEN=${PROVEN:-$ROOT/images/pfdaq-v051/payload.bin}
PROVEN_ELF=${PROVEN_ELF:-$ROOT/images/pfdaq-v051/polarfire-soc-freertos-amp-demo.elf}
BACKUPS=${BACKUPS:-$ROOT/logs/m5/m09/payload-backup}
PART=${PART:-/dev/mmcblk0p2}                 # host test: a plain file standing in for the partition (LOCAL_TEST=1)
SSHO=(-o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=3)
YES=0; BACKUP_DIR=""; FROM=proven

cmd=${1:-}; [ $# -gt 0 ] && shift
while [ $# -gt 0 ]; do
    case $1 in
        --yes) YES=1 ;;
        --backup) BACKUP_DIR=$2; shift ;;
        --from) FROM=$2; shift ;;
        --board) BOARD=$2; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

if [ -n "${LOCAL_TEST:-}" ]; then      # host test: "the board" is this PC and PART a file; no ssh, no scp
    remote() { timeout "${2:-60}" bash -c "$1"; }
    copy_to_board() { cp "$1" "/tmp/payload-to-write.$$.bin"; BOARD_COPY=/tmp/payload-to-write.$$.bin; }
    ISBLK="-e"; DDOPT="conv=fsync,notrunc"
else
    remote() { timeout "${2:-60}" ssh "${SSHO[@]}" "$BOARD" "$1"; }
    copy_to_board() { timeout 120 scp -q "${SSHO[@]}" "$1" "$BOARD:/root/payload-to-write.bin"; BOARD_COPY=/root/payload-to-write.bin; }
    ISBLK="-b"; DDOPT="conv=fsync"
fi
die() { echo "deploy_payload: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

# ---- what the partition holds ------------------------------------------------------------------
# prints: partition_bytes payload_bytes (payload length from the HSS header, offset 1480, after the magic check)
inspect_board() {
    local out
    out=$(remote "set -e
        test $ISBLK $PART || { echo 'no $PART' >&2; exit 1; }
        if grep -q '^$PART ' /proc/mounts; then echo '$PART is mounted' >&2; exit 1; fi
        echo size \$(blockdev --getsize64 $PART 2>/dev/null || stat -c %s $PART)
        echo magic \$(dd if=$PART bs=1 skip=0 count=4 2>/dev/null | od -An -tx4 | tr -d ' ')
        echo length \$(dd if=$PART bs=1 skip=1480 count=8 2>/dev/null | od -An -tu8 | tr -d ' ')" 30) || die "cannot inspect $PART on $BOARD"
    PART_BYTES=$(echo "$out" | awk '/^size/ {print $2}')
    MAGIC=$(echo "$out" | awk '/^magic/ {print $2}')
    PAY_BYTES=$(echo "$out" | awk '/^length/ {print $2}')
    [ "$MAGIC" = b007c0de ] || die "the first word of $PART is $MAGIC, not the HSS payload magic b007c0de: stop and look at the board"
    [ "$PAY_BYTES" -ge 1000000 ] && [ "$PAY_BYTES" -le 2097152 ] || die "payload length $PAY_BYTES in the header is outside 1..2 MiB (the HSS limit): stop"
    [ "$PART_BYTES" -ge "$PAY_BYTES" ] || die "partition ($PART_BYTES bytes) smaller than the payload ($PAY_BYTES)"
}

board_state() {
    remote "lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT /dev/mmcblk0 2>&1; echo; for r in /sys/class/remoteproc/*; do echo \$r \$(cat \$r/state 2>/dev/null); done; ls /sys/bus/rpmsg/devices 2>/dev/null | grep pfdaq-supervisor || echo 'no pfdaq-supervisor channel'; uname -r" 30
}

latest_backup() { ls -1d "$BACKUPS"/*/ 2>/dev/null | sort | tail -1 | sed 's:/$::'; }

# ---- commands ----------------------------------------------------------------------------------
do_status() {
    board_state
    inspect_board
    local rsha
    rsha=$(remote "head -c $PAY_BYTES $PART | sha256sum | cut -d' ' -f1" 120)
    echo "payload partition: $PART_BYTES bytes; HSS header says the payload is $PAY_BYTES bytes"
    echo "sha256 of those bytes: $rsha"
    [ -f "$PROVEN" ] && [ "$rsha" = "$(sha "$PROVEN")" ] && echo "  = the proven payload ($PROVEN)"
    [ -f "$DEMO" ] && [ "$rsha" = "$(sha "$DEMO")" ] && echo "  = the demo payload ($DEMO)"
    true
}

do_backup() {
    board_state
    inspect_board
    local d="$BACKUPS/$(date +%Y%m%d-%H%M%S)" rsha lsha limit=$((64*1024*1024)) n
    mkdir -p "$d"
    # 1. the payload bytes, with a checksum computed on the board and on the PC
    rsha=$(remote "head -c $PAY_BYTES $PART | sha256sum | cut -d' ' -f1" 120)
    remote "head -c $PAY_BYTES $PART" 180 >"$d/payload-on-board.bin"
    [ "$(stat -c %s "$d/payload-on-board.bin")" = "$PAY_BYTES" ] || die "backup has the wrong size"
    lsha=$(sha "$d/payload-on-board.bin")
    [ "$lsha" = "$rsha" ] || die "backup checksum on the PC ($lsha) differs from the one computed on the board ($rsha): transfer error, nothing was written to the board"
    # 2. the whole partition too, when it is not huge (what follows the payload is not part of it, but keep it)
    n=$PART_BYTES; [ "$n" -gt "$limit" ] && n=$limit
    remote "head -c $n $PART" 300 >"$d/partition.img"
    [ "$(stat -c %s "$d/partition.img")" = "$n" ] || die "partition image has the wrong size"
    [ "$(head -c "$PAY_BYTES" "$d/partition.img" | sha256sum | cut -d' ' -f1)" = "$rsha" ] || die "partition image does not start with the payload"
    local same=no
    [ -f "$PROVEN" ] && [ "$lsha" = "$(sha "$PROVEN")" ] && same=yes
    {
        echo "date=$(date -Is)"; echo "board=$BOARD"; echo "partition=$PART"; echo "partition_bytes=$PART_BYTES"
        echo "payload_bytes=$PAY_BYTES"; echo "payload_sha256=$lsha"; echo "partition_image_bytes=$n"
        echo "matches_proven_payload=$same"; echo "verified=1"
    } >"$d/MANIFEST"
    echo "backup written to $d"
    cat "$d/MANIFEST"
    if [ $same = no ]; then
        echo
        echo "NOTE: the payload on the board is NOT byte-identical to $PROVEN."
        echo "The generator leaves a few bytes of padding uninitialised, so two builds of the same inputs can differ."
        echo "Compare their structure before you decide what to restore:"
        echo "  python3 $HERE/../amp-station/verify_payload.py --demo-payload $d/payload-on-board.bin --demo-elf $PROVEN_ELF --proven-payload $PROVEN --proven-elf $PROVEN_ELF --uboot <u-boot.bin>"
    fi
}

confirm() {   # confirm <word>
    [ $YES = 1 ] && return 0
    echo "About to write $PART on $BOARD. Type $1 to go on:"
    read -r a
    [ "$a" = "$1" ] || die "not confirmed, nothing written"
}

write_payload() {   # write_payload <file> <word>
    local f=$1 n sum rsum
    n=$(stat -c %s "$f"); sum=$(sha "$f")
    inspect_board
    [ "$n" -le "$PART_BYTES" ] || die "$f ($n bytes) does not fit the partition"
    board_state
    echo "file $f: $n bytes, sha256 $sum"
    confirm "$2"
    copy_to_board "$f"
    rsum=$(remote "sha256sum $BOARD_COPY | cut -d' ' -f1" 30)
    [ "$rsum" = "$sum" ] || die "the copy on the board has another checksum ($rsum): nothing written to $PART"
    remote "dd if=$BOARD_COPY of=$PART bs=1M $DDOPT 2>&1 && sync" 120
    rsum=$(remote "head -c $n $PART | sha256sum | cut -d' ' -f1" 120)
    if [ "$rsum" != "$sum" ]; then
        echo "READ-BACK DIFFERS ($rsum). Do NOT reboot. Write again, or restore from the backup." >&2
        exit 1
    fi
    remote "rm -f $BOARD_COPY" 10
    echo "written and read back: $n bytes, sha256 $sum"
}

do_install() {
    [ -f "$DEMO" ] || die "$DEMO not found: run experiments/amp-m09/build_firmware.sh"
    [ -f "$PROVEN" ] || die "$PROVEN not found"
    local b=${BACKUP_DIR:-$(latest_backup)}
    [ -n "$b" ] && [ -f "$b/MANIFEST" ] && grep -q '^verified=1' "$b/MANIFEST" || die "no verified backup: run '$0 backup' first"
    [ "$(sha "$b/payload-on-board.bin")" = "$(grep '^payload_sha256=' "$b/MANIFEST" | cut -d= -f2)" ] || die "backup file does not match its manifest"
    echo "using backup $b"
    python3 "$HERE/../amp-station/verify_payload.py" --demo-payload "$DEMO" --demo-elf "$DEMO_ELF" --proven-payload "$PROVEN" \
        --proven-elf "$PROVEN_ELF" --uboot "$ROOT/build/amp-m09/u-boot.bin" >/dev/null || die "the demo payload fails the offline verification"
    echo "offline verification of $DEMO passed"
    write_payload "$DEMO" WRITE-DEMO-PAYLOAD
    cat <<EOF

Next, by you: reboot the board       ssh $BOARD reboot
wait about a minute, then            ssh $BOARD 'ls /sys/bus/rpmsg/devices | grep pfdaq-supervisor; cat /sys/class/remoteproc/remoteproc0/state'
and check the hart 4 console for     [m09] AMP demo variant M09: PMP report and one user-mode test, then the supervisor as usual
Then run                             $HERE/run_m09.sh
To go back                           $0 restore
EOF
}

do_restore() {
    local f
    if [ "$FROM" = proven ]; then
        f=$PROVEN
        local want
        want=$(grep ' payload.bin$' "$ROOT/images/pfdaq-v051/SHA256SUMS" | cut -d' ' -f1)
        [ -n "$want" ] && [ "$(sha "$f")" = "$want" ] || die "$f does not match images/pfdaq-v051/SHA256SUMS"
    else
        f=$FROM/payload-on-board.bin
        [ -f "$f" ] && grep -q '^verified=1' "$FROM/MANIFEST" || die "$FROM is not a verified backup directory"
    fi
    write_payload "$f" WRITE-RESTORE-PAYLOAD
    echo "Reboot the board (ssh $BOARD reboot) to run it. Check afterwards: remoteproc attached, pfdaq-supervisor channel present,"
    echo "and the hart 4 console prints [pfdaq-sup] lines and no [m09] lines."
}

case $cmd in
    status) do_status ;;
    backup) do_backup ;;
    install) do_install ;;
    restore) do_restore ;;
    *) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
