#!/bin/sh
# Run the whole I08 matrix on the board. Each line of output is "<label> <json>".
# Usage on the board:  sh run_all.sh [runs]     (default 10 runs per condition; about 10 minutes)
N=${1:-10}
B=${CACHED_DMA:-/tmp/cached_dma}
say() { echo "$1 $2"; }
echo "# cached_dma matrix, $N runs per condition"
$B -B | while read l; do say "read_speed" "$l"; done
$B -B | while read l; do say "read_speed" "$l"; done
$B -B | while read l; do say "read_speed" "$l"; done
for win in c nc; do
  for st in cold clean dirty; do
    [ "$win" = nc ] && [ "$st" = cold ] && continue
    for f in 16 1024; do
      i=0; while [ $i -lt $N ]; do
        $B -S $st -f $f -D $win | while read l; do say "dma_$win/$st/f$f" "$l"; done
        i=$((i+1))
      done
    done
  done
done
for win in c nc; do
  i=0; while [ $i -lt $N ]; do $B -S partial -D $win | while read l; do say "partial/$win" "$l"; done; i=$((i+1)); done
done
for f in 16 1024; do
  i=0; while [ $i -lt $N ]; do $B -S control -f $f | while read l; do say "control/f$f" "$l"; done; i=$((i+1)); done
done
for m in sync evict; do
  for st in clean dirty; do
    i=0; while [ $i -lt 5 ]; do
      $B -S $st -f 16 -D nc -M $m | while read l; do say "maint_$m/nc/$st/f16" "$l"; done
      $B -S $st -f 1024 -D nc -M $m | while read l; do say "maint_$m/nc/$st/f1024" "$l"; done
      i=$((i+1))
    done
  done
done
for win in c nc; do
  i=0; while [ $i -lt 5 ]; do $B -S bench -n 32 -D $win | while read l; do say "bench/$win" "$l"; done; i=$((i+1)); done
done
