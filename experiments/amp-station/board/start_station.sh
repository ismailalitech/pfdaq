#!/bin/sh
# Start the four-core station on the board: three processes, one per Linux cpu.
#   start_station.sh [extra options passed to every process, e.g. --window-ms 500]
# Run as root on the board. Needs: /root/station (the static binary), the pfdaq image with the
# supervisor channel (the demo payload on hart 4), the UIO devices and u-dma-buf of the image.
#   cpu 0 (hart 1)  station acq   real-time capture from the fabric into ring A
#   cpu 1 (hart 2)  station ana   verification, features, the RPMsg link to hart 4
#   cpu 2 (hart 3)  station tel   dashboard, CSV, operator commands (FIFO $DIR/cmd)
DIR=${STATION_DIR:-/dev/shm/station}
BIN=${STATION_BIN:-/root/station}
LINK=${STATION_LINK:-rpmsg}         # tests on the PC use unix:PATH and STATION_FABRIC=sim
FABRIC=${STATION_FABRIC:-hw}
EXTRA="$*"

[ -x "$BIN" ] || { echo "start_station: $BIN not found or not executable" >&2; exit 1; }
if [ "$LINK" = rpmsg ]; then
    [ -d /sys/bus/rpmsg/devices ] && ls /sys/bus/rpmsg/devices | grep -q pfdaq-supervisor || {
        echo "start_station: no pfdaq-supervisor channel: is the payload on hart 4 running?" >&2; exit 1; }
fi
if [ -f "$DIR/ctl" ] && ls "$DIR"/*.pid >/dev/null 2>&1; then
    echo "start_station: $DIR has pid files: run stop_station.sh first" >&2; exit 1
fi
rm -rf "$DIR"; mkdir -p "$DIR" || exit 1
# the fabric's sequence counter is not reset between starts: init reads it so the analysis reports the right number to hart 4
SYNC=; [ "$FABRIC" = hw ] && SYNC=--sync-seq
"$BIN" init --dir "$DIR" $SYNC || exit 1

pin() {  # pin <cpu> <name> <role> [options]: taskset when available, the process also pins itself
    cpu=$1; name=$2; role=$3; shift 3
    if command -v taskset >/dev/null 2>&1; then
        taskset -c "$cpu" "$BIN" "$role" --dir "$DIR" --cpu "$cpu" "$@" $EXTRA >"$DIR/$name.log" 2>&1 &
    else
        "$BIN" "$role" --dir "$DIR" --cpu "$cpu" "$@" $EXTRA >"$DIR/$name.log" 2>&1 &
    fi
    echo $! >"$DIR/$name.pid"
}

pin 2 tel tel
pin 1 ana ana --link "$LINK"
# the analysis arms hart 4 first; start the data source only when that worked (or after 5 s)
n=50
while [ $n -gt 0 ] && ! grep -q "hart 4 armed\|did not acknowledge" "$DIR/ana.log" 2>/dev/null; do sleep 0.1; n=$((n-1)); done
grep -q "hart 4 armed" "$DIR/ana.log" || { echo "start_station: hart 4 did not acknowledge ARM (see $DIR/ana.log)" >&2; "$(dirname "$0")/stop_station.sh"; exit 1; }
pin 0 acq acq --fabric "$FABRIC" ${STATION_FABSIM:+--fabsim "$STATION_FABSIM"}
sleep 1
for p in tel ana acq; do kill -0 "$(cat "$DIR/$p.pid")" 2>/dev/null || { echo "start_station: $p exited, see $DIR/$p.log" >&2; cat "$DIR/$p.log" >&2; exit 1; }; done
echo "station running: acq $(cat "$DIR/acq.pid") on cpu 0, ana $(cat "$DIR/ana.pid") on cpu 1, tel $(cat "$DIR/tel.pid") on cpu 2; logs in $DIR"
