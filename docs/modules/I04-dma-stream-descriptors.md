---
id: I04
title: "DMA stream descriptors: the rules this core really follows"
track: integration
level: 3
summary: How Linux hands a destination to the DMA controller, and the rules the user guide does not state.
needs: [H02, I01]
repro: base
status: validated
hardware: [dma, ddr-noncached, pfdaq-stream-src]
software: [pfdaq-cap, uio, udmabuf]
sources:
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:arm,wait_done,open_uio,DMA_STREAM0,DESC_CFG_ARMED
  - experiments/I04-dma-one-burst/dma_one_burst.c:DESC_ARMED,DMA_STREAM0
  - yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts
evidence: [suite-freertos, dma-one-burst-run]
related: [I05, I07, K03, K04, K09, K10, P05]
---

# DMA stream descriptors: the rules this core really follows

After this module you can make the fabric DMA controller write an AXI4-Stream packet into a buffer of your choice from a Linux user program: prepare a descriptor, tell the DMA where it is, start the stream, sleep until the interrupt, and decide correctly whether the transfer finished. You will also know four rules this DMA core follows in practice, only some of which are written in its user guide.

## Before you start

- **Level 3.** You should know the AXI4-Stream handshake (module H02) and how a Linux program reaches fabric registers through a UIO device (module I01).
- **Board:** the shared baseline, fabric v0.5.1 with the pfdaq image.
- **Tools:** a RISC-V 64-bit Linux C compiler on your PC, and SSH access to the board.

## The idea

A DMA controller copies data without the processor touching each byte. The processor's part is to say *where the data goes* and *how much of it there is*, and to be told when the copy is done. That instruction is a **descriptor**: a small record with a destination address, a length and a few control bits.

For a stream, the data arrives on its own schedule from a source in the fabric. So the order of events matters. The descriptor must exist, and the DMA must know where to find it, before the first beat of the stream arrives. From then on the DMA works alone, and software waits for one interrupt.

The general background is on the concept page for direct memory access (C01).

## On this board

The DMA is Microchip's CoreAXI4DMAController, version 2.2.107, in the fabric. Four things take part in one transfer:

| Part | Where | Role |
|---|---|---|
| DMA registers | `0x6001_0000`, through FIC0 | Linux tells the DMA where the descriptor is and enables its interrupt |
| Stream descriptor | in DDR, at the start of the non-cached window `0xC800_0000` | destination, byte count, control bits; the DMA fetches it over its AXI4 master port |
| Data buffer | in the same window, one page further | where the frames land |
| Interrupt | fabric line F2M[2], PLIC source 120 | wakes Linux when the transfer is complete |

### The descriptor

A stream descriptor is three 32-bit words in memory. The layout is from the CoreAXI4DMAController User Guide (DS50003714A), section "Stream Descriptor Support":

| Offset | Word | Content |
|---|---|---|
| `+0x0` | Configuration | bits [1:0] destination operation (`01` = incrementing address), bit 2 Destination Data Ready, bit 3 Descriptor Valid |
| `+0x4` | Byte count | length of the transfer, at most 8 MB |
| `+0x8` | Destination address | physical address of the buffer |

The value this project writes to the configuration word is `0xD`: valid, destination ready, incrementing address. The DMA clears the Destination Data Ready bit itself when the stream data has been written to memory, so bit 2 reads 0 afterwards. That change is one of the two completion signals used below.

The descriptor is *external*: it lives in ordinary memory, not in the DMA. The DMA is told its address through one of four registers, selected by the `TDEST` value of the incoming stream:

| Register | Offset | Used for |
|---|---|---|
| `STREAM_0_ADDR_REG` | `0x460` | packets with `TDEST` = 0 |
| `STREAM_1_ADDR_REG` | `0x464` | `TDEST` = 1 |
| `STREAM_2_ADDR_REG` | `0x468` | `TDEST` = 2 |
| `STREAM_3_ADDR_REG` | `0x46C` | `TDEST` = 3 |

Burst mode uses only the first. Real-time mode keeps all four armed (module I07). The guide requires the descriptor's address to be aligned to the width of the DMA's data bus, 8 bytes here.

