---
id: K09
title: "An early TLAST does not end a transfer"
track: case-study
level: 3
summary: What the DMA really waits for, found by aborting a burst.
repro: doc
status: validated
hardware: [pfdaq-stream-src, dma]
software: [pfdaq-cap]
sources:
  - hw/rtl/pfdaq_stream_src.v:fill_mode,split_a,remaining,fill_beat
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:run_abort
evidence: [sim-stream, suite-freertos, suite-freertos-raw]
related: [I04, P05, K03]
---

# An early TLAST does not end a transfer

The stream source can be told to stop in the middle of a burst. The obvious way to stop cleanly is to end the packet early: finish the frame in progress and mark it with `TLAST`. The DMA controller did not accept that as the end of its transfer and went on waiting. This page records how that was found, the fix, and a second, smaller fault that the fix introduced and the simulation caught.

## Symptom

An abort was requested during an 8 MB burst. The stream stopped after a whole frame, with `TLAST` set on that frame's last word, which is what AXI4-Stream says closes a packet. The source went idle and reported that the run was aborted.

The DMA did not report completion. The transfer descriptor had been armed for the full burst length, and the DMA was left waiting for the rest of it.

The design intent of that first version is in the project history: "CTRL.ABORT finishes the frame in progress ... and sets TLAST on its CRC word, so the DMA always sees a closed packet". The failure was seen on the board during the v0.5 work and is recorded in the project's decisions as "Found on the board that an early TLAST does not complete a transfer". The raw capture of the failing run was not kept.

The earlier abort, in fabric v0.3.0, did something cruder: it stopped the source in the middle of a packet, without `TLAST`. That also left the DMA waiting, and was documented as a known limit that needed a reboot. The first packet-safe version was the attempt to remove that limit.

## Isolation

The reasoning was short, because only one thing had changed between "works" and "does not".

1. **A burst that runs to its end completes.** The same descriptor and the same source complete every time when nothing is aborted. The hardware, the arming and the interrupt path were fine.
2. **An aborted burst stops after a frame boundary.** The packet was closed: it ended with `TLAST` on the last word of a frame. Nothing was wrong with the end of the packet itself.
3. **The only difference was the length.** The packet was shorter than the byte count in the descriptor. The DMA behaved as if it were counting bytes, not watching for `TLAST`.

The design that follows treats that as a rule, and module I04 lists it with the other rules this core follows.

## Cause

**Established by test.** On this board, with CoreAXI4DMAController 2.2.107 as integrated here, a stream transfer completes when the descriptor's full byte count has arrived. A `TLAST` before that does not complete it.

**Not established.** Why the core behaves this way, and what it does with a packet that ends early: whether it discards the data, keeps the transfer open, or treats the next packet as a continuation. What happens to later transfers after an early `TLAST` is not recorded. The vendor guide was not checked for a statement about it for this page, and the vendor core's RTL was not simulated.

## Fix

If the DMA completes only on the full count, the source must send the full count. Aborting a burst therefore means: finish the frame in progress, then **send zero words up to the length of the whole run**, with `TLAST` on the last one.

The source decides when this applies:

```verilog
// burst mode with a single packet per run: an abort is completed with zero fill
wire        fill_mode  = !cfg_rt && !cfg_tlast_pf;
```

In real-time mode and in the per-frame `TLAST` mode every frame is its own packet and its own transfer, so an abort simply stops after the current frame. Only a burst that is one packet for the whole run needs the zero fill.

The number of words still owed is the run's total less what has been sent, and a small state machine sends them:

```verilog
wire [31:0] remaining  = cfg_total - run_words;
...
task fill_beat;
    out_data   <= 64'd0;
    out_keep   <= (remaining == 32'd1) ? 8'h0F : 8'hFF;
    out_last   <= (remaining <= 32'd2);
```

The last beat carries `TLAST`, and `TKEEP` is `0x0F` if one word remains, so the byte count is exact even for an odd total. `FRAMES_SENT` still counts only the real frames, so software knows where the data stops and the zeros begin. The status register's aborted bit says the run was cut short.

### The second fault: a one-clock gap

