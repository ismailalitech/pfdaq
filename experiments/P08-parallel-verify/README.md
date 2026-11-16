# P08: copy and verification spread over several Linux cores

Belongs to module [P08, raising verified throughput with more cores](../../docs/modules/P08-parallel-verify.md), which gives the measured before and after.

`pfdaq_cap_mt.c` is `pfdaq_cap`'s burst loop with the copy and the verification moved to a pool of worker threads:

- the **main thread** arms a burst in a free buffer of a ring in the non-cached DMA region, starts the fabric and waits for the DMA interrupt, then hands the finished burst to the workers;
- each **worker** copies a finished burst out of its non-cached buffer, gives the buffer back to the ring, and verifies its private copy (magic, sequence number, payload against the model of the mode, CRC-32);
- the checks, the three payload models, the fault injection (`-F -i -e`) and the pass rule are copied from `pfdaq_cap.c` (`verify`, `payload_bad`, `crc32_words`, the counter expectations). Not copied: supervision over RPMsg (`-r`), real-time mode, the abort test and the shared-memory test.

Options: `-j workers` (default 1), `-b buffers` in the ring (default workers + 2), `-p` to pin the main thread to CPU 0 and the workers to CPUs 1, 2, 0, and the capture options `-n -f -w -m -S -F -i -e` with the meaning they have in `pfdaq_cap`. One JSON line is printed, with `workers`, `buffers`, `cpu_s` (CPU time of all threads) and `dma_blocked_on_buffers_s` (time the main thread waited for a free buffer) in addition to the fields of `pfdaq_cap`.

## What it needs

- The shared baseline on the board: fabric v0.5.1 and the pfdaq image. Like `pfdaq_cap` it uses the UIO devices `pfdaq_stream_src` and `dma-controller@60010000` and the non-cached buffer `udmabuf-ddr-nc0`, so nothing else may use them while it runs.
- A RISC-V 64-bit Linux GCC on the PC; the program is linked statically (with `-lpthread`).

## Build and run

```sh
make CROSS_COMPILE=riscv64-linux-gnu-
scp pfdaq_cap_mt run_all.sh root@<board>:/tmp/
ssh root@<board> '/tmp/pfdaq_cap_mt -m counter -w 252 -f 1024 -n 32 -j 3'
ssh root@<board> 'sh /tmp/run_all.sh 8'              # the whole matrix, about five minutes
```

The exit status is 0 if the run passed (all bursts verified, the error counters equal what was injected).
