---
id: H05
title: "A FIR filter on math blocks, decoupled by a FIFO"
track: fpga
level: 3
summary: A pipelined 8-tap filter that feeds a stream which must never pause.
needs: [H02]
repro: sim
status: validated
hardware: [pfdaq-fir-src, pfdaq-stream-src]
software: [pfdaq-cap]
sources:
  - hw/rtl/pfdaq_stream_src.v:pfdaq_fir_src,tri16,issue,mem_after,fir_ok,fir_pop,underrun,R_PREFILL
  - hw/tb/check_trace.py:tri16
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:tri16,payload_bad,model_restart
evidence: [sim-stream, timing, suite-freertos, suite-freertos-raw]
related: [H02, H04, H06, H11, P01]
---

# A FIR filter on math blocks, decoupled by a FIFO

After this module you can describe how a multiply-accumulate filter is pipelined for one result per clock per lane, why a small FIFO sits between the filter and the stream, and how a fixed-size credit rule keeps that FIFO from overflowing. You can also reproduce, in Python, the exact 32-bit word the hardware sends, and see what happens to the stream when the start-up margin is removed.

## Before you start

- **Level 3.** Read module H02 first: the point of this design is that the stream must not pause inside a packet. Familiarity with signed multiplication and 2's complement helps.
- **Tools:** Icarus Verilog 12 and Python 3. No board.
- **Time:** the simulation runs in under a minute; the experiments add a few minutes each.

## The idea

A **finite impulse response (FIR) filter** replaces every input sample with a weighted sum of the last few inputs. For an 8-tap filter with coefficients `c[0..7]`:

```
y[n] = c[0]*x[n] + c[1]*x[n-1] + ... + c[7]*x[n-7]
```

Each output needs eight multiplications and seven additions. Doing that in one clock is too long a path for 125 MHz; the standard remedy is a **pipeline**: cut the work into stages separated by registers, so that every stage does a little and a new input enters on every clock. The first result appears after the pipeline depth, but after that one result leaves per clock.

A pipeline has a latency, and a stream that cannot pause (module H02) does not tolerate latency that appears in the middle of a packet. The usual answer is a **FIFO** between the producer and the consumer, filled before the packet starts, so that the consumer always finds data. A FIFO also needs a rule against overflow, because the pipeline keeps producing for a few clocks after the consumer stops asking. This design uses a **credit rule**: before starting new work, check that the FIFO will have room for everything already in flight plus the new work.

## On this board

The payload mode FIR is selected with `MODE[1:0] = 2`. The input is a synthetic triangle wave, so that software can reproduce it exactly without any real signal.

### The input

Sample `i` since START is

```
x[i] = tri((i + 1) * STEP mod 2^16)
```

where `STEP` is the `FIR_STEP` register and, for a 16-bit phase `p`:

```
t      = p[15] ? ~p[14:0] : p[14:0]        // fold the phase into a ramp 0..32767..0
tri(p) = t - 16384                         // signed 16 bit, range -16384 .. +16383
```

The phase starts at 0 on every START, so the first sample is `tri(STEP)`, not `tri(0)`, and the wave runs on without a break across all frames of one run. `STEP` sets the frequency: the triangle repeats about every `65536 / STEP` samples.

### The filter

`COEF0..COEF7` are signed 16-bit registers, tap 0 applying to the newest sample. The sum wraps to 32 bits (it is not saturated). Samples before the start count as zero, so the first outputs are partial sums. In `hw/rtl/pfdaq_stream_src.v` the module is `pfdaq_fir_src`.

The generator needs two payload words per clock (module H02), so the filter produces **two outputs per clock**, `y[n]` and `y[n+1]`. It does so by keeping a 9-sample window: `y[n]` uses `x[n]` down to `x[n-7]`, and `y[n+1]` uses `x[n+1]` down to `x[n-6]`. The pipeline has four stages:

| Stage | What it holds | Registers |
|---|---|---|
| 1 | Two new samples, and the window around them | `w[0..8]` |
| 2 | Sixteen products, `c[k] * sample` for both outputs | `p0[0..7]`, `p1[0..7]` |
| 3 | Eight partial sums, pairs of products | `q0[0..3]`, `q1[0..3]` |
| 4 | Two results, each the sum of four partial sums | `y0`, `y1` |