The rule from case study K03 still holds: `TVALID` must not drop inside a packet. The first version of the fill left a gap of one clock at the junction between the last real frame and the first fill beat, when the abort fell exactly on a frame boundary. The simulation caught it, because the testbench reports `VALID_GAP` on any clock inside a packet where `TVALID` is low. Which scenario first showed it was not recorded.

The fix, in `pfdaq_stream_src.v`, sends the first fill beat in the same clock in which the abort is recognised at a frame boundary, so there is no idle clock between the last frame and the fill. A related case is an abort that lands between the two words of one beat: there `split_a` sends the last word of the aborted frame and the first fill word together in one beat.

The cost of the zero fill is time, not data. The DMA completes when the whole burst has gone past, so an abort does not shorten the transfer. In the board runs below the interrupt arrives about 8.9 ms after START, whether the abort came after 2 ms or after 50 µs. For comparison, 8,192,000 bytes at 931 to 947 MB/s (the fabric rate measured on v0.3.0) take 8.7 to 8.8 ms; that comparison is arithmetic, not a separate measurement.

## Result

**Simulation** (`sim-stream`, 20 scenarios, all pass). The abort scenarios, with the number of frames that were real and the zero words that followed:

| Scenario | Run | Real frames | Zero fill |
|---|---|---|---|
| 16 | 50 frames of 104 words, mid-burst | 13 | 3,848 words |
| 20 | 40 frames of 37 words, FIR payload | 21 | 703 words |
| 21 | 30 frames of 12 words, abort right after START | 2 | 336 words |

In each case the checker confirms that the burst has the full length of the run, that the words after the real frames are zero, and that `TLAST` is on exactly the last word of the burst.

**Board** (`suite-freertos`, fabric v0.5.1):

| Test | Run | Result |
|---|---|---|
| `ABORT-2ms` | 500 frames, PRBS, abort after 2 ms | DMA completed after 8.899 ms; 119 frames real, the rest zero; nothing written past the burst |
| `ABORT-50us` | 8,000 frames, FIR, abort after 50 µs | DMA completed after 8.904 ms; 128 frames real, the rest zero; nothing written past the burst |
| `ABORT-after` | eight supervised 1 MiB bursts, run straight after the abort tests | 0 sequence, payload and CRC errors; the DMA works normally again |

In the raw records, `fill_nonzero` is 0 in both abort tests, and no CRC, sequence or payload error was seen in the real frames. `run_abort` in `pfdaq_cap.c` makes the checks: DMA done, the status bit set, the real frames intact, the fill all zero, and 64 guard bytes past the burst untouched.

One mode is left outside this: `CTRL.FORCE_ABORT` stops at once and may leave the DMA waiting, and the register description says recovery then needs a reset.

## How to recognise it

| What you see | What it tells you |
|---|---|
| A stream packet ends with `TLAST` but the DMA never reports completion | The core counts bytes. Compare the packet length with the byte count in the descriptor |
| The simulation with a standard-conforming sink passes while the board hangs | The sink model accepts an early `TLAST`; this core does not. Add the rule to the testbench |
| A stopped transfer leaves the DMA unresponsive until reboot | Prefer a stop that completes the transfer over one that leaves it open |
| A new feature in the stream path produces `VALID_GAP` in the simulation | The no-gap check from K03 is doing its job. Look at the junctions: frame boundaries, the first beat after a state change |

Two habits follow. When a vendor core ignores a signal you expected to matter, state the rule that you found as a testbench check, as `VALID_GAP` did for K03. And design the stop so that it does not depend on the core's tolerance: ending the transfer the way the core wants is cheaper than finding out what it does with anything else.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [CoreAXI4DMAController](../system/README.md#dma) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `fill_mode`, `split_a`, `remaining`, `fill_beat`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `run_abort` |
| **Evidence** | [Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [I04 DMA stream descriptors: the rules this core really follows](../modules/I04-dma-stream-descriptors.md)<br>P05 Abort without wedging the DMA *(planned)*<br>[K03 A DMA core that needs TVALID without gaps](K03-tvalid-gaps.md) |
| **Status** | validated |
<!-- nav:end -->
