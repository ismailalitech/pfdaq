---
id: K03
title: "A DMA core that needs TVALID without gaps"
track: case-study
level: 3
summary: Packets above about 360 bytes were corrupted and then hung the DMA, although the stream obeyed the standard.
needs: [H02]
repro: doc
status: validated
hardware: [pfdaq-stream-src, dma]
sources:
  - hw/rtl/pfdaq_stream_src.v:pfdaq_word_step,w_ready,out_valid
  - hw/tb/tb_pfdaq_stream_src.v:VALID_GAP
evidence: [fabric-throughput-v030, sim-stream, suite-freertos, timing]
related: [I04, K04, K09, K10, H11]
---

# A DMA core that needs TVALID without gaps

The first version of the stream source that ran on the board delivered small packets perfectly and destroyed large ones. The source kept every rule of AXI4-Stream. The vendor DMA core behind it needed one more rule that its user guide does not state. This page records what was seen, how the cause was narrowed down, what was changed, and what is still not known.

## Symptom

Fabric v0.2.1 produced one 32-bit word per clock. The stream is 64 bits wide, so the source needed two clocks to fill a beat: `TVALID` was high on every second clock and low in between, for the whole length of a packet.

On the board, with CoreAXI4DMAController 2.2.107 receiving the stream:

| Packet size | Result |
|---|---|
| up to 256 bytes | captured correctly, every time |
| 512 bytes and above | wrong data in memory, then the DMA stopped |

The failures had the same shape each time:

- The buffer was correct from the start up to about byte 360.
- After that point the data was wrong, and the stream's contents appeared a second time starting at offset 2048 of the buffer.
- The source was still busy afterwards: it had beats left to send and the DMA was no longer taking them.
- No later transfer worked, of any size, until the board was rebooted. The DMA refused all further stream data.

These observations were made during bring-up, on one board, and are recorded in the project's notes. The failing captures themselves were not kept, so the numbers above (256, 512, 360, 2048) have no file in `evidence/` behind them. What is in `evidence/` is the behaviour after the fix.

## Isolation

The failure could have been in four places: the source's data, the software that arms the DMA, the handshake, or the DMA core. They were ruled out in this order.

1. **The data was right.** Small packets used the same generator, the same CRC logic and the same output stage, and they verified. A generator bug would not depend on whether the packet is 256 or 512 bytes long.
2. **The software was right.** The same descriptor, written the same way, worked for small packets. Only the byte count differed.
3. **The size was the only variable.** After a reboot, a sweep of single-frame packets passed at 128, 160, 192 and 256 bytes and failed at 512. Nothing else changed between the runs.
4. **The handshake was legal.** The source did not wait for `TREADY`, and it held its signals while stalled. The only unusual property of the stream was the regular one-clock pause between beats.

That left the pause. The AXI4-Stream standard allows it: a source may drop `TVALID` between any two beats, inside a packet or not. The core's user guide (CoreAXI4DMAController User Guide, DS50003714) states a requirement on `TKEEP` and `TSTRB`, which the source met, and says nothing about `TVALID` having to stay high. So the conclusion was an inference from the size sweep, to be tested by removing the pauses.

You can reproduce the *shape* of the v0.2.1 stream in simulation without a board. The experiment in module H02 changes one line of the output stage so that a beat is offered every second clock. The data stays correct and the handshake rules still hold; only the pauses appear. A sink that follows the standard would accept that stream.

## Cause

**Established:** on this board, CoreAXI4DMAController 2.2.107 with its stream interface at 64 bits corrupts a transfer and then stops accepting stream data when the source pauses `TVALID` between the beats of a packet longer than a few hundred bytes. Removing the pauses removes the failure.

**Not established:**

- **Why.** The core's RTL was not simulated or inspected. The numbers suggest an internal buffer that small packets fit into and large ones do not, but that is a guess.
- **The exact threshold.** It lies between 256 and 512 bytes. "About 360 bytes" is where the data in one failing buffer went wrong, not a measured limit.
- **Which pause patterns matter.** Only one pattern was tried: a pause after every beat. A single pause in a long packet may or may not be enough.
- **Whether other versions or configurations of the core behave the same.** One core version, one configuration, one board.

