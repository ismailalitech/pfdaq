---
id: M05
title: "Shared memory between Linux and a bare-metal hart"
track: multicore
level: 3
summary: Two contexts reading and writing the same DDR region, cached and non-cached, with the result checked from both sides.
needs: [F02, M02]
repro: base
status: validated
hardware: [ddr-cached-shared, ddr-noncached, u54-linux, u54-4]
software: [pfdaq-cap, supervisor, udmabuf]
sources:
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:memchk,PFDAQ_T_MEMCHK
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:run_shm,sup_xfer
  - yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts
  - tests/board/pfdaq_final_suite.sh
  - tests/board/pfdaq_bm_suite.sh
evidence: [suite-freertos, suite-baremetal, boot-hss]
related: [F02, M02, M04, I05, I08]
---

# Shared memory between Linux and a bare-metal hart

After this module you can explain how Linux on three cores and a separate context on the fourth exchange data through plain DDR memory, what the project's shared-memory test does step by step, and what its zero-mismatch result does and does not tell you about caches and memory ordering.

## Before you start

- **Level 3.** You should know what the three views of DDR are (module F02) and how the two contexts exchange messages (module M02).
- **Board:** the shared baseline. The results below were recorded earlier; nothing on this page was run on the board while it was written.

## The idea

Two processors that share memory face two questions:

1. **Visibility.** When one writes a value, does the other eventually read that value and not an older copy from its cache?
2. **Order.** If the first writes a block of data and then sets a flag, can the second see the flag before it sees the data?

Hardware gives different answers depending on the kind of memory, on whether caches take part, and on the barrier instructions the programs use. The standard way to avoid depending on subtle rules is **turn taking**: only one side touches the data at a time, and a message between the sides marks the hand-over. The project's test is exactly that. It checks that turn taking works across the two operating contexts on this chip; it is not a litmus test of the memory model.

## On this board

### Two regions, two kinds of mapping

The device tree reserves two 32 MiB pools, both marked `no-map` so that Linux does not use them for ordinary memory, and exposes each through a u-dma-buf device (module I05):

| Device | Physical window | Linux's mapping | Used for |
|---|---|---|---|
| `udmabuf-ddr-nc0` | `0xC800_0000`, 32 MiB | non-cached | DMA descriptors and frame buffers (module I04) and the **non-cached** test |
| `udmabuf-ddr-c0` | `0x8800_0000`, 32 MiB | cacheable (the code comment says "mapped cacheable on Linux") | the **cached** test |

The capture tool opens the non-cached device with `O_SYNC` and the cached one without it.

The boot monitor prints its segment table at start. Both windows are listed with "physical DDR 0x00000000" as their base, together with a third, write-combining window at `0xD000_0000`:

```
        Cached: SEG0_0: offset 0x0080000000, physical DDR 0x00000000
    Non-cached: SEG1_2: offset 0x00c0000000, physical DDR 0x00000000
Non-cached WCB: SEG1_4: offset 0x00d0000000, physical DDR 0x00000000
```

Read literally, the cached and non-cached windows are two address ranges onto **the same DDR**. The test keeps the two regions apart by using different offsets (see below), so it never relies on that aliasing. The same table has the L2 configuration: 1 MiB of L2 cache, 512 KiB of scratchpad and 512 KiB of loosely integrated memory.

### The protocol

The test is the pair of functions `run_shm` in the Linux tool and `memchk` on the real-time hart. One iteration:

1. Linux fills `words` 32-bit words with an xorshift32 sequence from a seed, then executes a full barrier (`__sync_synchronize()`).
2. Linux sends one RPMsg message of type `MEMCHK` (6). Its three fields carry the physical address, the word count and the seed.
3. The real-time hart reads every word, counts those that differ from the sequence, **writes the bitwise inverse** in the same place, executes `fence rw, rw`, and replies with the number of mismatches and the number of words checked.
4. Linux then reads every word and counts those that differ from the inverse.

