#!/bin/sh
# Run the P08 matrix on the board. Each output line is "<label> <json>".
# Usage on the board:  sh run_all.sh [runs]     (default 8 runs per condition; about 10 minutes)
N=${1:-8}
MT=${PFDAQ_CAP_MT:-/tmp/pfdaq_cap_mt}
say() { while read l; do echo "$1 $l"; done; }
echo "# pfdaq_cap_mt matrix, $N runs per condition, 32 bursts of 1 MiB per run"
modes="counter:-m_counter prbs:-m_prbs fir:-m_fir_-S_333"
for mm in $modes; do
  mode=${mm%%:*}; opt=$(echo ${mm#*:} | tr _ ' ')
  i=0; while [ $i -lt $N ]; do pfdaq_cap $opt -w 252 -f 1024 -n 32 | say "base/$mode"; i=$((i+1)); done
  i=0; while [ $i -lt $N ]; do pfdaq_cap -r $opt -w 252 -f 1024 -n 32 | say "base_r/$mode"; i=$((i+1)); done
  for j in 1 2 3; do
    i=0; while [ $i -lt $N ]; do $MT $opt -w 252 -f 1024 -n 32 -j $j | say "mt/$mode/j$j"; i=$((i+1)); done
  done
done
# pinned threads (main on CPU 0, workers on CPUs 1, 2, 0), counter payload
for j in 2 3; do
  i=0; while [ $i -lt $N ]; do $MT -m counter -n 32 -j $j -p | say "mt_pinned/counter/j$j"; i=$((i+1)); done
done
# size of the buffer ring, three workers, counter payload
for b in 3 4 5 8 14; do
  i=0; while [ $i -lt $N ]; do $MT -m counter -n 32 -j 3 -b $b | say "mt_ring/counter/j3/b$b"; i=$((i+1)); done
done
# fault injection: the counters must equal what was injected, with three workers
for mm in $modes; do
  mode=${mm%%:*}; opt=$(echo ${mm#*:} | tr _ ' ')
  $MT $opt -w 252 -f 1024 -n 20 -j 3 -F crc -i 7 -e 5 | say "fault_crc/$mode/j3"
  $MT $opt -w 252 -f 1024 -n 20 -j 3 -F seq -i 7 -e 4 | say "fault_seq/$mode/j3"
  $MT $opt -w 252 -f 1024 -n 20 -j 3 -F payload -i 7 -e 10 | say "fault_payload/$mode/j3"
  pfdaq_cap $opt -w 252 -f 1024 -n 20 -F crc -i 7 -e 5 | say "fault_crc/$mode/orig"
  pfdaq_cap $opt -w 252 -f 1024 -n 20 -F seq -i 7 -e 4 | say "fault_seq/$mode/orig"
  pfdaq_cap $opt -w 252 -f 1024 -n 20 -F payload -i 7 -e 10 | say "fault_payload/$mode/orig"
done