### Where the descriptor and the buffer live

Both sit in a 32 MiB region of DDR that the device tree reserves at `0xC800_0000` and that Linux maps **without caching**. User space reaches it through the u-dma-buf device `/dev/udmabuf-ddr-nc0`, and reads the region's physical address from `/sys/class/u-dma-buf/udmabuf-ddr-nc0/phys_addr`.

Two addresses are in play, and mixing them up is the classic first mistake. The program writes the descriptor through its own *virtual* mapping. What it writes *into* the descriptor, and into `STREAM_0_ADDR_REG`, are *physical* addresses, because the DMA is hardware on the bus and knows nothing about Linux's address spaces.

Non-cached memory is used so that software reads exactly what the DMA wrote, with no cache maintenance. What changes with cached buffers is the subject of module I05.

### Arming

This is the whole of `arm()` in `pfdaq_cap.c`:

```c
volatile uint32_t *desc = (volatile uint32_t *)(ddr + DESC_OFF);
desc[1] = nbytes;
desc[2] = (uint32_t)(ddr_phys + dest_off);
desc[0] = DESC_CFG_ARMED;                             /* 0xD */
dma[DMA_STREAM0 / 4] = (uint32_t)(ddr_phys + DESC_OFF);
```

The order is deliberate. Byte count and destination are written first and the configuration word last, so the descriptor never looks valid while it is half written. Only then does the DMA learn its address. The stream source is started after this, never before.

The whole transfer, in the order the program does it:

![Order of one DMA transfer: Linux writes the descriptor in DDR (byte count, destination, then 0xD last), writes its physical address to STREAM_0_ADDR_REG of the DMA, and only then starts the stream source; the source sends one packet to the DMA, which fetches the descriptor, writes the burst into the data buffer, clears the descriptor's ready bit and raises the interrupt on F2M[2], PLIC source 120; the blocking read on the DMA's UIO device returns and Linux checks the status and the ready bit.](../diagrams/lab-i04-arm-start-wait.svg)

1. Linux writes the descriptor into DDR: byte count, then destination, then the configuration word `0xD` last.
2. Linux writes the descriptor's physical address to `STREAM_0_ADDR_REG` (`0x460`).
3. Only then does Linux start the source, with `SRC_CTRL` = `0x1`.
4. The source sends the burst as one packet with no `TVALID` gap in it. The DMA fetches the descriptor and writes the stream into the data buffer.
5. When the full byte count has arrived, the DMA clears the descriptor's ready bit and raises its interrupt, F2M[2], PLIC source 120.
6. Linux, asleep in `read()` on the DMA's UIO device, wakes, and checks the error bits and the ready bit.

### Waiting for completion

The DMA's interrupt output is wired to fabric-to-MSS line F2M[2], which is PLIC source 120. The vendor's device tree describes the DMA as `dma-controller@60010000` with that interrupt, and the kernel driver `uio-microchip-dma` turns it into a UIO device. A user program therefore waits for the DMA with an ordinary blocking `read()` on `/dev/uioN`, which returns a 4-byte event count when the interrupt has fired.

Before the first transfer, `pfdaq_cap` writes `0xF` to `INTR_0_MASK_REG` (`0x14`), which selects the status flags that may raise the interrupt, and `0xF` to `INTR_0_CLEAR_REG` (`0x18`) to discard old events.

Deciding that a transfer *succeeded* takes more care than waiting for the wake-up. This is `wait_done()`:

```c
if (poll(&p, 1, timeout_ms) <= 0)   return -1;     /* no interrupt in time   */
if (read(dma_fd, &cnt, 4) != 4)     return -1;
*stat = dma[DMA_STAT / 4];
dma[DMA_CLEAR / 4] = 0xF;
if (*stat & 0xE)                    return -1;     /* DMA reported an error  */
if (desc[0] & 0x4)                  return -1;     /* ready bit still set    */
return 0;
```