Two things are known *not* to trigger it. Pauses caused by the sink are fine: the DMA drops `TREADY` routinely (the source counts those cycles in `STALL_CYCLES`, and they are non-zero in every large burst) and the data is intact. Idle time between packets is fine too: in real-time mode every frame is its own packet with idle clocks before the next one.

## Fix

Fabric v0.3.0 generates **two words per clock**, so a full 64-bit beat is ready on every clock and `TVALID` stays high from the first beat of a packet to the last.

- The frame generator is a combinational module, `pfdaq_word_step`, that produces one word and the generator state after it. The source instantiates it twice and chains the two, so the second word is computed from the state the first one leaves.
- The output register is refilled in the same clock in which the DMA takes its contents (`w_ready = ~out_valid | TREADY`). Module H02 walks through those lines.
- A packet with an odd number of words ends with a half-filled beat (`TKEEP = 0x0F`), which the user guide permits on the last beat only.

The cost is logic depth: two CRC-32 steps and two payload steps now sit in series within one 8 ns clock period. The design still meets timing at 125 MHz in all corners (v0.5.1 build).

Two checks keep the property from being lost again:

- **In simulation**, the testbench reports `VALID_GAP` whenever `TVALID` is low on a clock inside a packet, and the checker fails the run on any such line. This is stricter than the standard on purpose. It later caught a real regression: when the abort feature was added, an abort that landed exactly on a frame boundary produced a one-clock gap, and the scenario failed until the first fill beat was moved into the same clock.
- **On the board**, every capture is verified word by word, so a corrupted transfer cannot pass unnoticed.

Results after the fix:

- On v0.3.0, single-packet captures of 96, 512, 4,096, 80,640, 524,288 and 8,192,000 bytes all verified, in every one of the repeated suite runs. The 512-byte packet is the size that failed before.
- On v0.5.1, the board suite passes all its data-integrity tests, including four bursts of 8,192,000 bytes each, and the simulation passes all 20 scenarios with the gap check active.

## How to recognise it

| What you see | What it tells you |
|---|---|
| Small transfers work, larger ones fail, and the boundary is a size rather than a time | Something inside the receiver fills up; look at how the stream is paced, not at the data |
| The first part of the buffer is correct and the rest is not | The transfer started properly; the source's data path and the descriptor are probably fine |
| The source stays busy long after the packet should have ended | The sink has stopped taking beats; the source is waiting, not broken |
| The source and the DMA disagree about whether the packet is over | Do not trust a completion signal alone; verify the data, and read the source's own status |
| The next transfer fails too, whatever its size, until a reboot | The core is in a state software cannot clear; make "reboot and retest the smallest case" the first step of any diagnosis |
| Simulation with a standard-conforming sink model passes | The standard is not the whole contract with this core |

Three habits follow from this case and are used throughout the project:

- **Sweep the size first.** A one-variable sweep found the boundary in minutes and ruled out most hypotheses on its own.
- **Treat a vendor core's behaviour as something to measure.** The rules this DMA really follows were all found by test; they are collected in module I04.
- **Turn each finding into a check.** A rule that lives only in a comment is forgotten at the next change. `VALID_GAP` is one line of testbench and has already paid for itself.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](../modules/H02-axi4-stream.md) |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [CoreAXI4DMAController](../system/README.md#dma) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `pfdaq_word_step`, `w_ready`, `out_valid`<br>[`hw/tb/tb_pfdaq_stream_src.v`](../../hw/tb/tb_pfdaq_stream_src.v): `VALID_GAP` |
| **Evidence** | [Fabric-side throughput of single bursts, 931 to 947 MB/s](../../evidence/v0.3.0/board/suite-python-x3.txt) (board, fabric v0.3.0, 2026-11-15)<br>[Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15) |
| **Related** | [I04 DMA stream descriptors: the rules this core really follows](../modules/I04-dma-stream-descriptors.md)<br>[K04 The descriptor that was re-armed too late](K04-descriptor-rearmed-too-late.md)<br>[K09 An early TLAST does not end a transfer](K09-early-tlast.md)<br>[K10 Odd frame lengths lose 2 KB chunks](K10-odd-frame-lengths.md)<br>[H11 Verification with an independent model](../modules/H11-independent-model.md) |
| **Status** | validated |
<!-- nav:end -->
