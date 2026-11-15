# I04: one burst through the fabric DMA

The smallest complete use of the fabric DMA controller from Linux user space. It belongs to module [I04, DMA stream descriptors](../../docs/modules/I04-dma-stream-descriptors.md), which explains every step.

`dma_one_burst.c` is one file with no dependency on the project's tools. It

1. finds the stream source and the DMA controller among the UIO devices by name, and maps the non-cached DDR window through `/dev/udmabuf-ddr-nc0`;
2. writes one stream descriptor (byte count, destination, then the configuration word `0xD`) and gives its physical address to the DMA;
3. configures the stream source for 16 frames of 252 payload words (1 KiB each, counter payload) and sets START;
4. sleeps in `poll()`/`read()` until the DMA interrupt arrives;
5. prints the DMA status and the first frame, and checks header, sequence number and CRC-32 of all 16 frames.

## What it needs

- The shared baseline on the board: fabric v0.5.1 and the pfdaq image. The program relies on the device-tree nodes of that image (`pfdaq_stream_src`, `dma-controller@60010000`, `udmabuf-ddr-nc0`).
- A RISC-V 64-bit Linux GCC on the PC. The program is linked statically, so the compiler's C library does not have to match the board's.
- Nothing else may be using the stream source while it runs (stop `pfdaq_cap` first).

## Build

```sh
make CROSS_COMPILE=riscv64-linux-gnu-
```

If your toolchain has no static C library, link dynamically with `make CROSS_COMPILE=... LDFLAGS=`.

## Run

```sh
scp dma_one_burst root@<board>:/tmp/
ssh root@<board> /tmp/dma_one_burst
```

The exit status is 0 if the DMA completed without error and all frames are good.

## Output

Recorded on the board (evidence `dma-one-burst-run`), two runs in a row:

```text
DMA interrupt: event count 1, INTR_0_STAT 0x210, descriptor config 0x9 (complete, ready bit cleared by the DMA)
frame 0: header a55a00fc  seq 0  timestamp 3211942696  payload 00000000 00000001 00000002 ...  crc a9fba266
16 frames, 16384 bytes: 0 bad (header, sequence or CRC-32)
```

The second run printed `event count 2` and `seq 16`; the sequence number advances by 16 because the source never resets its counter.

## Things to try

- Run it twice: the sequence number of frame 0 advances by 16.
