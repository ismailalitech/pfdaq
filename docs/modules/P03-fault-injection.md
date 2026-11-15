---
id: P03
title: "Fault injection from the fabric up"
track: performance
level: 3
summary: Corrupting a CRC, skipping a sequence number and flipping a payload bit on purpose, and proving each is caught.
needs: [I06]
repro: base
status: validated
hardware: [pfdaq-stream-src]
software: [pfdaq-cap, supervisor]
sources:
  - hw/rtl/pfdaq_stream_src.v:pay_word,crc_word,fault_here,n_seq
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:fault_every,fault_frame,injected,exp_crc,exp_seq_err,exp_pay
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:seq_mismatch,max_lag
  - tests/board/pfdaq_final_suite.sh:T9
evidence: [suite-freertos, suite-freertos-raw, supervisor-console, sim-stream]
related: [H01, H04, H05, H11, I06, K09, K10, P04]
---

# Fault injection from the fabric up

After this module you can make the hardware produce one precisely chosen error, say what the verifier should report for it before you run anything, and read the result as a test of the checker rather than of the data. You will also see what each layer of this system notices and what it does not.

## Before you start

- **Level 3.** You should have read I06 (how a capture is verified) and know the frame layout: header word, sequence number, timestamp, payload, CRC-32.
- **Tools:** the board with the shared baseline for the board runs; Icarus Verilog and Python for the simulation.
- **Time:** the nine board runs take about twelve seconds of loop time in total (from the records), most of it the three FIR runs.

## The idea

A checker that has never been seen to fail has not been shown to work. The only way to know that a CRC check, a sequence check and a payload check really fire is to cause each failure on purpose, once, at a known place, and see exactly that failure and nothing else reported.

Good fault injection is **controlled** (one fault, at a chosen frame), **observable** (the expected counters are known in advance) and **reversible** (the next run is clean). This system injects at the earliest point it can, in the fabric that generates the data, so that every layer after it, the DMA, the memory, the copy and the software model, is part of what is tested.

## On this board

### What the fabric does

The `FAULT` register at offset `0x18` of the stream source has three enable bits and a frame index. It is read when START is written (module H01: configuration is sampled at START), so it must be written first.

| Bits | Meaning |
|---|---|
| 0 | corrupt the CRC |
| 1 | skip one sequence number |
| 2 | flip a payload bit |
| 31:16 | index of the frame, counted from zero within the run |

In the RTL the three faults are three small expressions:

```verilog
wire [31:0] pay_word  = (fault_here && cfg_fault_en[2] && (pay_idx == 16'd0)) ? (pay_good ^ 32'h00000100) : pay_good;
wire [31:0] crc_word  = (fault_here && cfg_fault_en[0]) ? (crc_final ^ 32'h00000001) : crc_final;
...
n_seq = seq + 32'd1 + ((cfg_fault_en[1] && fault_next && !last_frame) ? 32'd1 : 32'd0);
```

- **CRC fault:** bit 0 of the CRC word of the chosen frame is inverted. Everything else is intact.
- **Payload fault:** bit 8 of the first payload word of the chosen frame is inverted. The CRC is computed over the intended payload, before the flip (module H04), so this frame carries a payload that does not match its own CRC. One injected fault therefore produces two detections.
- **Sequence fault:** the sequence number jumps by one on the way into the chosen frame, so that number is never sent. Frame zero is a special case, handled when the run starts.

### What the tool does

`pfdaq_cap` takes `-F crc|seq|payload`, `-i` for the frame index and `-e K` to inject in every K-th burst, counting from one:

```c
int inj = fault && fault_every && (b % fault_every == fault_every - 1);
src[SRC_FAULT / 4] = inj ? ((fault_frame << 16) | fault) : 0;
```

The register is rewritten before every burst, with zero for a burst that is not to carry a fault, so a fault never outlives its burst. After the run the tool compares what the verifier counted with what it expects, and only then reports a pass:

```c
uint64_t exp_crc = (fault == 1 || fault == 4) ? injected : 0;
uint64_t exp_seq_err = (fault == 2) ? injected : 0;
uint64_t exp_pay = (fault == 4) ? injected : 0;
```

A test passes only if the counts are **equal** to the number of injections, not merely non-zero. A verifier that reported ten errors for four faults would fail the test.

### The suite's nine runs

For each of the three payload types the board suite runs three tests, 20 bursts of 1,024 frames each:

| Test | Command (payload type varies) | Bursts that carry a fault | Injected |
|---|---|---|---|
| CRC | `-F crc -i 7 -e 5` | 5th, 10th, 15th, 20th | 4 |
| Sequence | `-F seq -i 0 -e 4` | 4th, 8th, 12th, 16th, 20th | 5 |
| Payload | `-F payload -i 1023 -e 10` | 10th, 20th | 2 |

The payload fault goes into the last frame of the burst, frame 1023. The reason for that choice is not recorded.

## What each layer reports

From the board record (fabric v0.5.1, FreeRTOS supervisor), the same counts in all three payload types:

| Fault | Injected | Linux verifier counts | Supervisor "linux errors" at the end of the run |
|---|---|---|---|
| CRC | 4 | `bad_crc` 4; sequence 0, payload 0 | 4 |
| Sequence | 5 | `bad_seq` 5; CRC 0, payload 0 | 5 |
| Payload | 2 | `bad_payload` 2 **and** `bad_crc` 2; sequence 0 | 4 |

The last row is the double detection described above: two payload faults give two payload errors and two CRC errors, so the supervisor, which is sent the sum, reports 4. Each of the nine runs passed with exactly these counts, and in every one the supervisor round trip also passed.

