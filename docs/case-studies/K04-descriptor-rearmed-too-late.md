---
id: K04
title: "The descriptor that was re-armed too late"
track: case-study
level: 3
summary: A transfer that ignored its new destination, and a DMA that stopped taking data.
repro: doc
status: validated
hardware: [dma, pfdaq-stream-src, ddr-noncached]
software: [pfdaq-cap]
sources:
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:arm,run_realtime,arm_rt
  - hw/rtl/pfdaq_stream_src.v:credit_pulse_a,can_start
evidence: [suite-freertos, sim-stream]
related: [I04, H06, I07, K03, K09]
---

# The descriptor that was re-armed too late

The natural way to capture a stream of separate frames looks like this: give the DMA a buffer, let a frame arrive, take the interrupt, give it the next buffer, repeat. With this DMA core that loop does not work. This page records what happened, what was concluded, and the two patterns the design uses instead.

## Symptom

The fabric can mark the end of every frame (`TLAST` on each frame, `CTRL.TLAST_PER_FRAME`), so that each frame is one packet and one DMA transfer. The first board tool used it free-running: the source sent frames back to back and software, after each completion interrupt, wrote the next destination into the descriptor and the stream address register.

Two failures were seen, on fabric v0.3.0:

1. **A changed destination was ignored.** Software wrote a new buffer address into the descriptor, but the frames kept landing on the old address.
2. **A re-armed descriptor was not picked up.** When the descriptor was re-armed after the next packet had already started to arrive, the DMA did not use it, the stream stalled, and the DMA took no further data until the board was rebooted.

The runs themselves were not kept as logs that can be published. The notes record the conclusion and the working pattern; this page is built on those.

## Isolation

Two observations shaped the diagnosis, and a third limits it.

- **Arming first, then starting, worked.** Writing the descriptor, then its address, then `START` is the pattern every successful capture used, from 80 kB to 8 MB. The notes do not record a controlled comparison of arming earlier and later; the rule is the pattern that worked, not a measured boundary.
- **Both failures involved re-arming while frames were still flowing.** In the free-running mode software wrote the next destination after each completion interrupt, with the source still sending. That mode was dropped in favour of the pattern above, so it was not investigated further.
- **The same hardware passed in simulation.** The testbench scenarios with `TLAST_PER_FRAME` (a gap between packets, backpressure, a fault in the middle) pass, so the source's packet boundaries were right. The simulation has no DMA model; it cannot say what the vendor core does with a late descriptor.

The experiments did not go further. No signal inside the DMA core was observed.

## Cause

**Established, by test on one core version (CoreAXI4DMAController 2.2.107) on one board:**

- A stream descriptor armed *before* the packet's first beat is used.
- A descriptor changed or re-armed *after* a packet is waiting at the core is not used for that packet, and the stalled transfer is not recoverable by software.

**Not established:** why. The core might read the descriptor once, when the first beat of a packet arrives, and cache it; the notes do not say that, and no one looked at the core's RTL. The user guide, as read for this project, does not state the rule.

**Not tested:** other timing between re-arm and packet start. The rule below is the safe side of what was observed, not a measured boundary.

## Fix

The design never re-arms a descriptor that has a packet waiting. There are two patterns.

### Burst mode: arm, then start, one packet per burst

Software owns the timing. Each burst is armed completely before the source is told to start, and the next burst is armed only after the previous one has completed. The tool's header comment states the rule it was built on:

```c
 * Rule learnt on the board: the stream descriptor must be armed before the packet starts,
 * so each burst is: write descriptor, write stream address register, START.
```

and the loop does exactly that, alternating two buffers so that verification of one burst overlaps with the fabric filling the other:

```c
arm(buf_off[k], (uint32_t)burst_bytes);
t0 = now_s();
src[SRC_CTRL / 4] = 0x1;
```

Per-frame capture (one frame per packet) uses the same loop with a burst of one frame, which is why it is software-paced: arm, `START`, wait for the interrupt. The free-running per-frame mode stays in the RTL, passes simulation, and is not used on the board.