The products are signed 16 × 16 → 32 bit multiplications, written plainly as `c[k] * w[k + 1]`; the synthesis tool decides how to implement them. Whether they were mapped to the device's dedicated math blocks, and how many such blocks the design uses, is not recorded in the published build extract; the architecture notes state that the filter runs on the fabric's multipliers.

### The FIFO and the credit rule

The four stages end in a 16-word FIFO (`mem`, write pointer `wp`, read pointer `rp`, fill level `count`). The generator takes zero, one or two words per clock from the front. Two registered outputs, `head0` and `head1`, show the next two words. They are computed one clock ahead by `mem_after`, which also forwards a word that is being written in the same clock, so the generator never has to read the RAM-like array and wait.

The rule that prevents overflow is one line:

```verilog
wire [2:0] inflight = {2'd0, v1} + {2'd0, v2} + {2'd0, v3} + {2'd0, v4};
wire       issue    = run && ({1'b0, count} + {2'd0, inflight, 1'b0} + 6'd2 <= 6'd16);
```

`v1..v4` say which pipeline stages hold valid data. Each valid stage will deliver two words, so `inflight * 2` words are on their way. A new pair of samples is issued only if what is in the FIFO, plus what is on its way, plus the two new outputs, still fits in 16. If the generator stops, the pipeline drains into the free space that was reserved, and nothing is lost.

### Never running dry

On the consumer side, the generator in `pfdaq_stream_src` checks before every beat:

```verilog
wire fir_ok = (cfg_mode != 2'd2) || ({3'd0, fir_pop_need} <= fir_count);
```

It sends a beat only when the FIFO holds as many words as the beat needs. A packet cannot be paused without breaking the DMA's rule, so the design avoids the situation instead of handling it. A run in FIR mode starts in a state `R_PREFILL`, in which the filter runs and the generator waits until the FIFO holds 12 words:

```verilog
R_PREFILL: begin
    if (abort_pend)
        end_run(1'b1);
    else if (fir_count >= 5'd12)
        run <= cfg_rt ? R_RTWAIT : R_RUN;
end
```

If, despite that, a beat is wanted and the FIFO cannot supply it, the source records it: `underrun <= 1'b1` when the generator is ready but `fir_ok` is false. STATUS bit 3 shows the flag. It is cleared only by reset, not by START, so one underrun stays visible until the fabric is reset.

### Software's side

Software recomputes the same words. `pfdaq_cap.c` has `tri16` and the filter loop in `payload_bad`; `model_restart` clears the sample count and the history at the same moment the hardware restarts at START. The hardware wraps its sum at 32 bits; the software computes in 64 bits and compares the low 32, which is the same number. The checker in `hw/tb/check_trace.py` is a third implementation, in Python.

## Run it

1. Run the stream simulation into a directory of your own:

   ```sh
   make -C hw sim-stream OUT=/tmp/pfdaq-h05
   ```