Three observations from the same records:

- **The supervisor counts, it does not alarm.** It is told the number of errors in every status message and prints it when it disarms (`linux errors 5`). Its deadline and its sequence cross-check (module P04) stayed quiet in all nine runs: `seq_mismatch` is 0 everywhere.
- **Why a skipped sequence number does not trip that cross-check.** The supervisor compares the fabric's sequence counter with the number Linux expects next. Linux resynchronises on the number it sees, so both sides skip together. The fabric's counter does run ahead of its frame counter by the number of numbers skipped, cumulatively since the fabric was reset: the record shows the difference growing by five after each sequence run (5, 10, 15 over the three payload types).
- **A fault in real-time mode.** The suite also injects one payload fault at frame 10 of a real-time run (`T6-rt-fault`). The record shows 1,000 frames sent, none dropped, one payload error and one CRC error.

## Run it

These are the commands the project ran. The board was not used to write this page; the results are in the records above.

```sh
for m in counter prbs fir; do
  pfdaq_cap -r -m $m -w 252 -f 1024 -n 20 -F crc     -i 7    -e 5
  pfdaq_cap -r -m $m -w 252 -f 1024 -n 20 -F seq     -i 0    -e 4
  pfdaq_cap -r -m $m -w 252 -f 1024 -n 20 -F payload -i 1023 -e 10
done
```

The same three faults exist in simulation, where the checker is the independent Python model (module H11). This is a command I ran for this page:

```sh
make -C hw sim-stream
```

Among its 20 scenarios:

```text
scenario  3:   6 frames x  12 words, mode 0, ready  70%, ... detected crc=[2] seq=[] pay=[]
scenario  4:   6 frames x  12 words, mode 0, ready  70%, ... detected crc=[] seq=[1] pay=[]
scenario  5:   4 frames x  12 words, mode 0, ready  50%, ... detected crc=[0] seq=[] pay=[0]
scenario 15:  12 frames x  24 words, mode 1 rt/3 dest rate 60, ... detected crc=[1] seq=[] pay=[1]
```

The numbers in brackets are frame indices: scenario 3 corrupts the CRC of frame 2, scenario 4 skips a sequence number before frame 1, scenario 5 flips a payload bit of frame 0 and so is caught by both the payload and the CRC check.

## What you should see

One line per run, with the expected counts. For the CRC run in counter mode:

```text
"bad_magic": 0, "bad_seq": 0, "bad_payload": 0, "bad_crc": 4, "faults_injected": 4, ...
"supervisor": {"pass": true, ... "seq_mismatch": 0, ...}
```

## Evidence

- **Board, fabric v0.5.1:** nine runs with exact detection (record `suite-freertos`, raw JSON in `suite-freertos-raw`), the supervisor's side of the same runs (record `supervisor-console`) and the real-time fault.
- **Simulation:** scenarios 3, 4, 5 and 15 of the stream simulation (record `sim-stream`), also re-run for this page.
- **Not tested:** faults that start inside the DMA or the memory. The injection point is the fabric's own output, so it proves the path from there to the verifier. A real defect of that kind was found without injection (K10), by the same checker, which reported 3 sequence, 4 payload, 4 CRC errors and 121 bad headers.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| The run fails with more errors than faults | a fault that is not injected: a real defect, or the odd-length case (K10) |
| A payload fault shows only one error | one of the two checks did not run: by construction both the payload and the CRC must fire |
| The fault appears in the run after the one you meant | the register was written after START; it is sampled at START (H01) |

## Trade-offs and going further

- **Inject early, detect late.** Injecting in the fabric exercises the longest path, but it cannot tell you which stage would have caught a fault that began in memory.
- **One fault per burst keeps the arithmetic exact.** A random fault density would test more combinations but would make "exactly the number injected" impossible to state.
- **Exercise:** predict the verifier's three counts for `-F payload -i 0 -e 1` over 20 bursts (every burst carries a fault), then compare with what the tool's expected-count rule would demand.
- **Exercise:** the supervisor does not alarm on errors. Decide what it should do, and what that would cost in false alarms during a real defect.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [I06 Capturing and verifying a stream](I06-capture-and-verify.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `pay_word`, `crc_word`, `fault_here`, `n_seq`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `fault_every`, `fault_frame`, `injected`, `exp_crc`, `exp_seq_err`, `exp_pay`<br>[`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `seq_mismatch`, `max_lag`<br>[`tests/board/pfdaq_final_suite.sh`](../../tests/board/pfdaq_final_suite.sh): `T9` |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Supervisor console during the fresh-card suite, with the alarm and recovery messages](../../evidence/v0.5.1/board/supervisor-console.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15) |
| **Related** | [H01 An APB register block that software can trust](H01-apb-register-block.md)<br>[H04 CRC-32 at 64 bits per clock](H04-crc32-two-words-per-clock.md)<br>[H05 A FIR filter on math blocks, decoupled by a FIFO](H05-fir-on-math-blocks.md)<br>[H11 Verification with an independent model](H11-independent-model.md)<br>[I06 Capturing and verifying a stream](I06-capture-and-verify.md)<br>[K09 An early TLAST does not end a transfer](../case-studies/K09-early-tlast.md)<br>[K10 Odd frame lengths lose 2 KB chunks](../case-studies/K10-odd-frame-lengths.md)<br>[P04 Deadline supervision, alarm and recovery](P04-deadline-supervision.md) |
| **Status** | validated |
<!-- nav:end -->