### Real-time mode: credits instead of races

In real-time mode frames become due on a schedule and software cannot hold them back, so software must be sure a descriptor is armed before its frame can start. The source sends a frame only if it holds a **credit**. Software returns credits, one per armed descriptor:

```c
for (int k = 0; k < ndest; k++)
    arm_rt(k, BUF_OFF + k * slot_sz, fbytes);
src[SRC_CREDIT / 4] = 0x80000000u | (uint32_t)ndest;   /* set: ndest credits */
...
memcpy(copy, ddr + BUF_OFF + k * slot_sz, fbytes);
arm_rt(k, BUF_OFF + k * slot_sz, fbytes);                /* re-arm this buffer ... */
src[SRC_CREDIT / 4] = 1;                                 /* ... then pay one credit */
```

The order is the whole design. A descriptor is re-armed first and its credit is released second, so a frame that finds a credit always finds an armed descriptor. Four descriptors, one for each `TDEST` value, rotate, so up to four frames can be in flight. A frame that finds no credit is not sent: the slot is dropped, counted in `DROPS` and visible as a gap in the sequence numbers (module H06).

This changes what software has to guarantee from "re-arm quickly" to "re-arm before returning the credit", which does not depend on timing.

## Evidence

- **Real-time mode, board, fabric v0.5.1:** the suite runs the slot rate from 1 ms down to 5 µs, with four descriptors, and the errors column of the [suite record](../../evidence/v0.5.1/board/suite-freertos.md) is `0/0/0` at every rate while frames are dropped as the rate rises. The same record shows a one-descriptor run (`T6-rt-1dest`) that passes.
- **Burst mode, board:** every burst test of the suite (counter, PRBS, FIR, bursts up to 8,192,000 bytes) arms before `START` and verifies without a sequence, payload or CRC error.
- **Simulation:** the real-time scenarios (13, 14, 15 and 18) and the per-frame `TLAST` scenarios (2 and 17) pass in the [simulation record](../../evidence/v0.5.1/sim/stream.txt).

None of these shows the original failure. They show that the two patterns avoid it.

## How to recognise it

| What you see | What it tells you |
|---|---|
| Frames land on the same address however often you rewrite the destination | The core read the descriptor earlier than you wrote it |
| The stream stops after one re-arm and does not restart | You re-armed with a packet already waiting. Reboot, then change the order |
| Everything works with a gap of a few milliseconds between frames and fails at speed | The re-arm races the next packet. Do not tune the delay; change the protocol |
| A design that relies on "software is fast enough" to re-arm | It is a race, and the core's behaviour in the lost case is a hang |

Habits that followed:

- **Make the safe order structural.** Arm-then-start in a loop, and re-arm-then-credit in real-time mode, mean no timing assumption is left.
- **Prefer hardware flow control to software speed.** The credit is a few registers of logic and removes the need to measure how fast Linux re-arms.
- **A rule found by test belongs in the tool.** The comment above is in the code that has to obey it. Module I04 lists this rule beside the others that were found the same way.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [CoreAXI4DMAController](../system/README.md#dma) · [Stream source](../system/README.md#pfdaq-stream-src) · [Non-cached window](../system/README.md#ddr-noncached) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `arm`, `run_realtime`, `arm_rt`<br>[`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `credit_pulse_a`, `can_start` |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15) |
| **Related** | [I04 DMA stream descriptors: the rules this core really follows](../modules/I04-dma-stream-descriptors.md)<br>[H06 Real-time sources: slots, credits and honest drops](../modules/H06-real-time-source.md)<br>[I07 Real-time capture with four descriptors and credits](../modules/I07-real-time-capture.md)<br>[K03 A DMA core that needs TVALID without gaps](K03-tvalid-gaps.md)<br>[K09 An early TLAST does not end a transfer](K09-early-tlast.md) |
| **Status** | validated |
<!-- nav:end -->
