# I09: the fabric DMA against the MSS DMA

Belongs to module [I09, Fabric DMA against the MSS DMA](../../docs/modules/I09-fabric-dma-vs-mss-dma.md), which explains the method and the recorded results.

`dma_compare.c` is one file with no dependency on the project's tools. It moves the same number of bytes into the same kind of DDR buffer with three different movers and reports time, throughput and processor use:

| `-m` | mover | what is moved |
|---|---|---|
| `fabric` | the CoreAXI4DMAController in the fabric, armed with a stream descriptor as `pfdaq_cap` does it | a burst of 1 KiB frames from the stream source (counter payload), `-b` bytes |
| `pdma` | the MSS DMA (PDMA, 4 channels), through the kernel's `/dev/dma-proxy0..3` devices | `-b` bytes from a buffer in window `-R` to a buffer in window `-D` |
| `memcpy` | the processor, `memcpy()` | the same copy |
| `idle` | nothing | the floor of the processor-use figure |

The buffers are inside the three 32 MiB regions that the device tree reserves for u-dma-buf: `udmabuf-ddr-c0` (cached window), `udmabuf-ddr-nc0` (non-cached) and `udmabuf-ddr-nc-wcb0` (non-cached, write-combining). They are the same DDR bytes seen through three windows. Window letters: `c`, `nc`, `wcb`.

Per configuration it

1. pushes whatever earlier programs left in the caches out (reads 8 MiB elsewhere),
2. fills the destination with one pattern and the source with another, does **one** transfer and checks every word, through the destination's own window and, after another 8 MiB read, through the non-cached window,
3. repeats the transfer for at least `-t` seconds (default 1.0) and at least `-n` times (default 8), recording the wall time and the CPU time of the program's thread around each transfer, and the idle time of all CPUs over the loop,
4. prints one JSON line.

Options: `-c N` (pdma only) moves N buffers on N channels at the same time, `-C` reads 8 MiB before every transfer (untimed) so that the source is not in the caches, `-V` skips the check.

## What it needs

- The shared baseline on the board: fabric v0.5.1 and the pfdaq image (UIO devices `pfdaq_stream_src` and `dma-controller@60010000` for the fabric mover; u-dma-buf devices; and for the PDMA the kernel's `sf-pdma` driver with the `mpfs-dma-proxy` devices `/dev/dma-proxy0..3`, `CONFIG_MPFS_DMA_PROXY=y`, which this image has).
- Nothing else may use the stream source, the DMA controller or the PDMA channels, nor the offsets `0x40_0000` to `0x80_0000` (destinations), `0x100_0000` to `0x140_0000` (sources) and `0x180_0000` to `0x200_0000` (the 8 MiB read) of the three regions. Stop `pfdaq_cap` first.
- A RISC-V 64-bit Linux GCC on the PC; the program is linked statically.
- Nothing is installed or changed on the board: no module, no device-tree change, no sysfs write. The program only opens devices and writes into the reserved regions.

## Build and run

```sh
make CROSS_COMPILE=riscv64-linux-gnu-
BOARD=root@pfdaq-board
scp dma_compare run_all.sh board_state.sh $BOARD:/tmp/
ssh $BOARD 'sh /tmp/board_state.sh'                      # read-only: what the board offers (device tree, drivers, config)
ssh $BOARD 'cd /tmp && ./dma_compare -m pdma -b 1048576 -R nc -D nc'
ssh $BOARD 'cd /tmp && ./dma_compare -m fabric -b 1048576 -D nc'
ssh $BOARD 'cd /tmp && ./dma_compare -m memcpy -b 1048576 -R nc -D nc'
ssh $BOARD 'cd /tmp && nohup sh run_all.sh 5 > /root/i09-matrix.txt 2>&1 < /dev/null &'   # the whole matrix, about 2 minutes per pass
```

Read `/root/i09-matrix.txt` only after the matrix has finished: a login while it runs adds work on the processors and changes the numbers. On the PC:

```sh
python3 summarise.py matrix.txt > summary.txt
```

## Output

One JSON line per run: `mb_per_s`, `ms_per_mib`, `ms_mean`/`ms_median`/`ms_min`/`ms_max` per transfer, `cpu_ms_mean` and `cpu_share` (the program's thread on a CPU, per transfer and as a fraction of the transfer time), `non_idle_cores` (online cores minus their idle time over the loop, which includes interrupt handling and wake-ups outside the program), the number of wrong words in the check, and `fails`. See the recorded runs in the module's evidence.
