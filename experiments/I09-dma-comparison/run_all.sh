#!/bin/sh
# Run the whole I09 matrix on the board. Each line of output is "<label> <json>".
# Usage on the board:  sh run_all.sh [passes]      (default 5 passes; one pass takes about 2 minutes)
# Start it with nohup and read the output file afterwards: logging in over ssh while it runs disturbs the numbers.
N=${1:-5}
B=${DMA_COMPARE:-/tmp/dma_compare}
say() { while read l; do echo "$1 $l"; done; }
echo "# dma_compare matrix, $N passes; kernel $(uname -r); started $(date -u +%Y-%m-%dT%H:%M:%SZ)"
p=1
while [ $p -le $N ]; do
  echo "# pass $p"
  $B -m idle -t 2 | say "idle/pass$p"
  for b in 4096 65536 1048576 4194304; do
    for w in nc c wcb; do
      $B -m fabric -b $b -D $w | say "fabric/$w/b$b"
    done
    for pair in "nc nc" "c c" "wcb wcb" "c nc" "nc c"; do
      set -- $pair
      $B -m pdma -b $b -R $1 -D $2 | say "pdma/$1>$2/b$b"
    done
    for pair in "nc nc" "c c" "wcb wcb" "c nc" "nc c"; do
      set -- $pair
      $B -m memcpy -b $b -R $1 -D $2 | say "memcpy/$1>$2/b$b"
    done
    for pair in "c c" "c nc"; do
      set -- $pair
      $B -m memcpy -b $b -R $1 -D $2 -C | say "memcpy-cold/$1>$2/b$b"
      $B -m pdma -b $b -R $1 -D $2 -C | say "pdma-cold/$1>$2/b$b"
    done
  done
  for ch in 2 4; do
    for w in "nc nc" "c c"; do
      set -- $w
      $B -m pdma -b 1048576 -c $ch -R $1 -D $2 | say "pdma-x$ch/$1>$2/b1048576"
    done
  done
  $B -m idle -t 2 | say "idle/pass$p"
  p=$((p+1))
done
echo "# done $(date -u +%Y-%m-%dT%H:%M:%SZ)"
