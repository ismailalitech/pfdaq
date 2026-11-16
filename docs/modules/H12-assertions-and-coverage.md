---
id: H12
title: "Assertions and coverage for the stream source"
track: fpga
level: 4
summary: Which behaviours the scenarios really exercise, and protocol rules written as checks.
needs: [H11]
repro: sim
status: validated
hardware: [pfdaq-stream-src]
sources:
  - hw/tb/pfdaq_stream_checks.v:pfdaq_stream_checks,pfdaq_apb_snoop
  - hw/tb/pfdaq_stream_cover.v:pfdaq_stream_cover
  - hw/tb/tb_pfdaq_assert.v
  - hw/Makefile
  - hw/rtl/pfdaq_stream_src.v:pfdaq_stream_src,w_ready,out_valid,fabort_pulse_a
  - experiments/lab-assertions/planted_bugs.py:BUGS
  - experiments/lab-assertions/extra_runs.py
  - experiments/lab-assertions/recount_trace.py
  - experiments/lab-assertions/assert_dump.v
  - experiments/lab-assertions/vcd_to_wave.py
evidence: [sim-assert, sim-assert-coverage, sim-assert-recount, sim-assert-extra-runs, sim-planted-bugs, sim-planted-bug-w-ready, sim-planted-bug-emit, sim-planted-bug-force-abort]
related: [H11, H02, H06, H07]
---

# Assertions and coverage for the stream source

After this module you can write protocol rules as named checks that run on every clock of a simulation, attach them to an existing testbench without changing it, count which situations the scenarios actually reach, read the holes in that count, and prove that each check can fail by planting a bug.

## Before you start

- **Level 4.** You should have read H11 (the testbench records, a separate program judges) and H02 (the stream handshake). H06 helps for the real-time rules.
- **Tools:** Icarus Verilog 12.0 (`iverilog -g2012`, because the reports use `final`) and Python 3. No board, no vendor tools.
- **Time:** the run takes about 5 seconds. The eleven planted bugs take about a minute.

## The idea

H11 ended with a list of limits. Two of them were: nothing says which situations the scenarios reached, and the per-clock rules were only three monitors written inside the testbench. This module adds two things.

An **assertion** is a rule that must hold at every clock, with a name, checked while the simulation runs. When it fails you get the rule, the time and the scenario, not a mismatch found later in a file. The Python model of H11 judges what was transferred; an assertion judges how it was transferred.

**Coverage** answers a different question. A passing run says the design broke no rule in the situations the testbench created. It says nothing about situations the testbench never created. Coverage counts the situations: each *bin* is a name, a condition and a counter. A bin at zero is a behaviour no scenario reached.

Both can lie, in opposite ways:

- A check that never fires may be a check that cannot fire (it is *vacuous*). The only way to know is to break the design on purpose and see it fire. Every rule below has been seen to fire.
- A coverage table full of non-zero counts shows the situations were reached. It does not show the design handled them well. That is still the checks' job.

This project's checks are ordinary procedural Verilog, with no SystemVerilog assertion or covergroup syntax. They use two features from SystemVerilog that Icarus 12 accepts with `-g2012`: `final` blocks and `void` functions.

