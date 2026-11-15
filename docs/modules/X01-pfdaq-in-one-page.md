---
id: X01
title: pfdaq in one page
track: system
level: 1
summary: What the system is, what runs where, and the results that matter.
repro: doc
status: validated
hardware: [soc, e51, u54-linux, u54-4, pfdaq-core, dma, ihc, ddr-noncached]
software: [hss, linux-kernel, pfdaq-cap, pfdaq-lat, supervisor]
evidence: [suite-fresh-card, suite-baremetal, reboot-30, timing, sim-stream, soak-v030, fabric-throughput-v030]
related: [C01, F03, H02, I04, P02, K03]
---

# pfdaq in one page

This page is the shortest complete description of the system: what it is for, which part runs where, how data and control move through it, what has been measured, and what does not work. Every section points to the place that explains it properly.

## What it is for

pfdaq is a data-acquisition chain built to be *checked*. The data is synthetic, so that every word that arrives in memory can be recomputed and compared. The interesting part is everything around the data: a hardware source that cannot be paused, a DMA that must be fed correctly, an operating system that is not real-time, and a second, small program on its own processor core that notices when the first one falls behind.

It runs on one board, the Microchip PolarFire SoC Discovery Kit. The chip on it combines five RISC-V processor cores with an FPGA, which makes it possible to put each job where it belongs:

| Job | Where it runs | Why there |
|---|---|---|
| Generate data at a fixed rate, timestamp it, protect it with a CRC | FPGA fabric | needs exact timing and no software in the path |
| Move the data into memory | DMA controller in the fabric | the processor must not copy at the source's rate |
| Configure, capture, verify, report | Linux on three cores | files, network, tools, and plenty of computing power |
| Watch Linux; answer hardware within microseconds | a supervisor on the fourth core | must keep its timing whatever Linux is doing |
| Start everything | the boot monitor on the fifth, smaller core | has to run before any of the others can |

## What runs where

| Core | Software | Privilege | Started by |
|---|---|---|---|
| E51 | Hart Software Services (boot monitor) | machine | the chip, from its internal flash |
| U54 1, 2, 3 | U-Boot, then Linux 6.18 | supervisor, above OpenSBI | the boot monitor |
| U54 4 | pfdaq supervisor on FreeRTOS, or the same source on bare metal | machine | the boot monitor, at the same time |

Two operating systems share one chip without a hypervisor: this is *asymmetric multiprocessing*. Linux is told in its device tree that the fourth application core does not exist, and memory for the supervisor is reserved so Linux never touches it. FreeRTOS and bare metal are alternative builds of the same supervisor for the same core; they are never present together.

The supervisor is already running when Linux starts to boot. Linux later attaches to it and opens a message channel.

## How data moves

One burst, start to finish. The [system page](../system/README.md) lists the same walk with the component on each side of every step.

1. The Linux capture tool, `pfdaq_cap`, writes a *descriptor* into memory: where the data should go and how many bytes to expect. It tells the DMA controller where the descriptor is.
2. It writes the frame length and frame count to the stream source's registers and sets START.
3. The source produces frames: a header, a sequence number, a timestamp, the payload and a CRC-32. It sends two 32-bit words on every 125 MHz clock over an AXI4-Stream interface.
4. The DMA controller writes the stream into DDR memory through the chip's AXI interface.
5. When the full byte count has arrived, the DMA raises an interrupt. Linux wakes `pfdaq_cap`.
6. `pfdaq_cap` copies the burst out of the DMA buffer and checks every frame: header, sequence, payload against a software model, CRC.
7. It reports the burst to the supervisor over RPMsg.

The payload is one of three kinds: a counter, a pseudo-random sequence, or the output of an 8-tap FIR filter that runs on the FPGA's multipliers. Linux recomputes all three.

There is also a *real-time mode*. Frames become due on a fixed schedule, as samples from a converter would. If software has not handed back a buffer in time, the frame is dropped, the drop is counted, and the sequence number shows the gap.

## How it is supervised

The supervisor does two independent things with each report from Linux.

- **Deadline.** If no report arrives within the agreed time, it raises an alarm and counts the miss. When reports resume, it returns to normal.
- **Cross-check.** It reads the stream source's own sequence counter directly from the fabric and compares it with what Linux claims to have verified. Linux may be at most one burst behind the hardware, never ahead of it.

Because the supervisor runs on its own core, in machine mode, with its own interrupt line from the fabric, nothing Linux does can delay it. That claim is measured, not assumed: a timer in the fabric raises an interrupt and counts 20 ns steps until software answers.

