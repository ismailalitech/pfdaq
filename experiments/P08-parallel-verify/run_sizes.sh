#!/bin/sh
# P08, second experiment: does the per-worker slowdown depend on the size of a burst?
# 32 MiB per run in bursts of 64 KiB, 256 KiB, 1 MiB and 4 MiB, counter payload, 1 and 3 workers, and the original tool.
# Usage on the board:  sh run_sizes.sh [runs]     (default 5; about two minutes)
N=${1:-5}
MT=${PFDAQ_CAP_MT:-/tmp/pfdaq_cap_mt}
say() { while read l; do echo "$1 $l"; done; }
echo "# burst-size matrix, $N runs per condition, 32 MiB per run, counter payload"
for fn in 64:512 256:128 1024:32 4096:8; do
  f=${fn%%:*}; n=${fn#*:}
  i=0; while [ $i -lt $N ]; do pfdaq_cap -m counter -w 252 -f $f -n $n | say "size/f$f/base"; i=$((i+1)); done
  for j in 1 3; do
    i=0; while [ $i -lt $N ]; do $MT -m counter -w 252 -f $f -n $n -j $j | say "size/f$f/j$j"; i=$((i+1)); done
  done
done