In `INTR_0_STAT_REG` (`0x10`), bit 0 (`OPS_COMPL`) is the completion flag, and the user guide defines bits 1 to 3 as a write error, a read error and an invalid descriptor. On this system the complete flag cannot be used: the kernel driver's interrupt handler clears it before user space runs. What user space reads is `0x210`, which is descriptor number 33 in bits 9:4, the guide's code for "stream operation complete", with all four flag bits zero. So completion is taken from two things together: the UIO interrupt arrived, and the DMA cleared the descriptor's Destination Data Ready bit. The error bits are still checked.

### The rules

| # | Rule | How it is known |
|---|---|---|
| 1 | **Arm first, then START.** The descriptor must be valid in memory and its address written to the stream address register before the stream's first beat. | Vendor document, and board test: a descriptor re-armed while a packet was already waiting was not picked up, and a changed destination was ignored (case study K04). |
| 2 | **One burst is one packet is one DMA transfer**, at most 8 MB. | Vendor document for the 8 MB limit. Board test up to 8,192,000 bytes per transfer. |
| 3 | **An early `TLAST` does not complete a transfer.** The DMA finishes when the descriptor's full byte count has arrived, not when the packet ends. | Board test only. An aborted burst that ended its packet early left the DMA waiting (case study K09). The source now pads an aborted burst with zero words up to the armed length. |
| 4 | **No `TVALID` gap inside a packet.** | Board test only. A source that offered a beat every second clock corrupted packets above about 360 bytes and then hung the DMA (case study K03). The AXI4-Stream standard allows such gaps. |
| 5 | **All byte lanes valid on every beat except the last** (`TKEEP` = `TSTRB` = all ones). | Vendor document. |
| 6 | **Completion is the interrupt plus the cleared ready bit**, not the status flag. | Kernel driver source and board observation. |

Rules 3 and 4 are properties of this core version as integrated here, found by experiment. They are not statements about AXI4-Stream or about DMA controllers in general.

One defect is open: with an odd number of 32-bit words per frame and several frames in one transfer, data is lost at destination boundaries, although the stream is correct in simulation. `pfdaq_cap` refuses that combination, and case study K10 records what is known.

## Run it

The experiment is one C file with no dependency on the project's tools: `experiments/I04-dma-one-burst/dma_one_burst.c`. It arms one descriptor, starts one burst of 16 frames of 1 KiB, sleeps until the DMA interrupt, prints the first frame and checks the CRC-32 of all sixteen.

1. Build it on your PC. The program is linked statically, so any RISC-V 64-bit Linux GCC will do:

   ```sh
   make -C experiments/I04-dma-one-burst CROSS_COMPILE=riscv64-linux-gnu-
   ```

2. Copy it to the board and run it as root:

   ```sh
   scp experiments/I04-dma-one-burst/dma_one_burst root@<board>:/tmp/
   ssh root@<board> /tmp/dma_one_burst
   ```

3. Run it a second time and compare the sequence number and timestamp of frame 0 with the first run.

## What you should see

Recorded on the board (`dma-one-burst-run`), two runs one after the other:

```text
DMA interrupt: event count 1, INTR_0_STAT 0x210, descriptor config 0x9 (complete, ready bit cleared by the DMA)
frame 0: header a55a00fc  seq 0  timestamp 3211942696  payload 00000000 00000001 00000002 ...  crc a9fba266
16 frames, 16384 bytes: 0 bad (header, sequence or CRC-32)

DMA interrupt: event count 2, INTR_0_STAT 0x210, descriptor config 0x9 (complete, ready bit cleared by the DMA)
frame 0: header a55a00fc  seq 16  timestamp 3332262599  payload 00100000 00100001 00100002 ...  crc 14ed3c30
16 frames, 16384 bytes: 0 bad (header, sequence or CRC-32)
```

![Recorded output of two runs of the one-burst program on the board: each reports the DMA interrupt with INTR_0_STAT 0x210 and descriptor configuration word 0x9, the first frame, and 16 frames of 16384 bytes with 0 bad; the second run starts at sequence 16 and its payload begins 0x00100000.](../diagrams/lab-i04-one-burst-run.svg)

