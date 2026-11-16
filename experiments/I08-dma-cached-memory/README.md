# I08: what the processor sees when the DMA writes memory it has cached

Belongs to module [I08, DMA into cached memory](../../docs/modules/I08-dma-cached-memory.md), which explains the method and the recorded results.

`cached_dma.c` is one file with no dependency on the project's tools. It takes a test area inside the 32 MiB regions that the kernel reserved for `udmabuf-ddr-c0` (cached window, `0x8800_0000`) and `udmabuf-ddr-nc0` (non-cached window, `0xC800_0000`), which are the same DDR bytes, and

1. puts the area into a state with known contents and known cache state (`-S cold|clean|dirty`),
2. points one burst of the fabric DMA at it, through the cached window (`-D c`, default) or the non-cached window (`-D nc`),
3. reads the area back through the cached mapping, and through the non-cached mapping (which shows what DDR holds), and counts every word as new (what the DMA wrote), old, pre (what DDR held before) or other,
4. repeats the readback after the processor has read 8 MiB elsewhere, which pushes the area's lines out of the caches.

Other modes: `-S control` (no DMA; shows the method can see stale cache lines), `-S partial` (a 24-byte DMA write into the middle of a modified line), `-S bench` (1 MiB bursts verified in place or after a copy-out), `-M sync` (the kernel's u-dma-buf `sync_for_cpu`), `-M evict` (read 8 MiB before and after the DMA), `-B` (read speed of the two mappings). `run_all.sh` runs the whole matrix.

## What it needs

- The shared baseline on the board: fabric v0.5.1 and the pfdaq image (UIO devices `pfdaq_stream_src` and `dma-controller@60010000`, u-dma-buf devices `udmabuf-ddr-c0` and `udmabuf-ddr-nc0`).
- Nothing else may use the stream source, the DMA controller or the first 1 MiB and offsets `0x40_0000` to `0x50_0000` and `0x100_0000` to `0x180_0000` of the two regions. Stop `pfdaq_cap` first.
- A RISC-V 64-bit Linux GCC on the PC; the program is linked statically.

## Build and run

```sh
make CROSS_COMPILE=riscv64-linux-gnu-
scp cached_dma run_all.sh root@<board>:/tmp/
ssh root@<board> 'cd /tmp && ./cached_dma -S dirty -f 16 -D c && ./cached_dma -S dirty -f 16 -D nc'
ssh root@<board> 'cd /tmp && sh run_all.sh 10'        # about one minute
```

The area is at offset `0x40_0000`, the descriptor at offset 0 of the non-cached region (where `pfdaq_cap` keeps its own). The program writes only inside those and inside the 8 MiB it reads at offset `0x100_0000`.

## Output

One JSON line per run; see the recorded runs in the module's evidence (`i08-cached-dma-figure-runs`, `i08-cached-dma-matrix`).
