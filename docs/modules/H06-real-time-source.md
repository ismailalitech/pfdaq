---
id: H06
title: "Real-time sources: slots, credits and honest drops"
track: fpga
level: 3
summary: A source that cannot wait, a consumer that sometimes must, and how every lost frame is still accounted for.
needs: [H02]
repro: sim
status: validated
hardware: [pfdaq-stream-src, dma]
software: [pfdaq-cap]
sources:
  - hw/rtl/pfdaq_stream_src.v:pfdaq_stream_src,R_RTWAIT,rt_timer,can_start,drop_now,tdest_idx
  - hw/tb/tb_pfdaq_stream_src.v
  - hw/tb/check_trace.py
evidence: [sim-stream, suite-freertos]
related: [H02, I04, I07, K04, P01]
---

# Real-time sources: slots, credits and honest drops

After this module you can explain why a source driven by a clock, such as a converter, cannot use backpressure the way a burst source can; describe the contract of slots, credits and drops that this design uses instead; read the "sent / dropped" figures of the simulation and of the board suite; and run an experiment that moves a run from "everything dropped" to "nothing dropped" by changing one number.

## Before you start

- **Level 3.** Read H02 first: the handshake and backpressure are assumed.
- **Tools:** Icarus Verilog 12 and Python 3. No board.
- **Time:** about fifteen minutes, most of it the simulation.

## The idea

In *burst mode* the source can be paused. When the receiver is not ready, the source waits, and nothing is lost: the stream gets slower, that is all. That suits a test generator. It does not suit the thing the generator imitates.

An analogue-to-digital converter samples on a clock. If the receiver cannot take a sample, the converter does not wait; the next sample arrives on schedule regardless. Either the system keeps up, or samples are lost. A real-time source has to say what happens in the second case, and the useful answer is that the loss is **visible and counted**, not silent.

This design makes three promises:

1. **Frames become due on a fixed schedule**, one every `RATE` clock cycles, regardless of the receiver.
2. **A due frame is sent only if two conditions hold:** the source is idle (the previous frame has left) and it holds a **credit**. Otherwise the frame is **dropped**.
3. **A drop leaves a trace:** the `DROPS` register goes up by one, and the sequence number still advances, so the receiver sees a gap in the numbering where the frame would have been.

The credit is the contract with software. A credit means "there is a buffer ready to receive a frame". The software hands credits to the source and takes them back one at a time as it frees buffers. The source never has to know how fast the software is; it only knows whether a credit is available.

## On this board

All of it is in `hw/rtl/pfdaq_stream_src.v`, in the 125 MHz stream domain.

### The slot timer

`MODE.REALTIME` selects the mode, `FRAME_COUNT` is the number of slots in a run, and `RATE` is the slot interval in 125 MHz cycles (minimum 2). On `START`, `rt_timer` is loaded with `rate - 1` and counts down. A slot is **due** when it reaches zero, and it reloads at once:

```verilog
wire        rt_active  = cfg_rt && ((run == R_RTWAIT) || (run == R_RUN));
wire        due        = rt_active && (rt_timer == 32'd0);
...
rt_timer <= due ? cfg_rate - 32'd1 : rt_timer - 32'd1;
```

The timer runs whether the source is waiting (`R_RTWAIT`) or sending (`R_RUN`). The slots do not wait for the data path.

### Send or drop

The decision is two wires:

```verilog
wire        can_start  = (run == R_RTWAIT) && (credits != 16'd0) && !abort_pend;
wire        drop_now   = due && !can_start;
```

`can_start` needs the source to be in `R_RTWAIT`, which means no frame is in progress, and a non-zero credit count. If a slot is due and `can_start` is true, the source takes a credit, records the timestamp and enters `R_RUN`. If a slot is due and `can_start` is false, that is `drop_now`: `drops` is incremented and the slot is gone.

There are two reasons for a drop, and the hardware does not distinguish them:

| Reason | What it means |
|---|---|
| **No credit** | Software has not freed a buffer in time |
| **Busy** | The previous frame is still going out, because the receiver (the DMA) applied backpressure or because the frame is long compared to the slot |

### The sequence number

A frame carries a sequence number, one more than the previous frame's. A dropped slot also consumes one:

```verilog
if (drop_now)
    seq <= seq + 32'd1;        // a dropped slot still uses a sequence number
```

When the drop happens while a frame is being sent, the count is held in `seq_adv` and folded into the next frame's number, so the numbering stays one per slot either way. A receiver that sees sequence 41 followed by 44 knows that two slots were lost between them. If the last slots of a run are dropped, no later frame shows the gap, but `NEXT_SEQ` does, and so does `DROPS`.

### The timestamp

Word 2 of a real-time frame is the cycle counter at the moment the frame became **due**, not at the moment it was sent. In the simulation, the checker therefore requires every timestamp to lie on the slot grid: the differences between frame timestamps are exact multiples of `RATE`. A receiver can tell from the timestamps alone how long a frame waited.

### Credits

`CREDIT` is a write-mostly register. A write adds `[15:0]` to the count; a write with bit 31 set **replaces** the count instead. A read returns the count as of the last snapshot. A credit is consumed in the same clock in which a frame starts:

```verilog
if (credit_pulse_a && credit_val_p[16])
    credits <= credit_val_p[15:0] - {15'd0, due & can_start};
else
    credits <= credits + credit_add - {15'd0, due & can_start};
```

The Linux tool shows how software uses them (`run_realtime` in `pfdaq_cap.c`). Before START it arms one DMA descriptor per destination, then writes `0x80000000 | ndest`: replace the count with exactly as many credits as there are armed buffers. After that, each time the DMA reports a transfer done, the program copies the frame out, re-arms the descriptor and writes `1` to `CREDIT`. A credit therefore always stands for a buffer that is ready, and the source can never write into one that is still being read.

### Four destinations

With `MODE[9:8]` set, frames take the stream destination (`TDEST`) in turn, 0, 1, 2, 3, 0, ...

```verilog
tdest_idx <= (tdest_idx == cfg_tdn) ? 2'd0 : tdest_idx + 2'd1;
```

The DMA has one descriptor per `TDEST`, so up to four buffers can be armed at once and software has the time of four slots to turn one of them around. The destination advances only when a frame is sent, not when one is dropped, so the receiver can predict it from the count of frames received.

### Every frame is its own packet

In real-time mode `TLAST` is set on the CRC word of every frame. The reason is the DMA's rule that a transfer ends when its byte count is reached (module I04): if the whole run were one packet, no data would be visible to software until the run was over, and a lost frame would shift every later one. One packet per frame makes each frame an independent transfer with its own interrupt and its own destination.

## Run it

1. From the repository root:

   ```sh
   make -C hw sim-stream
   ```

2. Find the real-time scenarios in the output. Four of the twenty use it (13, 14, 15 and 18):

   ```sh
   make -C hw sim-stream | grep ' rt/'
   ```

3. **Change the credits.** Scenario 13 runs 30 slots, one every 200 cycles, with 12 credits and never returns any. In `hw/tb/tb_pfdaq_stream_src.v`, find the line that starts `run(13,` and change the credit argument, the number just before the last two zeros, from `12` to `30`. Run `make -C hw sim-stream` again, then restore the file (`git checkout hw/tb/tb_pfdaq_stream_src.v`).

## What you should see

The unmodified run reports the four real-time scenarios like this:

```
scenario 13:  30 frames x  44 words, mode 0 rt/4 dest rate 200, ready 100%, sent  12 dropped  18, detected crc=[] seq=[] pay=[]
scenario 14:  20 frames x  44 words, mode 2 rt/2 dest rate  20, ready 100%, sent  10 dropped  10, detected crc=[] seq=[] pay=[]
scenario 15:  12 frames x  24 words, mode 1 rt/3 dest rate  60, ready  80%, sent  12 dropped   0, detected crc=[1] seq=[] pay=[1]
scenario 18:  40 frames x  34 words, mode 0 rt/4 dest rate 100, ready 100%, sent   5 dropped   0, detected crc=[] seq=[] pay=[] ABORTED
```

The first number after `frames` is the number of slots, not the number of frames sent: scenario 13 had 30 slots and sent 12 frames, one per credit, and dropped the other 18. For every real-time scenario the checker verifies that `sent + dropped` equals the number of slots, that every sent frame uses the next destination in turn, that credits left equal credits given minus frames sent, that no sequence gap exceeds `DROPS`, and that the timestamps lie on the slot grid.

The other scenarios show the second kind of drop. Scenario 14 has 100 credits, plenty, but slots every 20 cycles and a 44-word frame that occupies the stream for 22 beats of two words each, and half of the slots are dropped for being **busy**, with no credit involved. Scenario 15 injects a payload error into one frame, and shows that it is detected (`crc=[1] pay=[1]`) even in this mode.

In step 3 (30 credits instead of 12), recorded in a scratch copy:

```
scenario 13:  30 frames x  44 words, mode 0 rt/4 dest rate 200, ready 100%, sent  30 dropped   0, detected crc=[] seq=[] pay=[]
RESULT: FAIL
   scenario 13: credit-limited run: sent 30 dropped 0, expected 12/18
```

Thirty credits cover thirty slots, and nothing is dropped. The run still ends in FAIL, but not because the hardware is wrong: the checker has a line that pins scenario 13 to exactly 12 sent and 18 dropped (`if n == 13 and (nfr, drops) != (12, 18)`), because that scenario exists to show credit-limited drops. The checker is judging the *scenario*, not just the hardware. Setting the credits to 0 in the same way gives `sent 0 dropped 30` and the same kind of failure line. In both cases all the general checks pass: the accounting is exact at every value.

## Evidence

**Simulation.** All 20 scenarios pass in the recorded run. The real-time ones exercise credit-limited drops (13), busy drops with the FIR payload (14), a fault in real-time mode (15) and an abort in real-time mode (18).

**Board, fabric v0.5.1.** The suite sweeps the slot interval with 4000 slots of 1 KiB frames, PRBS payload, four destinations. The result lines of `suite-freertos`:

| Slot interval | Offered load | Sent | Dropped | Dropped share | Verified | Data errors (seq/payload/CRC) |
|---|---|---|---|---|---|---|
| 1000 µs | 1.0 MB/s | 4000 | 0 | 0.0 % | 4000 | 0/0/0 |
| 100 µs | 10.2 MB/s | 3999 | 1 | 0.0 % | 3998 | 0/0/0 |
| 50 µs | 20.5 MB/s | 3208 | 792 | 19.8 % | 3205 | 0/0/0 |
| 20 µs | 51.2 MB/s | 1318 | 2682 | 67.0 % | 1316 | 0/0/0 |
| 10 µs | 102.4 MB/s | 586 | 3414 | 85.3 % | 583 | 0/0/0 |
| 5 µs | 204.8 MB/s | 463 | 3537 | 88.4 % | 460 | 0/0/0 |

Three things follow from the table.

- **Nothing is silently lost.** At every rate, frames sent plus frames dropped equals the 4000 slots, and the frames that did arrive have no sequence, payload or CRC error. The "verified" column is a few frames short of "sent" in some rows; the rows are single runs, and the difference was not investigated further.
- **The knee is between 100 µs and 50 µs**, at about 20 MB/s offered. The simulation shows what the source does with a credit; it does not show how fast the Linux side hands credits back. That is a software limit, consistent with the single Linux core that copies and verifies each frame (the project's architecture notes name it as the verified-throughput limit); a dedicated experiment that isolates it on the real-time path has not been run.
- **More destinations help.** The same 50 µs slot with one destination instead of four drops 50.2 % (sent 1993 of 4000), against 19.8 % with four. With one buffer, a slot is lost whenever the previous frame has not yet been copied out and re-armed; with four, software has four slots of time. This comparison uses the counter payload for the one-destination run and PRBS for the other, so it is suggestive, not a controlled experiment.

**The "delivered MB/s" figure is not a rate.** The suite also prints a delivered rate for each run, and it is easy to read wrongly. It is computed over the whole loop, including the final wait for the last transfer, so it understates the real rate. Two rows with the same 100 µs slot, 0 drops and the same load report 1.7 MB/s (4000 slots) and 10.2 MB/s (1000 slots). The sent and dropped counts are the figures that mean something. The validation plan records this as an instrumentation limit.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| `sent + dropped` is not the number of slots | A slot was lost between the timer and the counters: a defect in the source, not the receiver. The simulation checker catches it. |
| Drops even at a very low rate | Credits are not being returned, or are returned before the buffer is free. Read `CREDIT` after a snapshot to see the count. |
| Sequence gap larger than `DROPS` | Frames were lost after the source, between the source and memory. Not a source problem. |
| Frames arrive with the same destination twice in a row | The software re-armed the wrong descriptor; the source rotates strictly in order. |
| Frames overwritten before they are read | Credits were returned before the copy finished. Return the credit after the copy, as `pfdaq_cap` does. |
| The first frame is late | The first slot is due about `RATE` cycles after START, not at START. |

## Trade-offs and going further

- **Credits cost a register write per frame.** At 100,000 frames per second that is 100,000 writes over the 50 MHz bus. The alternative is a hardware ring buffer, with the source writing the descriptors itself, which removes the software from the loop and costs a good deal of logic.
- **The source cannot tell why it dropped.** A drop for no credit and a drop for being busy look the same in `DROPS`. Two counters would separate them, at the price of two registers.
- **Drops are counted, not retried.** That is the right behaviour for a source standing in for a converter. A source that must deliver every frame belongs in burst mode.
- **Exercise:** in the unmodified testbench, set the 12 credits of scenario 13 to 0 and predict the `NEXT_SEQ` the checker will expect. Then run it and compare.
- **Exercise:** scenario 14 drops half its slots because the source is busy. Estimate from the frame length how many 125 MHz cycles a frame occupies, then find the slot interval at which drops stop, and test your answer. In a scratch copy of the testbench, scenario 14 sent 10 frames and dropped 10 at 20, 21 and 22 cycles, and dropped none at 23 and above, so the frame needs a little over its 22 beats.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md) |
| **How to run** | Simulation only: Icarus Verilog and Python, no board |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [CoreAXI4DMAController](../system/README.md#dma) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `pfdaq_stream_src`, `R_RTWAIT`, `rt_timer`, `can_start`, `drop_now`, `tdest_idx`<br>[`hw/tb/tb_pfdaq_stream_src.v`](../../hw/tb/tb_pfdaq_stream_src.v)<br>[`hw/tb/check_trace.py`](../../hw/tb/check_trace.py) |
| **Evidence** | [Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md)<br>[I04 DMA stream descriptors: the rules this core really follows](I04-dma-stream-descriptors.md)<br>I07 Real-time capture with four descriptors and credits *(planned)*<br>[K04 The descriptor that was re-armed too late](../case-studies/K04-descriptor-rearmed-too-late.md)<br>P01 Where 940 MB/s becomes 30: throughput layer by layer *(planned)* |
| **Status** | validated |
<!-- nav:end -->