![The existing stream testbench is instantiated unchanged inside a new top level, tb_pfdaq_assert. A snoop module copies the APB register writes, the checker watches the stream ports and uses the snoop's copy of the run configuration, and the coverage module takes events from the checker and probes from the design. At the end of the run the checker and the coverage module print their reports.](../diagrams/lab-h12-where-checks-sit.svg)

1. **The testbench is not modified.** `tb_pfdaq_stream_src.v` is instantiated as `tb` inside `tb_pfdaq_assert.v`. It still drives the APB bus, sets `TREADY` at random and writes its trace. The trace is byte-identical to the one of `make -C hw sim-stream`.
2. **The new top reads its wires** by hierarchical reference (`tb.TVALID`, `tb.PADDR`, and so on) and passes them to the new modules.
3. **`pfdaq_apb_snoop` copies the configuration.** It watches register writes on the APB bus and, at each `START` write, keeps what the run was told to do: payload words, frame count, run kind, payload mode, destinations, fault bits.
4. **`pfdaq_stream_checks` watches the stream ports** and uses the snoop's copy as the specification of the run. It never reads the design's internal registers, so a bug in the design's own copy of the configuration cannot hide.
5. **`pfdaq_stream_cover` counts.** Most bins come from the checker's events. Five signals inside the design are read as probes (the dashed arrow): the run state, the abort pulse, the drop event, the credits and the pending-abort flag.
6. **A `final` block prints both reports** when the testbench calls `$finish`: violations per rule, the coverage table, and the line `ASSERT RESULT: PASS` or `FAIL`.

## On this board

The stream source feeds the vendor DMA. The interface is 64 bits wide at 125 MHz and carries two 32-bit words per beat. A frame is `PAYLOAD_WORDS + 4` words: magic, sequence number, timestamp, payload, CRC. A packet (what `TLAST` closes) is the whole burst, or one frame in per-frame and real-time modes. Module H02 describes the handshake and the byte lanes; H06 the real-time slots and the destinations.

This module is simulation only. Nothing was run on the board for it.

### The 18 rules

Each rule comes from a place that is not the checker: the AXI4-Stream handshake (H02), the DMA's needs (K03), the frame layout and run modes in the header comment of `hw/rtl/pfdaq_stream_src.v`, or the properties H11 recomputes in Python.

| # | Name | The rule | Where it comes from |
|---|---|---|---|
| 1 | `VALID_DROP` | If `TVALID` was high and `TREADY` low at one clock, `TVALID` is high at the next | H02, rule 2 |
| 2 | `DATA_STABLE` | ... and `TDATA` has not changed | H02, rule 2 |
| 3 | `SIDE_STABLE` | ... and `TLAST`, `TKEEP`, `TSTRB`, `TDEST` and `TID` have not changed | H02, side-band signals |
| 4 | `KEEP` | `TKEEP` equals `TSTRB`, and is `FF`, or `0F` on a `TLAST` beat | H02, "On this board" |
| 5 | `VALID_GAP` | `TVALID` is never low while a packet is open | K03 |
| 6 | `MAGIC` | The first word of a frame is `A55A0000` plus `PAYLOAD_WORDS`, and the next header comes `PAYLOAD_WORDS + 4` words later | frame layout in the RTL header |
| 7 | `SEQ` | The sequence number rises; from frame to frame of one non-real-time run without the sequence-skip fault it rises by exactly 1 | H11, "Sequence" |
| 8 | `TIMESTAMP` | The timestamp word rises from frame to frame | H11, "Timestamps" |
| 9 | `TLAST_EARLY` | `TLAST` is not raised before the last word of the packet | H11, "`TLAST`" |
| 10 | `TLAST_LATE` | `TLAST` is raised on the last word of the packet, not after it | H11, "`TLAST`" |
| 11 | `TLAST_LONG` | A `TLAST` beat does not carry words beyond the packet length | H11, "`TLAST`" |
| 12 | `TDEST` | Burst: always 0. Real-time: packet *k* of the run uses destination *k* mod `TDEST_COUNT` | H06, H11 |
| 13 | `FILL_ZERO` | After an abort of a burst, the rest of the packet is zero words | abort, in the RTL header |
| 14 | `RUN_PACKETS` | A burst run is one packet (none or one if aborted); a per-frame run has `FRAME_COUNT` packets (at most, if aborted); a real-time run at most `FRAME_COUNT` | run modes, in the RTL header |
| 15 | `RUN_FRAMES` | A burst run holds `FRAME_COUNT` frames (at most, if aborted) | run modes, in the RTL header |
| 16 | `PKT_OPEN` | A run ends with its packet closed, so the DMA is never left waiting | abort, in the RTL header |
| 17 | `FORCE_STOP` | After `FORCE_ABORT`, `TVALID` is low within 8 clocks and stays low until the next `START` | "stops at once", in the RTL header |
| 18 | `UNDERRUN` | `STATUS` bit 3 (FIR data underrun, "should never be set") is never read as 1 | register map in the RTL header |

*Packet length* means `PAYLOAD_WORDS + 4` in per-frame and real-time runs, and `FRAME_COUNT` times that in a burst. The sizes and modes come from the register writes the snoop saw, not from the design.

Three details of how the rules are applied:

- **Beats and words.** A beat with `TKEEP` `FF` is two words, a beat with `0F` is one. Rules 6 to 11 count words, so a frame header can fall in the high word of a beat when the frame length is odd.
- **Exceptions are explicit.** `FORCE_ABORT` clears the output register on purpose, even when the sink is stalling it. Rules 1, 2, 3 and 5 are suspended for 8 clocks after the write, and rule 17 takes over. After an abort request in a burst, a zero word where a header should be starts the zero fill (rule 13); until then it is a rule 6 failure.
- **Run-level rules** (14, 15, 16) are evaluated when the next `START` is seen and at the end of the simulation, because only then is the run known to be over.

The checks do not look at payload words or the CRC. That stays with the Python model of H11, which recomputes them from the trace.

## Run it

1. Run the checks and the coverage on the real design:

   ```sh
   make -C hw sim-assert
   ```

   It compiles with `iverilog -g2012`, runs the unchanged testbench inside `tb_pfdaq_assert`, prints the violations per rule and the end of the coverage table, and also runs `hw/tb/check_trace.py` on the same trace. It writes `assert_results.txt`, `coverage.txt`, `assert_log.txt` and the trace under `build/sim/`.

2. Read the whole coverage table, and then only the holes:

   ```sh
   cat build/sim/coverage.txt
   grep 'NOT HIT' build/sim/coverage.txt
   ```

3. Check that the numbers do not depend on the checker. The script counts beats, packets and headers again from the trace, in Python:

   ```sh
   python3 experiments/lab-assertions/recount_trace.py build/sim/sim/trace.txt
   ```

4. Check that the run is repeatable, and that the testbench was not disturbed:

   ```sh
   for i in 1 2 3; do make -C hw sim-assert OUT=/tmp/a$i >/dev/null; done
   md5sum /tmp/a*/assert_results.txt /tmp/a*/coverage.txt /tmp/a*/sim/trace.txt
   make -C hw sim-stream OUT=/tmp/s >/dev/null
   md5sum /tmp/s/sim/trace.txt
   ```

5. **Plant bugs and watch the checks fire.** The script makes one scratch copy of the RTL per bug, runs `make -C hw sim-assert ASSERT_RTL=<copy>` on it, and prints what the new checks, the testbench monitors and `check_trace.py` each said. The repository is not touched:

   ```sh
   python3 experiments/lab-assertions/planted_bugs.py /tmp/planted-bugs a_wready b_emit c_force
   python3 experiments/lab-assertions/planted_bugs.py /tmp/planted-bugs     # all eleven
   ```

6. **Close some holes without touching the testbench.** The script adds five runs to a scratch copy of the stream testbench (`ASSERT_TB=<copy>`) and prints the verdicts and the bins that are still at zero:

   ```sh
   python3 experiments/lab-assertions/extra_runs.py /tmp/extra-runs
   ```

7. **Draw your own waveform** of a check firing. The second top level `assert_dump.v` dumps a window; the script turns it into the `signals:` lines of a figure:

   ```sh
   REPO=$PWD; mkdir -p /tmp/h12 && cd /tmp/h12 && mkdir -p sim
   iverilog -g2012 -s tb_pfdaq_assert -s assert_dump -o d.vvp /tmp/planted-bugs/b_emit.v \
     $REPO/hw/tb/tb_pfdaq_stream_src.v $REPO/hw/tb/pfdaq_stream_checks.v $REPO/hw/tb/pfdaq_stream_cover.v \
     $REPO/hw/tb/tb_pfdaq_assert.v $REPO/experiments/lab-assertions/assert_dump.v
   vvp -n d.vvp +T0=4380 +LEN=72
   python3 $REPO/experiments/lab-assertions/vcd_to_wave.py assert.vcd 4380 72 \
     "TREADY=tb_pfdaq_assert.tready|hart" "rule reported=tb_pfdaq_assert.chk.v_now|bad|bus|0=-"
   ```

   Step 5 must have run first, because it writes `/tmp/planted-bugs/b_emit.v`.

## What you should see

Step 1 prints the 18 rules with a count each, and a verdict. On the real design no rule fires:

```
CHECK rule summary: violations per rule over the whole run
CHECK rule  1 VALID_DROP   0
CHECK rule  2 DATA_STABLE  0
...
CHECK rule 18 UNDERRUN     0
CHECK total violations 0
ASSERT RESULT: PASS
COV bins hit 61 of 89
COV runs started 21, packets 81, frame headers 186, accepted beats 15631
COV longest wait of one beat 17 clocks, longest streak of beats on consecutive clocks 756
COV payload words per frame: smallest 1, largest 500
COV frames per run: smallest 3, largest 50
COV real-time RATE: smallest 20, largest 200
trace checker (check_trace.py): RESULT: PASS
```

The 21 runs are the 20 scenarios of H11 and the `FORCE_ABORT` section, which starts a run (scenario 99) into a sink that never becomes ready. The 15,631 beats are the five wait bins added up: 14,406 + 972 + 160 + 83 + 10. Step 3 gives the same totals from the trace alone, so the counters agree with an independent count:

```
accepted beats 15631, packets 81, frame headers 186
packets per TDEST: 0: 56, 1: 13, 2: 8, 3: 4
```

Step 4: the three `md5sum` lines of each file are equal, and the trace of `sim-assert` equals the trace of `sim-stream`. The design is deterministic here because `TREADY` comes from a fixed seed.

### The coverage table

`coverage.txt` has one line per bin: a number, a group, what is counted, and how often it happened. The groups and what the real run reached:

| Group | Bins | What the run reached |
|---|---|---|
| backpressure (beats by clocks waited) | 0 to 4 | waits of 0, 1, 2, 3 to 7 clocks and 8 or more: 14,406, 972, 160, 83 and 10 beats; the longest wait is 17 clocks |
| where the stall hit | 5 to 7 | a stalled first beat 21 times, a stalled middle beat 1,182 times, a stalled `TLAST` beat 22 times |
| spacing | 8 to 12 | 105 frame starts inside a packet with no gap; packets that start 0 clocks after the previous one (5), 1 to 7 (21), 8 to 99 (24) and 100 or more (11) clocks after it |
| frame size, frames per run | 13 to 22 | payloads of 1, 2 to 8, 9 to 64 and 65 to 500 words (1, 6, 10 and 4 runs); runs of 2 to 8 and 9 to 64 frames (10 and 11 runs) |
| 4-byte last beat | 23 to 25 | in a burst 3 times, with `TLAST` per frame 20 times |
| run kind by payload mode | 26 to 34 | 8 of 9 cells |
| destinations | 35 to 42 | packets to `TDEST` 0 to 3: 56, 13, 8 and 4; real-time runs with 2, 3 and 4 destinations |
| real-time | 43 to 53 | `RATE` 20 to 99 and 100 or more; 26 frames with no dropped slot before them and 9 with one; 18 drops for no credit and 10 for a busy source |
| `GAP_CYCLES` | 54 to 57 | 0, 1 to 7 and 400 or more |
| faults | 58 to 66 | 4 of 9 fault and mode pairs |
| abort | 67 to 82 | `CTRL.ABORT` 5 times: 3 in bursts with a packet open, 1 in a per-frame run between packets, 1 in a real-time run with a packet open; one `FORCE_ABORT` |
| run states (probes), frame start | 83 to 88 | PREFILL, RTWAIT, RUN, GAP and FILL entered 4, 40, 79, 24 and 3 times; 16 frame starts in the high word of a beat |

### The holes

The bins were written down before the first run and not changed afterwards to improve the table. 28 of the 89 are at zero. The terminal figure lists them as `coverage.txt` prints them.

![The 28 bins at count 0 as printed by grep NOT HIT: payload above 500 words and the maximum 65535, runs of a single frame, real-time runs with RATE below 20 or one destination, more than one dropped slot before a frame, five of nine fault by payload-mode pairs, an abort before the first beat, while the sink stalls the source, in a per-frame run mid-packet, and while the source pre-fills the FIR or waits for a slot.](../diagrams/lab-h12-coverage-holes.svg)

1. **Sizes.** Payload stops at 500 words (the register takes up to 65535) and no run has fewer than 3 frames or more than 50. The numbers in this callout come from the last lines of `coverage.txt`, not from the bins shown.
2. **Odd length and FIR.** All four real-time runs use even frame lengths (44, 44, 24 and 34 words), so a 4-byte last beat never occurs in real-time mode. No per-frame-`TLAST` run uses the FIR payload.
3. **Real-time shapes.** The smallest `RATE` is 20 and the register accepts 2. No run uses one destination. No frame follows two or more dropped slots, because the busy drops of scenario 14 are one per frame and the credit drops of scenario 13 come after the last frame. A frame that has to carry several drops in its sequence number is therefore never produced.
4. **Faults by mode.** Five of nine pairs never occur: a CRC fault only in counter mode, a sequence skip only in counter mode, a payload flip in counter and PRBS mode only.
5. **Abort timing.** Scenario 21 is meant to abort right after `START` (one `PCLK` cycle later), but by the time the `ABORT` write was on the bus beats had already gone out (bin 74 is 0). No abort arrives while the sink stalls the source or in a per-frame run mid-packet.
6. **Abort and `FORCE_ABORT`.** No abort arrives while the source pre-fills the FIR, waits for a real-time slot, fills with zeros, or is idle. The one `FORCE_ABORT` came with `TVALID` high.

Whether a hole matters is a judgement. A bin such as `GAP_CYCLES` 8 to 399 is a range I chose, and the maximum payload of 65535 words would make a very long run. The table says where to look, not what is wrong.

**Closing some holes.** Step 6 adds five runs to a scratch copy of the testbench: a single frame, a real-time run with one destination and an odd frame length, a real-time run with `RATE` 5, a per-frame run with the FIR payload, and an abort while waiting for a real-time slot. The bins hit go from 61 to 70 of 89 (the nine are 19, 25, 31, 39, 44, 50, 71, 74 and 76), the checks report 0 violations and `check_trace.py` still passes. The record is `sim-assert-extra-runs`. Nineteen bins stay at zero.

### What the planted bugs showed

Step 5 gives one block per bug. Three of them in detail, then the whole set.

**1. `w_ready` refilled one clock late** (the bug of H02, step 3 of its "Run it"):

```
bug a_wready: w_ready is refilled only after the register was seen empty (the H02 experiment)
  new checks     : ASSERT RESULT: FAIL (15487 violations)
  rules that fired: 5 VALID_GAP x15487
  first report   : CHECK FAIL rule 5 VALID_GAP at 2332 ns, scenario 1, info 0x00000002
  testbench monitor lines in the trace (PROTOCOL_ERROR, VALID_GAP, KEEP_ERROR): 15487
  check_trace.py : RESULT: FAIL (first report: trace: VALID_GAP 2332000)
```

The new rule 5 counts 15,487 gaps, the same number as the `VALID_GAP` lines of the testbench monitor and as the number quoted in H02. Only rule 5 fires, because the data and the other rules are untouched: the stream is correct and slow, and this DMA cannot take it.

**2. The generator ignores `w_ready`.** `emit` no longer waits for the output register to be free, so the generator overwrites a beat the sink has not taken:

![Waveform of 72 ns of scenario 2. The sink takes one beat at 4388 ns and then holds TREADY low. As built, TVALID stays high and TDATA holds 00000218. With the bug, TDATA changes to 00050001 at 4396 ns and again every clock, and the checker reports rule 2, DATA_STABLE, from 4404 ns on.](../diagrams/lab-h12-stall-rule.svg)

1. **One beat goes.** At 4388 ns `TVALID` and `TREADY` are both high and the beat transfers.
2. **Stalled.** `TREADY` stays low. In the unmodified design `TVALID` stays high and `TDATA` holds `00000218`. The stall lasts 17 clocks in all, the longest of the run.
3. **The bug.** The generator ignores `w_ready` and `TDATA` changes at 4396 ns, while the sink has not taken the beat.
4. **The rule fires.** The checker compares each clock with the one before. Rule 2 is reported from 4404 ns on, every clock. The testbench monitor reports `PROTOCOL_ERROR` at the same time, 4404 ns (`PROTOCOL_ERROR 4404000` in picoseconds). In all, 1,432 violations: rule 2 1,096 times, rule 3 31 times and rules 6 to 9, 14 and 15 as consequences.

The waveform is two real runs of the same window, one of the unmodified design and one of the scratch copy, taken as step 7 describes.

**3. `FORCE_ABORT` leaves the output register full.** The one line that clears `out_valid` in the `FORCE_ABORT` branch is removed:

```
bug c_force: FORCE_ABORT no longer clears the output register
  new checks     : ASSERT RESULT: FAIL (153 violations)
  rules that fired: 5 VALID_GAP x126, 7 SEQ x1, 17 FORCE_STOP x26
  first report   : CHECK FAIL rule 17 FORCE_STOP at 234740 ns, scenario 99, info 0x00000000
  testbench monitor lines in the trace (PROTOCOL_ERROR, VALID_GAP, KEEP_ERROR): 130
  check_trace.py : RESULT: FAIL (first report: trace: VALID_GAP 234948000)
```

Rule 17 reports it at 234,740 ns, in the scenario that did the `FORCE_ABORT`, which is the cause. The testbench monitor and `check_trace.py` first report the symptom at 234,948 ns, 208 ns (26 clocks) later, in scenario 19, when the stale beat is finally taken and the packet that follows it has a gap. A rule named after the cause is easier to act on than a gap in the next run.

**All eleven.** The full output is `sim-planted-bugs`. In short:

| Planted bug | Rules that fired | Old detectors |
|---|---|---|
| `w_ready = ~out_valid` | 5 | monitor and `check_trace.py` fail |
| generator ignores `w_ready` | 2, 3, 6, 7, 8, 9, 14, 15 | monitor and `check_trace.py` fail |
| `FORCE_ABORT` keeps the output register | 5, 7, 17 | monitor and `check_trace.py` fail, later |
| real-time destination wraps one too early | 12 | `check_trace.py` fails, monitor silent |
| burst starts on destination 1 | 12 | `check_trace.py` fails, monitor silent |
| FIR underrun flag set at the end of the pre-fill | 18 | `check_trace.py` fails, monitor silent |
| 4-byte last beat with `TKEEP` `07` | 4 | monitor and `check_trace.py` fail |
| `TVALID` drops on a stalled beat once in 16 stall clocks | 1, 5, 6, 7, 8, 9 | monitor and `check_trace.py` fail |
| every frame one payload word longer than the header says | 6, 7, 8, 10, 11, 15 | `check_trace.py` fails, monitor silent |
| zero fill of an aborted burst is not zero | 6, 7, 8, 13 | `check_trace.py` fails, monitor silent |
| zero fill never raises `TLAST` on an even remainder | 5, 10, 16 | monitor and `check_trace.py` fail |

Every one of the 18 rules fires in at least one of the eleven runs. Every bug also fails `check_trace.py`. So on these eleven bugs the new checks do not find anything the older detectors miss; what they add is the name of the broken rule, the clock at which it breaks, and, for the `FORCE_ABORT` bug, an earlier and more direct report. One detail of the zero-fill bug: its first report is rule 6, not rule 13, because a non-zero word where the fill should start is taken for a frame header (rule 13 then reports 351 words).

## Do the same in your project

Use this when you have a testbench that already passes and you want to know what it covers and whether its checks can fail. The names are this project's: rename them for yours.

1. **Write the rules down first, in words, with their origin.** Number them and give each a short name. A rule with no origin other than "the RTL does this" checks the design against itself. Take them from the interface standard, from the consumer's documented needs and from your own contract.
2. **Attach to the ports, not to the internals.** The checker in `pfdaq_stream_checks.v` takes the stream signals and the bus. Put the glue in a second top level (`tb_pfdaq_assert.v`) that instantiates the existing testbench and reads its wires by hierarchical reference. Name the root with `-s <new top>`; with the dump module of step 7 there are two roots.
3. **Take the specification of each run from the bus.** `pfdaq_apb_snoop` copies the register values at the `START` write. The checker then knows the expected frame and packet length without asking the design.
4. **Check every clock against the last.** Keep one clock of history (`p_stall`, `p_data`, ...). Evaluate it before you handle the current beat. Treat `x` as a failure by comparing with `===` and `!==`.
5. **Count words, not beats,** if the bus is wider than the frame word. Keep a position in the frame and a position in the packet, and check `TLAST` against the expected packet length.
6. **Name every failure.** Print the rule number and name, the time in nanoseconds and the scenario, but only the first four of each rule; count them all; end with one verdict line. Make the run fail only through that line.
7. **List the exceptions in the code and the page.** Here: the 8 clocks after `FORCE_ABORT`, and the zero fill after an abort. Keep each exception narrow, and give it its own rule (17) so the exempted time is still checked.
8. **Write the coverage bins before the first run,** from the register map and the state machine: the smallest and largest of each size, odd and even lengths, each mode in each run kind, each destination, each way a run can end. Mark in the table the bins that read inside the design (`probe`). Do not move a bin afterwards to improve the table: add stimulus in your own testbench instead.
9. **Plant one bug per rule.** Keep the bugs in a table of "text to find, text to put" and require the text to occur exactly once (`BUGS` in `planted_bugs.py`). A rule that no planted bug makes fire is not proven.
10. **Prove that you left the stimulus alone.** `md5sum` the old testbench's trace with and without the new top. Run the whole thing several times and compare the md5 sums of the results.
11. **Keep the report in a `final` block, and in functions.** In Icarus 12 a `final` block cannot call a task; the reports are `void` functions.

What not to change:

- **Do not let the checker read the design's configuration registers.** It would repeat the design's mistakes.
- **Do not relax a rule to get a pass.** Decide first whether the design or the rule is wrong. While this module was written, every first failure on the real design was a fault in the checker, never in the design (see the table below).
- **Do not edit bins to hide a hole.** Add a run.
- **Do not trust a check that has never failed.**

## Evidence

- **Checks:** 18 rules, 0 violations in 21 runs and 15,631 beats of the real design, and `check_trace.py` passes on the same trace (`sim-assert`). Four runs gave byte-identical results, coverage and trace; the trace equals that of `make -C hw sim-stream`.
- **Coverage:** 61 of 89 bins hit, 28 not (`sim-assert-coverage`). The beat, packet and header totals agree with a Python recount from the trace (`sim-assert-recount`).
- **Holes closed:** five added runs in a scratch copy of the testbench take it to 70 of 89 with no violation (`sim-assert-extra-runs`).
- **Planted bugs:** eleven, every rule fired at least once (`sim-planted-bugs`); the three described above have their own records (`sim-planted-bug-w-ready`, `sim-planted-bug-emit`, `sim-planted-bug-force-abort`).
- **Not on the board.** Nothing here was run on the board. The coverage describes the simulated scenarios, not the 27 functional tests of the board suite, which may reach some of the holes; that was not measured.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| `final procedures cannot enable/call tasks` at compile time | The report is a task. Make it a `void` function (seen while writing this module) |
| The run-level rules fire on the first run after the configuration changed | They were evaluated with the new run's configuration. Keep a copy of the old run's configuration for the end-of-run checks (seen while writing: rules 14 and 15 reported 14 violations on the real design until the copy was added) |
| The longest wait is implausibly high | The wait counter was not cleared when `TVALID` dropped after `FORCE_ABORT`. The first version printed 114 clocks, the corrected one 17 |
| A rule never fires in any planted bug | The rule is vacuous, or an exemption is wider than you meant |
| A planted bug is reported by an unexpected rule first | The first symptom may be another rule's: the zero-fill bug is reported by rule 6 before rule 13 |
| A rule fires on the real design | Check the rule before the design. Look at the first report: rule, time, scenario |
| The trace differs from the one of `sim-stream` | The new top changed the stimulus. Look for a write to one of the testbench's signals |

## Limits of this method

- **Payload and CRC are not checked here.** The checks see the stream protocol and the frame structure only; the Python model of H11 recomputes the values.
- **Same author, same understanding.** The rules were written from the same documents as the RTL. A misreading would be in both.
- **The bins are my choice.** Ranges such as `GAP_CYCLES` 8 to 399 are arbitrary, and a count says a situation happened, not that it happened in an interesting order.
- **Gaps between real-time frames are seen only between two frames of one run.** Slots dropped before the first frame or after the last are not visible on the stream (they are in the `DROPS` register).
- **Some bins read inside the design.** The five probes are listed in `tb_pfdaq_assert.v` and marked `probe` in the table. If the RTL renames those signals, the compile fails.
- **Line and toggle coverage of the RTL are not measured.** The bins are about behaviours, chosen by hand.
- **One simulator,** Icarus Verilog 12.0.

## Trade-offs and going further

- **Procedural checks, not assertion syntax.** The rules are plain code, so they are easy to read and debug in Icarus, but they have no built-in ranges, sequences or implications. A simulator with SystemVerilog assertions would state rules 1 to 3 in one line each. This was not tried here.
- **Hierarchical references tie the top level to the testbench's names.** That is the price of not modifying the testbench. If you own it, give it ports for the signals instead.
- **Coverage by hand-written counters costs a long file** (313 lines for 89 bins). The table is cheap to read; the file is not cheap to keep in step with the register map.
- **Exercise:** pick one bin from the table that is still at zero after step 6 (for example 82, `FORCE_ABORT` with `TVALID` low), add a run for it to a scratch copy of the testbench, and see whether any rule fires.
- **Exercise:** write a rule 19 for a property of your own choice, for example that `TDEST` does not change between two beats of one packet. Then write the planted bug that must make it fire, and check that none of the 18 existing rules fires first.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [H11 Verification with an independent model](H11-independent-model.md) |
| **How to run** | Simulation only: Icarus Verilog and Python, no board |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) |
| **Source** | [`hw/tb/pfdaq_stream_checks.v`](../../hw/tb/pfdaq_stream_checks.v): `pfdaq_stream_checks`, `pfdaq_apb_snoop`<br>[`hw/tb/pfdaq_stream_cover.v`](../../hw/tb/pfdaq_stream_cover.v): `pfdaq_stream_cover`<br>[`hw/tb/tb_pfdaq_assert.v`](../../hw/tb/tb_pfdaq_assert.v)<br>[`hw/Makefile`](../../hw/Makefile)<br>[`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `pfdaq_stream_src`, `w_ready`, `out_valid`, `fabort_pulse_a`<br>[`experiments/lab-assertions/planted_bugs.py`](../../experiments/lab-assertions/planted_bugs.py): `BUGS`<br>[`experiments/lab-assertions/extra_runs.py`](../../experiments/lab-assertions/extra_runs.py)<br>[`experiments/lab-assertions/recount_trace.py`](../../experiments/lab-assertions/recount_trace.py)<br>[`experiments/lab-assertions/assert_dump.v`](../../experiments/lab-assertions/assert_dump.v)<br>[`experiments/lab-assertions/vcd_to_wave.py`](../../experiments/lab-assertions/vcd_to_wave.py) |
| **Evidence** | [Stream source simulation with 18 protocol checks: no violation in 21 runs, the existing trace checker also passes](../../evidence/v0.5.1/sim/assert-results.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Functional coverage of the stream source: 61 of 89 bins hit, 28 never reached](../../evidence/v0.5.1/sim/coverage.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Beats, packets and frame headers of the same run recounted from the trace with a Python script](../../evidence/v0.5.1/sim/assert-recount.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Coverage after five added runs in a scratch copy of the stream testbench: 70 of 89 bins, checks still pass](../../evidence/v0.5.1/sim/coverage-extra-runs.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Eleven deliberate bugs, one at a time in a scratch copy of the RTL: what each detector reported](../../evidence/v0.5.1/sim/planted-bugs.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Planted bug: w_ready = ~out_valid. The checks report VALID_GAP 15,487 times](../../evidence/v0.5.1/sim/planted-bug-w-ready.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Planted bug: the generator ignores w_ready. TDATA changes under a stalled sink and rule 2 fires](../../evidence/v0.5.1/sim/planted-bug-emit.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Planted bug: FORCE_ABORT leaves the output register full. Rule 17 fires at the cause, 208 ns before the old monitor](../../evidence/v0.5.1/sim/planted-bug-force-abort.txt) (sim, fabric v0.5.1, 2026-11-15) |
| **Related** | [H11 Verification with an independent model](H11-independent-model.md)<br>[H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md)<br>[H06 Real-time sources: slots, credits and honest drops](H06-real-time-source.md)<br>[H07 Measuring software from hardware: the latency unit](H07-latency-unit.md) |
| **Status** | validated |
<!-- nav:end -->
