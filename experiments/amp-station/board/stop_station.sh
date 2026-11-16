#!/bin/sh
# Stop the station cleanly: acquisition first (it stops the source), then analysis (sends DISARM to
# hart 4), then telemetry (prints the summary). Every wait is bounded; a process that does not exit
# in 5 s is killed. Safe to run when nothing is running.
DIR=${STATION_DIR:-/dev/shm/station}
BIN=${STATION_BIN:-/root/station}

for name in acq ana tel; do
    [ -f "$DIR/$name.pid" ] || continue
    pid=$(cat "$DIR/$name.pid")
    kill -0 "$pid" 2>/dev/null || { rm -f "$DIR/$name.pid"; continue; }
    kill -CONT "$pid" 2>/dev/null          # a stopped process cannot handle SIGTERM
    kill -TERM "$pid" 2>/dev/null
    n=50
    while [ $n -gt 0 ] && kill -0 "$pid" 2>/dev/null; do sleep 0.1; n=$((n-1)); done
    if kill -0 "$pid" 2>/dev/null; then
        echo "stop_station: $name ($pid) did not exit in 5 s, killing it" >&2
        kill -KILL "$pid" 2>/dev/null
    fi
    rm -f "$DIR/$name.pid"
done
pkill -f "tail .*-F $DIR/" 2>/dev/null
[ -f "$DIR/ctl" ] && "$BIN" summary --dir "$DIR"
exit 0