1. The `read()` returned (event count 1), the configuration word reads `0x9` (the DMA cleared the ready bit), and `INTR_0_STAT` is `0x210` with no error bits.
2. Frame 0 has header `a55a00fc`, sequence 0 and the counter payload 0, 1, 2 ...
3. All 16 frames pass the header, sequence and CRC-32 checks: 0 bad.
4. In the second run the sequence number continues at 16, and the payload starts with `0x0010` followed by the word index.

- `INTR_0_STAT` reads `0x210` (stream operation complete, no error bits), the same value `pfdaq_cap` sees.
- The descriptor's configuration word reads `0x9` afterwards: the DMA cleared bit 2, the ready bit, when it used the descriptor.
- The header `a55a00fc` is the magic value with the payload length 252.
- The second run's frame 0 has sequence number 16, because the source never resets its sequence counter, and its first payload word is `0x00100000`, not 0: in the counter payload mode word *i* of a frame is the low 16 bits of the sequence number followed by *i* (`hw/rtl/pfdaq_stream_src.v`), and 16 is `0x0010`. All 16 frames of both runs passed the header, sequence and CRC checks.

## Do the same in your project

This is the minimum to land an AXI4-Stream packet from your own fabric source in a buffer that a Linux program reads. `experiments/I04-dma-one-burst/dma_one_burst.c` is the template: 138 lines, no dependency on the project's tools.

1. **Check what the core requires of your source.** `TVALID` held high through the packet with no gap, `TKEEP` = `TSTRB` = all ones on every beat except the last, and a packet whose length equals the descriptor's byte count (rules 2 to 5). An early `TLAST` does not complete a transfer. Give your source's `TDEST` the stream number whose address register you arm; `TDEST` 0 uses `STREAM_0_ADDR_REG`.
2. **Reserve a buffer the DMA can write and Linux can read without cache maintenance.** Use a non-cached window and u-dma-buf as here (module I05): `/dev/udmabuf-ddr-nc0`, with its physical address in `/sys/class/u-dma-buf/udmabuf-ddr-nc0/phys_addr`. Put the descriptor at the start, 8-byte aligned, and the data one page later (`DESC_OFF` 0, `BUF_OFF` 0x1000).
3. **Open the devices by name.** Copy `open_uio()` and change the names: `pfdaq_stream_src` becomes your block's UIO name, and `dma-controller@60010000` is the DMA's UIO device, whose interrupt arrives as F2M[2] (module F03). Change the source's register offsets (`SRC_CTRL`, `SRC_PAYLOAD`, `SRC_FRAMES` and the rest) to yours.
4. **Prepare the DMA once.** Write `0xF` to `INTR_0_MASK_REG` (`0x14`) and to `INTR_0_CLEAR_REG` (`0x18`), then drain any completion left from an earlier program with `poll()` and `read()`.
5. **Arm, in this order.** `desc[1]` = byte count; `desc[2]` = the buffer's physical address; `desc[0]` = `0xD` last; then the descriptor's physical address into `STREAM_0_ADDR_REG` (`0x460`). Virtual addresses go nowhere near the descriptor.
6. **Start the source after arming, then sleep.** Write your source's `START`, then `poll()` and `read()` on the DMA's UIO device with a timeout.
7. **Decide success from two signals.** `INTR_0_STAT` & `0xE` must be 0 (the error bits) and the descriptor's configuration word must have bit 2 clear. Do not wait for the complete flag; the kernel driver has already cleared it. Write `0xF` to `INTR_0_CLEAR_REG` afterwards.
8. **Check what arrived.** Verify every frame in software, as the program does (header, sequence number, CRC-32), and make the byte count in step 5 equal what the source will send: frames × (payload words + 4) × 4, as `bytes` is computed in the program. Use an even number of 32-bit words per frame when a transfer holds several frames (case study K10).
9. **Build and run the template first,** as in "Run it", and compare its output with the recorded one. Then change one thing at a time.

What not to change:

- **Arm before START.** A descriptor re-armed while a packet is already waiting is not picked up (case study K04).
- **Configuration word last.** The descriptor must never look valid while half written.
- **Physical addresses** in the descriptor and in `STREAM_0_ADDR_REG`.
- **One transfer is at most 8 MB.**
- **Do not depend on an early `TLAST`** to end a transfer. An aborted burst has to be padded up to the armed length (case study K09).
- **Do not read the buffer through a cached mapping** unless you add the cache maintenance (module I05).