2. Four scenarios use FIR. Their parameters are in `hw/tb/tb_pfdaq_stream_src.v`:

   | Scenario | Frame | Mode | Step | What it exercises |
   |---|---|---|---|---|
   | 11 | 33 words × 5 | burst, `TREADY` 60 % | 1000 | the filter under backpressure |
   | 12 | 504 words × 3 | burst, no backpressure | 77 | the filter at full rate, frames longer than the FIFO |
   | 14 | 40 words × 20 | real-time, 2 destinations | 321 | real-time slots with drops: only frames that are sent consume filter samples (the checker's model advances the same way) |
   | 20 | 33 words × 40 | burst, abort requested 217 APB clocks after START | 555 | a stop in the middle of a filtered burst, odd frame length |

   Find their lines in the output:

   ```sh
   grep -E 'scenario +(11|12|14|20):' /tmp/pfdaq-h05/result_stream.txt
   ```

3. Check the first outputs by hand. The testbench sets the coefficients to 12000, −15000, 20000, 32767, −32768, 20000, −15000 and 12000. For scenario 11 (`STEP = 1000`) run:

   ```sh
   python3 - <<'EOF'
   c = [12000, -15000, 20000, 32767, -32768, 20000, -15000, 12000]
   def tri(p):
       p &= 0xFFFF
       t = (~p & 0x7FFF) if p & 0x8000 else p & 0x7FFF
       return t - 16384
   x = [tri((i + 1) * 1000) for i in range(8)]
   y = [sum(c[k] * x[n - k] for k in range(8) if n - k >= 0) & 0xFFFFFFFF for n in range(4)]
   print(x)
   print(['%08x' % v for v in y])
   w = []
   for line in open('/tmp/pfdaq-h05/sim/trace.txt'):
       p = line.split()
       if p and p[0] == 'B' and p[1] == '11':
           w.append(int(p[2], 16))
           if len(w) == 7: break
   print(['%08x' % v for v in w[3:7]])
   EOF
   ```

4. **Break it: remove the start-up margin.** In a scratch copy of `hw/`, change the prefill threshold:

   ```verilog
   else if (fir_count >= 5'd0)
   ```

   and run `make sim-stream OUT=...` from the copy. Then try `5'd1` and `5'd2`.

## What you should see

Step 1 ends with `RESULT: PASS`. Step 2 prints the four FIR lines, from the run made for this page:

```
scenario 11:   5 frames x  37 words, mode 2, ready  60%, sent   5 dropped   0, detected crc=[] seq=[] pay=[]
scenario 12:   3 frames x 504 words, mode 2, ready 100%, sent   3 dropped   0, detected crc=[] seq=[] pay=[]
scenario 14:  20 frames x  44 words, mode 2 rt/2 dest rate 20, ready 100%, sent  10 dropped  10, detected crc=[] seq=[] pay=[]
scenario 20:  40 frames x  37 words, mode 2, ready  70%, sent  21 dropped   0, detected crc=[] seq=[] pay=[] ABORTED, then 703 zero words
```

("37 words" counts the four header and CRC words in addition to the 33 of payload.) The checker did not raise an underrun or a payload error in any of them.

Step 3 prints, from the same run:

```
[-15384, -14384, -13384, -12384, -11384, -10384, -9384, -8384]
['f4ff1b00', '03775440', 'f0f2ba80', 'd3ea5cd8']
['f4ff1b00', '03775440', 'f0f2ba80', 'd3ea5cd8']
```

The first line is the triangle input: the first sample is `tri(1000) = 1000 − 16384`. The second is the filter output computed in Python. The third is what the hardware actually sent, words 3 to 6 of the first frame. They are the same. The first output is just `c[0] * x[0]` (12000 × −15384, wrapped to 32 bits) because the history is zero; the second is `c[0] * x[1] + c[1] * x[0]`, and so on.

Step 4, with the threshold at 0, fails:

```
RESULT: FAIL
   trace: VALID_GAP 111604000
   trace: VALID_GAP 111612000
   trace: VALID_GAP 111620000
   ...
   scenario 11: FIR underrun flag set
   scenario 12: FIR underrun flag set
   ...
```

In this run each of the three burst scenarios 11, 12 and 20 had three `VALID_GAP` lines, nine in all. In scenario 11 the trace shows the header beat (magic and sequence number) at 111,596 ns, then three clocks with no beat, then the timestamp and the first payload word at 111,628 ns. Without prefill, the beat that needs the first filter output has to wait for the four-stage pipeline to produce it. The underrun flag is then set; because only reset clears it, the checker reports it for every scenario after 11, including those that do not use FIR.

With the threshold at 1 and at 2 the simulation passed, with no gap and no underrun. In these scenarios the pipeline fills fast enough that one word of margin is enough. The value 12 in the design therefore gives a larger margin than these scenarios need. Why 12 was chosen, and what the true minimum is under other step sizes, frame lengths or backpressure patterns, is not recorded.

## Evidence

- **Simulation, fabric v0.5.1:** all 20 scenarios pass, including the four above, with the filter's output recomputed independently for every frame (record `sim-stream`, command `make -C hw sim-stream`). The experiment in step 4 was run for this page on a scratch copy and is not part of that record.
- **Board, fabric v0.5.1:** FIR bursts verify with no sequence, payload or CRC error: `T4-fir` (32,768 frames, 33.6 MB) and `T4-fir-37x1004` (592 frames of 1004 payload words, 2.4 MB). The three FIR fault tests, `T9-crc-fir`, `T9-seq-fir` and `T9-pay-fir`, each detect exactly the faults injected (record `suite-freertos`).
- **Cost of checking the filter in software**, from the raw suite output (record `suite-freertos-raw`, one run each, one Linux core doing the work):

  | Payload | End-to-end | Verification | Copy |
  |---|---|---|---|
  | counter (`T4-counter`) | 29.60 MB/s | 55.4 MB/s | 70.5 MB/s |
  | FIR (`T4-fir`) | 8.70 MB/s | 10.2 MB/s | 66.8 MB/s |

  The hardware streams the FIR payload as fast as the counter payload. What changes is the software's check: eight multiplications per word instead of one comparison, which brings the verification rate from 55.4 to 10.2 MB/s and the whole path from 29.6 to 8.7 MB/s. The copy rate hardly moves. That the software model alone sets the FIR rate follows from these three columns; no profile of the loop was taken.
- **Build, fabric v0.5.1:** timing met in all corners and the design uses 26.02 % of the fabric's logic elements (record `timing`). The filter's share of that is not published.

The simulation shows the logic and the stream shape. It does not show timing, which the build record covers only for the design as a whole.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| Software reports a payload mismatch only in FIR mode, from the first word | `STEP` or a coefficient differs between hardware and model; or the model's phase starts at `tri(0)` instead of `tri(STEP)` |
| The first words of every run are right, a later frame is wrong | History not carried across frames in the model; the filter state continues across frames of one run |
| The second run in a row differs from the first | The model was not restarted at START (`model_restart`) |
| `VALID_GAP` in simulation, only at the start of FIR packets | The start-up margin is too small for the pipeline depth (step 4) |
| STATUS bit 3 set, and stays set | An underrun happened since reset; it is cleared by reset only |
| Overflowed FIFO, repeated or missing outputs | The credit rule counts too few words in flight, for example after adding a pipeline stage without a `v` flag |

## Trade-offs and going further

- **Two outputs per clock** double the multipliers (sixteen instead of eight) and the adders. The alternative, one output per clock and a stream half as fast, fails the continuity rule of module H02.
- **Wrap, not saturate.** The 32-bit sum wraps on overflow. That is easy to model and exact on both sides; a real filter chain would saturate or scale. The testbench deliberately uses large coefficients of mixed signs so that the sum does wrap, and the checker confirms the wrapped values.
- **A deep FIFO costs area and latency; a shallow one costs margin.** The pipeline can hold 8 words in flight, so the 16 words leave 8 of margin; whether a smaller FIFO and a smaller prefill would still meet the rule was not explored.
- **The software model is the slow part.** Vectorised code or a lookup of the triangle phase would raise the 10.2 MB/s of verification. Module P01 treats the whole chain.
- **Exercise:** change `COEF` so that the filter is a pure delay (`c[3] = 1`, all others 0) and check that the stream equals the input shifted by three samples.
- **Exercise:** raise `STEP` until the triangle has fewer than eight samples per period. Which taps now see the same phase?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md) |
| **How to run** | Simulation only: Icarus Verilog and Python, no board |
| **In the system** | [FIR payload source](../system/README.md#pfdaq-fir-src) · [Stream source](../system/README.md#pfdaq-stream-src) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `pfdaq_fir_src`, `tri16`, `issue`, `mem_after`, `fir_ok`, `fir_pop`, `underrun`, `R_PREFILL`<br>[`hw/tb/check_trace.py`](../../hw/tb/check_trace.py): `tri16`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `tri16`, `payload_bad`, `model_restart` |
| **Evidence** | [Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md)<br>[H04 CRC-32 at 64 bits per clock](H04-crc32-two-words-per-clock.md)<br>[H06 Real-time sources: slots, credits and honest drops](H06-real-time-source.md)<br>[H11 Verification with an independent model](H11-independent-model.md)<br>P01 Where 940 MB/s becomes 30: throughput layer by layer *(planned)* |
| **Status** | validated |
<!-- nav:end -->
