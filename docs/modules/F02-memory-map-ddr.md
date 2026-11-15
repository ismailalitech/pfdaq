---
id: F02
title: "The memory map and the three faces of DDR"
track: fundamentals
level: 1
summary: One physical memory, several address windows with different cache behaviour, and who owns which region.
needs: [F01]
repro: base
status: validated
hardware: [ddr, ddr-noncached, ddr-cached-shared, ddr-rpmsg, ddr-u54-4-image, u54-linux, u54-4]
software: [udmabuf, pfdaq-cap]
sources:
  - yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts:contextb_reserved,vdev0vring0,vdev0vring1,vdev0buffer,rsctable
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:arm,run_shm,ddr_phys
evidence: [boot-hss, boot-uboot-linux, suite-freertos, suite-freertos-raw]
related: [F01, F03, I04, M05, K04]
---

# The memory map and the three faces of DDR

After this module you can read an address on this board and say which memory it is, whether the processor caches it, and who is allowed to use it. You will also know why the DMA buffers of this project live in a window the processor does not cache, and what that costs.

## Before you start

- **Level 1.** Module F01 introduces the cores and the interfaces.
- **Board:** the shared baseline (fabric v0.5.1 with the pfdaq image) if you want to repeat the checks; otherwise this is a reading page with recorded results.

## The idea

An address is not a place. It is a name the processor sends on a bus, and the hardware decides what answers. On this chip the **same physical DDR bytes answer to several different addresses**, and the address you use chooses how the access behaves:

| Window | Behaviour | Good for |
|---|---|---|
| **Cached** | the access goes through the processor's caches | code and data that only processors touch |
| **Non-cached** | every access goes to DDR itself, and nothing is kept in a cache | memory that hardware other than the processor writes or reads, such as a DMA buffer |
| **Non-cached, write-combining** | like non-cached, but the processor may merge neighbouring writes into one burst | large writes to memory that hardware reads |

The reason to care is the DMA. When a DMA controller writes into memory, the processor's cache does not hear about it. If the processor then reads through a cached window, it can be handed an old copy of what it read before. Reading through a non-cached window cannot: it always goes to the DDR.

## On this board

### What the boot monitor sets up

The boot monitor prints the windows it configures, and the board records them:

```text
[0.860312] Segment Configuration:
        Cached: SEG0_0: offset 0x0080000000, physical DDR 0x00000000
        Cached: SEG0_1: offset 0x1000000000, physical DDR 0x00000000
    Non-cached: SEG1_2: offset 0x00c0000000, physical DDR 0x00000000
    Non-cached: SEG1_3: offset 0x1400000000, physical DDR 0x00000000
Non-cached WCB: SEG1_4: offset 0x00d0000000, physical DDR 0x00000000
Non-cached WCB: SEG1_5: offset 0x1800000000, physical DDR 0x00000000
```

Read it as a table. Each kind of window appears twice, once at a 32-bit address and once at a 64-bit one, and **all six point at physical DDR address 0**:

| Behaviour | 32-bit window starts at | 64-bit window starts at |
|---|---|---|
| Cached | 0x0000_8000_0000 | 0x10_0000_0000 |
| Non-cached | 0x0000_C000_0000 | 0x14_0000_0000 |
| Non-cached WCB | 0x0000_D000_0000 | 0x18_0000_0000 |

"WCB" is the monitor's label; I read it as write-combining buffer. So physical byte N of the DDR is at `0x8000_0000 + N` cached, at `0xC000_0000 + N` non-cached and at `0xD000_0000 + N` write-combining.

### The three buffers the vendor reserved

The Discovery Kit's device tree reserves one 32 MiB buffer in each of the three windows and gives each a name through the u-dma-buf driver:

| Device name | Address | Window |
|---|---|---|
| `udmabuf-ddr-c0` | 0x8800_0000 | cached |
| `udmabuf-ddr-nc0` | 0xC800_0000 | non-cached |
| `udmabuf-ddr-nc-wcb0` | 0xD800_0000 | non-cached WCB |