## Evidence

- **Board, fabric v0.5.1:** the descriptor handling described here is the code path of every burst test in the board suite. It passes 27 of 27 functional tests, including four transfers of 8,192,000 bytes each (test `T5-max-burst`) and bursts of 64 one-word frames (`T5-min-frame`), with no sequence, payload or CRC error.
- **Board, rule 3:** in the abort tests (`ABORT-2ms`, `ABORT-50us`) the DMA completes only after the zero fill has brought the transfer to its armed length, and nothing is written past the buffer.
- **Board, the minimal experiment (`dma-one-burst-run`):** two runs, 16 frames of 1 KiB each, no bad frame, as shown above. The timestamps are the stream source's own 125 MHz counter.

Rules 1, 3 and 4 were established on fabric versions before v0.5.1, by failures that the current design avoids. The case studies give those observations.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| `read()` on the DMA device never returns | The descriptor was armed after the stream started (rule 1), or the packet is shorter than the armed byte count (rule 3) |
| Interrupt arrives, `INTR_0_STAT` has bit 3 set | The descriptor was not valid when fetched: wrong address in `STREAM_0_ADDR_REG`, or a virtual address written where a physical one is needed |
| Interrupt arrives, buffer still holds old data | The destination word holds the wrong address, or the buffer is read through a cached mapping |
| `INTR_0_STAT` reads `0x210` and you conclude "no completion flag, so it failed" | Expected here: the kernel driver has already cleared the flag. Check the descriptor's ready bit instead |
| A stale wake-up at the start of a run | A completion left over from an earlier program. Drain the UIO device before the first transfer, as `pfdaq_cap` does |
| Frames correct at first, then 2 KB of data missing | Odd frame length in a multi-frame transfer (case study K10) |

## Trade-offs and going further

- **External descriptors in DDR, not internal ones.** The core also has descriptors in its own registers. Stream transfers use the external kind, which costs one extra memory read by the DMA per packet and gives software a plain memory record to prepare.
- **One transfer per burst** keeps the interrupt rate low, at the price of latency: software sees nothing until the whole burst has landed. Real-time mode makes each frame its own transfer over four descriptors (module I07).
- **Completion from two signals** depends on the vendor driver clearing the flag. A different kernel driver could leave the flag for user space, and the check would be simpler.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md)<br>[I01 Reaching fabric registers from Linux](I01-fabric-registers-from-linux.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [CoreAXI4DMAController](../system/README.md#dma) · [Non-cached window](../system/README.md#ddr-noncached) · [Stream source](../system/README.md#pfdaq-stream-src) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [UIO devices](../system/README.md#uio) · [u-dma-buf](../system/README.md#udmabuf) |
| **Source** | [`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `arm`, `wait_done`, `open_uio`, `DMA_STREAM0`, `DESC_CFG_ARMED`<br>[`experiments/I04-dma-one-burst/dma_one_burst.c`](../../experiments/I04-dma-one-burst/dma_one_burst.c): `DESC_ARMED`, `DMA_STREAM0`<br>[`yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts`](../../yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts) |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[One 16-frame burst through the fabric DMA from a 138-line user-space program, run twice](../../evidence/v0.5.1/board/dma-one-burst.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [I05 Buffers the DMA can write: non-cached windows and u-dma-buf](I05-dma-buffers.md)<br>[I07 Real-time capture with four descriptors and credits](I07-real-time-capture.md)<br>[K03 A DMA core that needs TVALID without gaps](../case-studies/K03-tvalid-gaps.md)<br>[K04 The descriptor that was re-armed too late](../case-studies/K04-descriptor-rearmed-too-late.md)<br>[K09 An early TLAST does not end a transfer](../case-studies/K09-early-tlast.md)<br>[K10 Odd frame lengths lose 2 KB chunks](../case-studies/K10-odd-frame-lengths.md)<br>[P05 Abort without wedging the DMA](P05-abort.md) |
| **Status** | validated |
<!-- nav:end -->