The same four steps as turns, with the two windows underneath:

![One iteration of the shared-memory test, by turns. Linux fills 64 K words with an xorshift32 sequence and executes a full barrier; it sends one MEMCHK message with the physical address, the word count and the seed; hart 4 reads every word, counts those that differ, writes the inverse back and executes fence rw, rw; it replies with the number of mismatches and words checked; Linux then reads every word and expects the inverse.](../diagrams/lab-m05-memchk-turns.svg)

1. Linux's turn: it writes the whole region, then executes a full barrier. Hart 4 does not touch the region yet.
2. The MEMCHK message hands the turn over. It carries only the physical address, the word count and the seed; the data stays where it is.
3. Hart 4's turn: it reads each word and counts the differences, writes the inverse in the same place, and executes `fence rw, rw`.
4. The acknowledgement hands the turn back, carrying the mismatch count and the number of words checked.
5. Linux's turn again: it reads every word and expects the inverse. The region sits in one of the two windows below, at the offset the test uses.

```c
/* firmware/u54_4-supervisor/pfdaq_supervisor.c */
for (i = 0; i < words; i++) {
    x = xs32(x);
    uint32_t v = p[i];
    if (v != x)
        bad++;
    p[i] = ~x;
}
__asm volatile ("fence rw, rw" ::: "memory");
```

Reading first and writing the inverse second makes the test two-way. Linux's data must be visible to the hart, and the hart's data must be visible to Linux. The pointer is `volatile`, so the compiler cannot drop or reorder the accesses.

The two regions use different start offsets so that the test cannot disturb anything else: `0x100000` in the cached region, `0x1F00000` in the non-cached region, both away from the buffers the capture tool uses.

### What the hart sees

The hart receives a **physical address**, from `/sys/class/u-dma-buf/<device>/phys_addr`, and uses it directly as a pointer. It runs in machine mode with no address translation, so for the cached region it reads and writes through the cached window and for the non-cached region through the non-cached one. Neither side performs cache maintenance: there are no flush or invalidate calls on either side. That is the point of the cached run.

## Run it

These are the commands the project ran; the records are in the evidence section. They need the board with the pfdaq image and the supervisor payload (FreeRTOS or bare metal).

```sh
pfdaq_cap -X 65536 -n 50        # non-cached region
pfdaq_cap -X 65536 -n 50 -c     # cached region
```

`-X` is the word count (65,536 words, 256 KiB) and `-n` the number of iterations. Both lines are in `tests/board/pfdaq_final_suite.sh` and `tests/board/pfdaq_bm_suite.sh`. Each command prints one JSON line.

## What you should see

From the FreeRTOS suite (`suite-freertos`) and the bare-metal suite (`suite-baremetal`), as the suite summariser formats them:

| Context | Region | Words × iterations | Mismatches on the hart | Mismatches in Linux | Time per check |
|---|---|---|---|---|---|
| FreeRTOS | non-cached | 65,536 × 50 | 0 | 0 | 31.15 ms |
| FreeRTOS | cached | 65,536 × 50 | 0 | 0 | 20.10 ms |
| Bare metal | non-cached | 65,536 × 50 | 0 | 0 | 29.88 ms |
| Bare metal | cached | 65,536 × 50 | 0 | 0 | 18.77 ms |

"Time per check" is measured on the Linux side from just before the message is sent to just after the reply arrives. It therefore contains the message exchange and the hart's whole pass over 64 K words, and none of Linux's own writing or verifying. The non-cached runs are about 11 ms slower in both contexts. That is consistent with uncached accesses being slower than cached ones, and nothing more specific was measured. The few milliseconds of difference between FreeRTOS and bare metal were not investigated.

The commands, as recorded in the FreeRTOS suite, with hart 4's side of the same runs:

![The shared-memory test, cached and non-cached. Each run is 50 iterations of 65,536 words with 0 mismatches seen by hart 4 and 0 seen by Linux. The cached run reports a mean check of 20.104 ms at physical address 0x88100000, the non-cached run 31.148 ms at 0xc9f00000. Hart 4's console prints one MEMCHK line per iteration: 65536 words, 0 mismatches, inverse written.](../diagrams/lab-m05-memchk-run.svg)

1. `-X` is the word count, `-n` the number of iterations and `-c` selects the cached window. Nobody flushes or invalidates a cache.
2. Hart 4 saw 0 mismatches and Linux saw 0, over 50 passes of 65,536 words, with no RPMsg failure.
3. Hart 4 prints one `MEMCHK` line per iteration, 50 per run; one is shown. The address in it is the physical address Linux read from sysfs.
4. The same command without `-c` uses the non-cached window, the one the DMA buffers live in.
5. The check takes 31.148 ms against 20.104 ms. The time includes the message exchange and the hart's whole pass over the region.

The JSON lines are wrapped for display. The console lines are from the fresh-card run of the suite.

## Do the same in your project

You have Linux on some cores and a bare-metal or RTOS context on another, and you want to know that data written by one is read correctly by the other. Work in this order.

1. **Reserve one region per kind of mapping.** In the device tree, mark each pool `no-map` so that Linux does not use it for ordinary memory, and expose it through a u-dma-buf device. The project has `udmabuf-ddr-c0` (`0x8800_0000`, 32 MiB, cacheable on Linux) and `udmabuf-ddr-nc0` (`0xC800_0000`, 32 MiB, non-cached). Use your own windows and names.
2. **Keep the test away from your real buffers.** The project works at offset `0x100000` in the cached region and `0x1F00000` in the non-cached one.
3. **Pass a physical address.** Read it from `/sys/class/u-dma-buf/<device>/phys_addr` and add the offset. The other context uses it directly as a pointer, so it must be the physical address of the window, never a virtual one. Open the non-cached device with `O_SYNC` and the cached one without it.
4. **Add the message type to your RPMsg protocol.** `MEMCHK` is type 6 with the physical address, the word count and the seed in three of the existing fields (module M02).
5. **Write the two halves of the protocol.** Linux: fill the region from an xorshift32 sequence, `__sync_synchronize()`, send the message, then read every word and count those that differ from the inverse. The other context (`memchk()`): read every word and count those that differ from the sequence, write the bitwise inverse, execute `fence rw, rw`, and answer with the mismatch count and the number of words. Make the pointer `volatile`.
6. **Run it and read the three counters.** `pfdaq_cap -X 65536 -n 50` for the non-cached region and `pfdaq_cap -X 65536 -n 50 -c` for the cached one. In each JSON line expect `mismatch_seen_by_u54_4` 0, `mismatch_seen_by_linux` 0 and `rpmsg_fail` 0. The tool clamps `-X` to 0x40000 words.
7. **Run the cached variant too, with no cache maintenance on either side.** That is the point of the test: it shows whether the sharing pattern you chose works as it is. If it reports mismatches that grow with the pattern size, you have found a cache effect; none has been seen on this board.

What not to change:

- **Turn taking.** Never let both sides touch the region at the same time; this test says nothing about that.
- **A message between every two turns.** The barriers and the RPMsg round trip sit between each write and the next read. Leaving them out was not tried here.
- **Non-cached memory for anything a non-processor master writes.** Coherence with fabric DMA is not covered by this test; the project's DMA buffers are non-cached on purpose (module I05).
- **How much you claim from a pass.** Zero mismatches in 50 passes is evidence for this access pattern, on one board, in one run of the suite.

## Evidence

All four runs: 50 iterations of 64 K words, in each direction, with **zero mismatches** on both sides. That is 3.3 million words checked by the hart and the same number by Linux per region and context. The boot record (`boot-hss`) is the source of the segment and cache table quoted above.

### What this shows

- Two contexts, in two operating-system models, read what the other wrote, in cached and non-cached memory, when they take turns and a message separates the turns.
- In the cached case no stale data was seen **without any cache maintenance** on either side.

### What it does not show

- **No concurrent access.** The sides never touch the region at the same time. Nothing here says what happens when they do.
- **Ordering is supplied by the message exchange, not tested on its own.** Barriers and the RPMsg round trip sit between every write and the following read. Leaving them out was not tried, so the test says nothing about what they are protecting against.
- **No proof from a pass.** Zero mismatches in 50 passes is evidence for the access pattern used. It cannot rule out a rare failure, and it was one board and one run of the suite.
- **Coherence with other bus masters is a separate question.** Fabric DMA writing into a cached region, for example, is not covered. The DMA buffers in this project are therefore non-cached on purpose (module I05).
- **The mechanism is not established.** Why no stale data appeared, for example whether the harts' own caches are kept coherent by hardware, was not looked up in the vendor's manual for this page and is not claimed.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| `pfdaq_cap -X` fails with "supervisor RPMsg endpoint" | The real-time context has not announced its channel. Check that remoteproc is attached (module M06) |
| The hart reports mismatches on the first word of every pass | Linux and the hart disagree about the physical address. The address comes from `phys_addr` in sysfs; it must be the device's window, not a virtual address |
| Mismatches only in the cached run, growing with the pattern size | A cache effect. This has not been seen on this board; the test is the tool to look for it |
| `mmap` fails | The device was not created; check that the u-dma-buf devices exist (`ls /dev/udmabuf*`) |

## Trade-offs and going further

- **Non-cached memory is slow, cached memory needs care.** Non-cached is the safe choice for anything a non-processor master writes. Cached is faster for pure hart-to-hart exchange, which is what this test shows works without maintenance, within its limits.
- **A pointer is a bad message.** The protocol passes an address and a count over a channel and keeps the data in place. It works because both sides know the region and trust each other. Anything with a trust boundary needs more than this protocol gives; none exists in this project.
- **Exercise:** write down, before running it, what you expect if Linux skips the barrier in step 1. Then think about which part of the system would have to be at fault for the test to notice.
- **Exercise:** the check time grows with the number of words. Estimate how long a 256 KiB check should take at the speed of a plain `memcpy` of that size, and compare it with the table.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [F02 The memory map and the three faces of DDR](F02-memory-map-ddr.md)<br>[M02 RPMsg in practice: announce, bind, ping](M02-rpmsg-in-practice.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [Cached shared region](../system/README.md#ddr-cached-shared) · [Non-cached window](../system/README.md#ddr-noncached) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [pfdaq supervisor](../system/README.md#supervisor) · [u-dma-buf](../system/README.md#udmabuf) |
| **Source** | [`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `memchk`, `PFDAQ_T_MEMCHK`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `run_shm`, `sup_xfer`<br>[`yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts`](../../yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts)<br>[`tests/board/pfdaq_final_suite.sh`](../../tests/board/pfdaq_final_suite.sh)<br>[`tests/board/pfdaq_bm_suite.sh`](../../tests/board/pfdaq_bm_suite.sh) |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with the bare-metal supervisor on hart 4, 8 of 8](../../evidence/v0.5.1/board/suite-baremetal.md) (board, fabric v0.5.1, 2026-11-15)<br>[Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [F02 The memory map and the three faces of DDR](F02-memory-map-ddr.md)<br>[M02 RPMsg in practice: announce, bind, ping](M02-rpmsg-in-practice.md)<br>[M04 Same core, same code: FreeRTOS against bare metal](M04-freertos-vs-bare-metal.md)<br>[I05 Buffers the DMA can write: non-cached windows and u-dma-buf](I05-dma-buffers.md)<br>I08 DMA into cached memory: when you need cache maintenance *(planned)* |
| **Status** | validated |
<!-- nav:end -->