Subtract each window's base and the three buffers have the same offset, 0x0800_0000. Taken with the segment table, that means they are the same 32 MiB of physical DDR seen three ways. That is arithmetic on the recorded table and the vendor's addresses; this project did not test the aliasing directly, and it only ever uses the first two.

### Everything else Linux must leave alone

The project's device tree reserves five more regions so that Linux never maps them (`no-map`):

| Address | Size | Owner | What it holds |
|---|---|---|---|
| 0x91C0_0000 | 1 MiB | hart 4 | the supervisor's code and data |
| 0x91D0_0000 | 32 KiB | Linux and hart 4 | RPMsg ring 0 (`vdev0vring0`) |
| 0x91D0_8000 | 32 KiB | Linux and hart 4 | RPMsg ring 1 (`vdev0vring1`) |
| 0x91D1_0000 | 256 KiB | Linux and hart 4 | RPMsg message buffers (`vdev0buffer`) |
| 0x91D5_0000 | 4 KiB | Linux and hart 4 | the resource table (`rsc-table`) |

Linux's own view is smaller than the 1 GB on the board: the console says `Memory: 552904K/1048576K available`. The project's notes record that the vendor's baseline image also saw only about 545 MB, so that figure comes from the vendor's device-tree memory layout, which this page does not analyse. The boot monitor also keeps a 2 MiB buffer of its own at 0x10_3FC0_0000 (it prints `Copying 1376656 bytes to 0x103fc00000` when it loads the payload from the SD card).

### Who uses what

| Window / region | Used by | How |
|---|---|---|
| `udmabuf-ddr-nc0` (0xC800_0000) | the fabric DMA and Linux | the DMA writes descriptors and frames; Linux maps the buffer through `/dev/udmabuf-ddr-nc0` and reads |
| `udmabuf-ddr-c0` (0x8800_0000) | Linux and hart 4 | the shared-memory test only |
| 0x91C0_0000 and the RPMsg regions | hart 4 and Linux | message passing and the supervisor image |
| register windows (0x4000_0F00, 0x6001_0000, 0x5000_0000) | processors | not memory: these are fabric registers (module F01); the stream source and the DMA are reached from Linux through UIO devices |

### What the capture tool does with them

`pfdaq_cap` finds the non-cached buffer's physical address from sysfs, maps it, and gives the DMA the *physical* address in the descriptor. This is all of `arm()`:

```c
volatile uint32_t *desc = (volatile uint32_t *)(ddr + DESC_OFF);
desc[1] = nbytes;
desc[2] = (uint32_t)(ddr_phys + dest_off);
desc[0] = DESC_CFG_ARMED;
dma[DMA_STREAM0 / 4] = (uint32_t)(ddr_phys + DESC_OFF);
```

Two addresses of the same buffer are in play. `ddr` is the program's own virtual address, used to write the descriptor and read the frames. `ddr_phys` is what the hardware sees, and is what goes into the descriptor and into the DMA's stream address register. Mixing them up is the classic first mistake; module I04 goes through it.

The price of the non-cached window is stated in a comment in the tool itself: *"non-cached DDR is slow to read: copy each burst once into normal memory, verify the copy"*. The recorded run copied at 70.5 MB/s out of the non-cached window with one core, and verified the copy at 55.4 MB/s (module X02 has the whole run).

## Run it

These are the commands the project ran; the board is the only thing that can run them, and the results are in the evidence below.

1. Where the buffers are: `cat /sys/class/u-dma-buf/udmabuf-ddr-nc0/phys_addr` and the same for `udmabuf-ddr-c0` (the capture tool reads exactly these files).
2. Memory seen by both Linux and hart 4, non-cached and cached, from the board suite:

   ```sh
   pfdaq_cap -X 65536 -n 50        # non-cached
   pfdaq_cap -X 65536 -n 50 -c     # cached
   ```

   Linux writes a pattern of 65,536 words into the region; hart 4 verifies it through the RPMsg channel and writes the inverse; Linux verifies that.

