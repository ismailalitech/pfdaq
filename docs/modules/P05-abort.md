---
id: P05
title: "Abort without wedging the DMA"
track: performance
level: 4
summary: Stopping a transfer the DMA still expects to finish.
needs: [I04]
repro: base
status: validated
hardware: [pfdaq-stream-src, dma]
software: [pfdaq-cap]
sources:
  - hw/rtl/pfdaq_stream_src.v:fill_mode,abort_fill,split_a,fill_beat,remaining
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:run_abort,fill_bad,untouched
  - tests/board/pfdaq_final_suite.sh:ABORT-2ms
evidence: [suite-freertos, suite-freertos-raw, suite-fresh-card-raw, sim-stream]
related: [K03, K09, H06, I04, P03, P04]
---

# Abort without wedging the DMA

After this module you can stop a burst in the middle and still end with a completed DMA transfer, a buffer you can interpret, and a system that is ready for the next run. You can also say what the stop does not save, and how to tell how long the "2 ms" abort really took.

The discovery of the rule behind this is a case study (K09). This module is the working description: what the hardware does, how the tool uses it, and what the measurements show.

## Before you start

- **Level 4.** You should have read I04 (the DMA's descriptors and the rule that only the full byte count completes a transfer). K09 tells the story of how that was found.
- **Tools:** the board with the shared baseline for the board runs; Icarus Verilog and Python for the simulation.
- **Time:** the two abort tests take under a second each.

## The idea

Starting a producer is easy. Stopping it is hard when the consumer has been promised something. Here the promise is the descriptor: the DMA has been told to expect, say, 8,192,000 bytes, and it will not report completion until it has received them. A producer that simply stops leaves the consumer waiting, and a DMA left waiting was cleared only by a reboot.

There are two ways out. The consumer can be told to give up, which needs a command this DMA does not offer. Or the producer can **keep its promise with worthless data**, and tell the software exactly where the real data ends. This system does the second.

## On this board

### Two kinds of stop

Both are bits of the `CTRL` register of the stream source, written like a button press.

| Bit | Name | What happens |
|---|---|---|
| 4 | `ABORT` | graceful: finish the frame in progress, then complete the transfer (below) |
| 5 | `FORCE_ABORT` | stop at once; the DMA may be left waiting and the board must be rebooted to clear it |

The source reports it in `STATUS` bit 2 (the last run was aborted) and keeps counting frames, so software can ask how many were really sent (`FRAMES_SENT`).

### What a graceful abort does, by mode

The header comment of the RTL puts it in one place:

```text
// Abort (CTRL.ABORT) always leaves the DMA with a completed transfer:
//   - real-time and TLAST_PER_FRAME modes: every frame is its own packet, so the frame in
//     progress is finished and the run stops.
//   - burst mode (one packet for the whole run): the frame in progress is finished, then the
//     rest of the run's FRAME_COUNT x (PAYLOAD_WORDS+4) words is sent as zero words, with TLAST
//     on the last one.
```

In the first case there is nothing to pad: each frame was its own transfer. In burst mode the whole run is one transfer of a known length, so the source finishes the frame it is in the middle of, and then sends zeros until the total is reached. The zero words come from one task:

```verilog
task fill_beat;
    begin
        run        <= R_FILL;
        out_valid  <= 1'b1;
        out_data   <= 64'd0;
        out_keep   <= (remaining == 32'd1) ? 8'h0F : 8'hFF;
        out_last   <= (remaining <= 32'd2);
```

`remaining` is the run's total word count, `FRAME_COUNT × (PAYLOAD_WORDS + 4)`, minus the words already sent. `TLAST` goes on the last beat, with the half-filled beat rule of module H02 when the count is odd.

Two details make it correct on the board, both found by testing:

- **No gap.** The first fill beat leaves in the same clock in which the abort is noticed (`abort_fill`), or in the same beat as the last word of the frame in progress (`split_a`). A one-clock `TVALID` gap in a packet is exactly what this DMA does not tolerate (K03), and an abort that landed on a frame boundary once produced one in simulation.
- **Zero never looks like data.** A real frame starts with a magic word that is not zero. The fill therefore cannot be mistaken for a frame, even by a verifier that slices the buffer wrongly.

### What the tool does

`pfdaq_cap -A <microseconds>` arms the **whole** burst, starts the source, sleeps, writes the abort and waits for the DMA interrupt (abridged):

```c
arm(BUF_OFF, (uint32_t)burst_bytes);
src[SRC_CTRL / 4] = 0x1;       /* START */
usleep(abort_us);
src[SRC_CTRL / 4] = 0x10;      /* ABORT */
int dma_ok = !wait_done(timeout_ms, &stat);
```

It then asks the source how many frames it really sent, verifies exactly those frames, checks that every word after them is zero, and checks that the 64 bytes **after** the end of the burst still hold the marker it wrote beforehand (0xEE), so that nothing was written past the descriptor's length. The test passes only if all of the following hold: the DMA completed, the aborted flag is set, fewer frames than requested were sent, the real frames have no error of any kind, the fill is all zero, and the guard bytes are untouched.

## What the measurements show

Two board runs in the suite, plus a clean run right after the second:

| | `ABORT-2ms` | `ABORT-50us` |
|---|---|---|
| Command | `-A 2000 -m prbs -w 4092 -f 500` | `-A 50 -m fir -w 252 -f 8000` |
| Burst requested | 500 frames × 16,384 bytes = 8,192,000 bytes | 8,000 frames × 1,024 bytes = 8,192,000 bytes |
| Frames really sent | 119 (1,949,696 bytes) | 128 |
| Zero-fill words | 1,560,576 | 2,015,232 |
| Non-zero words in the fill | 0 | 0 |
| DMA completed, status | yes, `0x210` | yes, `0x210` |
| Aborted flag | set | set |
| Start to interrupt | 8.899 ms | 8.904 ms |
| Guard bytes after the burst untouched | yes | yes |
| Errors in the real frames | none | none |

The fill-word counts are the exact complement of the real frames: 500 × 4,096 words minus 119 × 4,096 is 1,560,576, and 8,000 × 256 minus 128 × 256 is 2,015,232.

The run that follows (`ABORT-after`, 8,192 frames, counter payload, supervised) passes with no error: the source and the DMA are healthy after an abort.

Three conclusions:

1. **The abort does not make the transfer shorter.** Both runs take about 8.9 ms, which is how long a full 8.19 MB burst takes at about 920 MB/s. The fill is streamed at the same rate as real data. What the abort saves is data production and the software's work on it, not bus time.
2. **The requested abort time is not the real one.** The frame counts are the measurement. At the stream rate of module P01 (about 933 MB/s), 119 frames of 16,384 bytes are 2.09 ms of data, close to the 2 ms asked for. But 128 frames of 1,024 bytes are 131,072 bytes, about 0.14 ms, nearly three times the 50 µs requested: a `usleep` and two register writes from a Linux process do not take 50 µs. The second test gave 130 frames in the fresh-card run: a spread that fits a user-space sleep, not investigated further. This is arithmetic from the records, using the rate derived there, not a timestamp.
3. **Software must trust `FRAMES_SENT`, not its own clock.** That is why the tool reads it before verifying.

### In simulation

The simulation exercises the same rules with a checker that knows where `TLAST` must be. These lines are from running `make -C hw sim-stream` for this page:

```text
scenario 16:  50 frames x 104 words, mode 0, ... sent  13 ... ABORTED, then 3848 zero words
scenario 20:  40 frames x  37 words, mode 2, ... sent  21 ... ABORTED, then 703 zero words
scenario 21:  30 frames x  12 words, mode 0, ... sent   2 ... ABORTED, then 336 zero words
scenario 17:  20 frames x  14 words, mode 1, ... sent   1 ... ABORTED
scenario 18:  40 frames x  34 words, mode 0 rt/4 dest rate 100, ... sent 5 ... ABORTED
```

Scenarios 16 and 20 abort in the middle of a burst (20 with an odd frame length and the FIR payload), 21 aborts right after START, 17 is a per-frame run aborted between packets and 18 is a real-time run. The zero-word counts are again the complement: 50 × 104 minus 13 × 104 is 3,848. `FORCE_ABORT` into a stalled sink is also simulated and is the one case that leaves the DMA waiting by design.

## Run it

These are the commands the project ran. The board was not used to write this page.

```sh
pfdaq_cap -A 2000 -m prbs -w 4092 -f 500        # ABORT-2ms
pfdaq_cap -A 50   -m fir  -w 252  -f 8000       # ABORT-50us
pfdaq_cap -r -m counter -w 252 -f 1024 -n 8     # ABORT-after: a clean run right afterwards
```

And the simulation, which I ran into a scratch directory:

```sh
make -C hw sim-stream
```

## What you should see

The JSON line of the first board run, trimmed:

```text
{"pass": true, "mode": "abort", "payload_mode": 1, "frames_requested": 500, "abort_after_us": 2000,
 "dma_completed": true, "dma_stat": "0x210", "start_to_irq_s": 0.008899, "status": "0x6", "aborted_flag": true,
 "frames_sent": 119, "bytes": 1949696, "bad_magic": 0, "bad_seq": 0, "bad_payload": 0, "bad_crc": 0,
 "fill_words": 1560576, "fill_nonzero": 0, "nothing_after_burst": true}
```

`"status": "0x6"` is "done" together with "aborted".

## Evidence

- **Board, fabric v0.5.1:** both abort runs and the clean run after them (records `suite-freertos`, `suite-freertos-raw`), and the same runs in the fresh-card suite (`suite-fresh-card-raw`: 119 and 130 frames sent, fills of 1,560,576 and 2,014,720 words, all zero).
- **Simulation:** scenarios 16, 17, 18, 20 and 21 (record `sim-stream`), re-run for this page.
- **Not tested on the board:** `FORCE_ABORT`, an abort in real-time mode, and an abort while the supervisor is armed and reporting. The board suite exercises burst-mode abort only.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| `dma_completed` is false | the transfer was cut with an early `TLAST`, or with `FORCE_ABORT`; the DMA waits for the rest and a reboot clears it (K09) |
| `fill_nonzero` is not zero, or `nothing_after_burst` is false | not seen on the board; the checks exist to catch a fill that is not all zeros and a transfer that runs past the descriptor's length |
| `frames_sent` is the full request | the abort came after the burst had finished; the test then fails because it expects fewer frames |
| The result of the next run is bad | the DMA was left waiting by an earlier forced stop; reboot first |

## Trade-offs and going further

- **Padding costs bus time.** An abort 2 ms into an 8.9 ms job still occupies the bus for 8.9 ms. The alternative is one transfer per frame, which makes abort trivial but needs a descriptor re-armed per frame (K04). Real-time mode takes that route and uses credits to make it safe.
- **A shorter descriptor is not a way out once the burst has started.** Changing the descriptor late is the kind of re-arm that this core was seen to ignore (K04).
- **The zero fill is a convention.** Software that does not read `FRAMES_SENT` cannot tell where the data ends; it can only guess from the magic word.
- **Exercise:** for a 500-frame burst of 4,092-word payloads, predict `fill_words` for an abort after 40 frames, then predict how long the DMA takes, and check that it does not depend on the 40.
- **Exercise:** the jitter of a `usleep`-based abort is hundreds of microseconds. Design a way to stop at an exact frame count, and say what it would need from the fabric.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [I04 DMA stream descriptors: the rules this core really follows](I04-dma-stream-descriptors.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [CoreAXI4DMAController](../system/README.md#dma) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `fill_mode`, `abort_fill`, `split_a`, `fill_beat`, `remaining`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `run_abort`, `fill_bad`, `untouched`<br>[`tests/board/pfdaq_final_suite.sh`](../../tests/board/pfdaq_final_suite.sh): `ABORT-2ms` |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the fresh-card suite](../../evidence/v0.5.1/board/suite-fresh-card.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15) |
| **Related** | [K03 A DMA core that needs TVALID without gaps](../case-studies/K03-tvalid-gaps.md)<br>[K09 An early TLAST does not end a transfer](../case-studies/K09-early-tlast.md)<br>[H06 Real-time sources: slots, credits and honest drops](H06-real-time-source.md)<br>[I04 DMA stream descriptors: the rules this core really follows](I04-dma-stream-descriptors.md)<br>[P03 Fault injection from the fabric up](P03-fault-injection.md)<br>[P04 Deadline supervision, alarm and recovery](P04-deadline-supervision.md) |
| **Status** | validated |
<!-- nav:end -->