## What has been measured

One board, fabric v0.5.1 unless stated. The records are under [`evidence/`](../../evidence), each with the command and conditions that produced it.

| Question | Answer |
|---|---|
| Does it all work from a clean start? | 27 of 27 functional tests pass from a freshly written SD card |
| Does it come up every time? | 30 of 30 warm reboots, both systems up and a supervised capture passing |
| Are injected faults caught? | Every corrupted CRC, skipped sequence number and flipped payload bit, in all three payload types |
| How fast is the hardware path? | 931 to 947 MB/s into memory (measured on fabric v0.3.0) |
| How fast is the verified path? | 29.6 MB/s for counter payload, 8.7 MB/s for FIR: one Linux core copying and checking |
| How quickly does the supervisor answer an interrupt? | 0.5 µs typical, 3.1 µs worst seen, no event missed in 600,000 |
| And Linux? | 4.4 µs typical in a kernel handler, 78 µs in user space, with rare stalls of 1 to 5 ms |
| Does RTOS or bare metal matter for that? | In the handler, no: 0.52 µs against 0.50 µs. A FreeRTOS task adds about 1.5 µs |
| Did the hardware meet timing? | In all corners, using 26 % of the fabric |
| Has it run for long? | Ten minutes, 17.8 GB, no error, on fabric v0.3.0. Not repeated on v0.5.1 |

## What does not work

- **Odd frame lengths in multi-frame bursts corrupt data.** With an odd number of 32-bit words per frame, 2 KB pieces go missing at memory boundaries. The stream is correct in simulation and timing is met. The cause has not been found. The capture tool refuses the combination.
- **Linux cannot restart the supervisor core.** It can stop it; the boot monitor rejects the request to start it again. Recovery is a reboot.
- **Once, in about sixty boots, the supervisor did not start.** It has not happened since and is unexplained.
- **Everything was measured on a single board.**

These are documented, not hidden: each has, or will have, a case study that says what is known.

## Where to go next

| If you want to | Read |
|---|---|
| See every component and connection | [The system, component by component](../system/README.md) |
| Learn one technique at a time | [Module catalog](README.md) and [learning path](../learning-path.md) |
| Understand the streaming interface | H02, AXI4-Stream |
| Understand how Linux drives the DMA | I04, DMA stream descriptors, with C01 for the general idea |
| Understand how an interrupt gets from the fabric to a core | F03 |
| See the latency measurements in full | P02 |
| Read about a failure and how it was found | K03, the DMA that needs `TVALID` without gaps |
| Look up a register or a rule | [Architecture as built](../architecture.md) |

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [MPFS095T system-on-chip](../system/README.md#soc) · [E51 (hart 0)](../system/README.md#e51) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [pfdaq_core](../system/README.md#pfdaq-core) · [CoreAXI4DMAController](../system/README.md#dma) · [Mi-V inter-hart communication](../system/README.md#ihc) · [Non-cached window](../system/README.md#ddr-noncached) · [Hart Software Services](../system/README.md#hss) · [Linux 6.18.17](../system/README.md#linux-kernel) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [pfdaq_lat](../system/README.md#pfdaq-lat) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Evidence** | [The same suite on a freshly written SD card, 27 of 27](../../evidence/v0.5.1/board/suite-fresh-card.md) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with the bare-metal supervisor on hart 4, 8 of 8](../../evidence/v0.5.1/board/suite-baremetal.md) (board, fabric v0.5.1, 2026-11-15)<br>[Thirty warm reboots, both systems up and a supervised capture passing each time](../../evidence/v0.5.1/board/reboot-30.csv) (board, fabric v0.5.1, 2026-11-15)<br>[Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Ten-minute supervised soak, 17.8 GB, no errors](../../evidence/v0.3.0/board/soak-10min.json) (board, fabric v0.3.0, 2026-11-15)<br>[Fabric-side throughput of single bursts, 931 to 947 MB/s](../../evidence/v0.3.0/board/suite-python-x3.txt) (board, fabric v0.3.0, 2026-11-15) |
| **Related** | [C01 Direct memory access](../concepts/C01-dma.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md)<br>[H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md)<br>[I04 DMA stream descriptors: the rules this core really follows](I04-dma-stream-descriptors.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md)<br>[K03 A DMA core that needs TVALID without gaps](../case-studies/K03-tvalid-gaps.md) |
| **Status** | validated |
<!-- nav:end -->