## What you should see

From the recorded suite (50 repetitions of 65,536 words, FreeRTOS on hart 4):

| Region | Mismatches seen by hart 4 / by Linux | Mean time per check |
|---|---|---|
| `udmabuf-ddr-nc0` (non-cached) | 0 / 0 | 31.15 ms |
| `udmabuf-ddr-c0` (cached) | 0 / 0 | 20.10 ms |

Both pass with **no cache maintenance** in the code. The tests ran at physical addresses 0xC9F0_0000 (non-cached) and 0x8810_0000 (cached), in different parts of the 32 MiB buffers.

Two things to read from this. First, hart 4 and Linux saw each other's writes in the cached region without flushing, which is what the project observed on this chip; the vendor documents were not used to prove why. Second, the cached check is faster, but each check includes one RPMsg exchange, so the figure is a cost per check and not a memory bandwidth.

## Evidence

- **Segment table and cache sizes:** the boot monitor console (board, fabric v0.5.1).
- **Linux memory and the RPMsg link:** the U-Boot and Linux console, which shows the memory line and `remoteproc` attaching.
- **Shared memory, both windows:** rows `T7-noncached` and `T7-cached` of the board suite, with their raw JSON (board, fabric v0.5.1, single runs of 50 repetitions).

The records show that both windows work for sharing between Linux and hart 4. They do not show what happens when the fabric DMA writes a **cached** buffer: that test was not done, and it is the subject of a planned module.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| The DMA completes but the program reads old data | the buffer is read through a cached window while hardware wrote it, or a physical and a virtual address were swapped in the descriptor |
| The capture tool cannot open `/dev/udmabuf-ddr-nc0` | the device tree of the booted image has no such node (the image is not the pfdaq one) |
| Linux and the supervisor disturb each other's memory | a region one side uses is not reserved (`no-map`) from the other; the project reserves the five regions above for this reason |
| Reading a non-cached buffer is slow | expected: that is the cost; copy it once into ordinary memory and work on the copy |

## Trade-offs and going further

- **Non-cached or cached for a DMA buffer.** Non-cached makes the program correct without cache maintenance and costs read speed. Cached gives speed and needs flush or invalidate around every transfer, and a missed call gives stale data that looks plausible. This project chose non-cached for the stream buffers and measured the price.
- **Write-combining** exists for large writes by the processor to memory that hardware reads. This project does not use it.
- **Exercise:** work out the physical address behind `0xC9F0_0000` and the address at which the cached window shows the same byte.
- **Exercise:** how big must the non-cached buffer be for the 1 MiB bursts of module X02 with two buffers in flight? Compare your answer with the layout in `pfdaq_cap`.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [F01 The chip and the board: MSS, fabric and the interfaces between them](F01-chip-and-board.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [DDR4 memory, 1 GB](../system/README.md#ddr) · [Non-cached window](../system/README.md#ddr-noncached) · [Cached shared region](../system/README.md#ddr-cached-shared) · [RPMsg shared memory](../system/README.md#ddr-rpmsg) · [Hart 4 image](../system/README.md#ddr-u54-4-image) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [u-dma-buf](../system/README.md#udmabuf) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts`](../../yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts): `contextb_reserved`, `vdev0vring0`, `vdev0vring1`, `vdev0buffer`, `rsctable`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `arm`, `run_shm`, `ddr_phys` |
| **Evidence** | [Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [F01 The chip and the board: MSS, fabric and the interfaces between them](F01-chip-and-board.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md)<br>[I04 DMA stream descriptors: the rules this core really follows](I04-dma-stream-descriptors.md)<br>[M05 Shared memory between Linux and a bare-metal hart](M05-shared-memory-linux-bare-metal.md)<br>[K04 The descriptor that was re-armed too late](../case-studies/K04-descriptor-rearmed-too-late.md) |
| **Status** | validated |
<!-- nav:end -->
