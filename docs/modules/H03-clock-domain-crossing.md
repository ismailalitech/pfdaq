---
id: H03
title: "Crossing clock domains: toggles, snapshots and constraints"
track: fpga
level: 3
summary: Moving pulses and multi-bit status between 50 MHz and 125 MHz, and telling the timing tool exactly which paths are intentional.
needs: [H01]
repro: sim
status: validated
hardware: [pfdaq-stream-src, fabric-clocks]
sources:
  - hw/rtl/pfdaq_stream_src.v:start_sync_a,snapv_pulse_p,snap_is_done,end_run,busy_sync_p,cfg_coef,credit_val_p
  - hw/libero/overlay/pfdaq_cdc.sdc
  - hw/tb/tb_pfdaq_stream_src.v:snapshot
evidence: [sim-stream, timing, timing-without-cdc]
related: [H01, H02, H10, K07]
---

# Crossing clock domains: toggles, snapshots and constraints

After this module you can list every signal that crosses between the two clocks of the stream source, say for each one why it is safe, read the constraint file that tells the timing tool about the crossings, and say what a simulator can and cannot tell you about them.

## Before you start

- **Level 3.** Read [An APB register block that software can trust](H01-apb-register-block.md) first: it introduces the register kinds that this page follows across the clock boundary.
- **Tools:** Icarus Verilog 12 and Python 3 for the simulation. Reading the timing evidence needs no tool.
- **Time:** the simulation runs in under a minute; the SDC takes a short read.

## The idea

A **clock domain** is the set of flip-flops driven by one clock. When a signal made in one domain is sampled by a flip-flop in another, the sampling edge can fall inside the narrow window in which the input is changing. The flip-flop then goes *metastable*: its output sits between 0 and 1 for an unpredictable time and may settle either way. Two rules follow.

1. **Never let a metastable value reach logic.** Pass a one-bit signal through two flip-flops in the receiving domain. The first may go metastable; by the time the second samples it, the chance that it still is has become negligible. This is the *two-flip-flop synchroniser*.
2. **Never sample a multi-bit value that is changing.** Each bit settles on its own, so a bus can be received as a mixture of its old and new values. A bus must be held still while it is sampled, and something one bit wide must say when.

Everything in this design is an application of those two rules. There are only three mechanisms:

| Mechanism | Used for | Why it is safe |
|---|---|---|
| **Toggle through a synchroniser** | one-shot events: START, SNAPSHOT, ABORT, FORCE_ABORT, a credit write; the "snapshot is ready" event | one bit, passed through two flip-flops; an event is a *change* of level, so it cannot be lost or doubled |
| **Level through a synchroniser** | the `busy` flag | one bit; only the current value matters |
| **Quasi-static value, sampled on a toggle** | configuration (PCLK → ACLK), snapshot registers (ACLK → PCLK), the credit amount | the bus does not change between the moment the toggle is sent and the moment it is used |

## On this board

The register block runs on **PCLK**, the 50 MHz clock of the APB bus (FIC3). The stream generator runs on **ACLK**, the 125 MHz clock of the AXI side (FIC0). Both come from the same PLL, so they are related, but their edges do not line up in any way a timing tool can use. That matters for the constraints below.

### Inventory of the crossings

Every crossing, from the RTL in `hw/rtl/pfdaq_stream_src.v`:

| Signal | Direction | Mechanism | Receiving logic |
|---|---|---|---|
| `start_tgl_p`, `snap_tgl_p`, `abort_tgl_p`, `fabort_tgl_p`, `credit_tgl_p` | PCLK → ACLK | toggle, three-flip-flop chain | `*_sync_a`, pulse = last two flip-flops differ |
| configuration: `payload_words`, `frame_count`, `gap_cycles`, `fault`, `mode_*`, `rate`, `fir_step`, `coef_p`, `tlast_per_frame` | PCLK → ACLK | quasi-static, copied on the START pulse | the `cfg_*` registers |
| `credit_val_p` | PCLK → ACLK | quasi-static, read on the credit pulse | the credit counter |
| `busy_a` | ACLK → PCLK | level, two-flip-flop synchroniser | `busy_sync_p`, bit 0 of STATUS |
| `snap_tgl_a` | ACLK → PCLK | toggle, three-flip-flop chain | `snapv_sync_p`, pulse = last two differ |
| `snap_frames`, `snap_words`, `snap_stall`, `snap_time`, `snap_seq`, `snap_drops`, `snap_credits`, `snap_flags` | ACLK → PCLK | quasi-static, copied on the snapshot pulse | the `snap_*_p` bank |
| `snap_is_done` | ACLK → PCLK | quasi-static, read on the snapshot pulse | `done_p` |

![The clock-domain crossings of the stream source: PCLK on the left, ACLK on the right. Configuration registers cross as values held still and sampled when the START pulse arrives. Events cross as toggles through a three flip-flop chain that turns a change of level into a one-clock pulse. In the other direction the snapshot registers are held still while a toggle crosses to PCLK, which then copies them into the snap_p bank in one clock.](../diagrams/lab-h03-crossings.svg)

1. **Configuration.** The PCLK registers are held still. The ACLK side samples them into `cfg_*` when the START pulse arrives.
2. **The toggle.** A write of 1 to a CTRL bit flips a toggle flip-flop in PCLK, and the toggle goes into a three flip-flop chain in ACLK.
3. **The pulse.** The chain turns the change of level into a one-clock pulse, `[2] xor [1]`, which loads the `cfg_*` registers.
4. **The return trip.** The ACLK side writes `snap_*`, holds them still and flips `snap_tgl_a`.
5. **The bank.** A three flip-flop chain in PCLK turns that toggle into a pulse, and on it the PCLK side copies the whole bank `snap_*_p`, together with `done_p`, in one clock.

### Toggles

A pulse cannot be passed directly: a one-cycle pulse in a 50 MHz domain would be sampled by a 125 MHz clock for two or three cycles, a one-cycle pulse in the fast domain could be missed entirely by the slow one. A **toggle** turns the event into a level change:

```verilog
// PCLK side: a write of 1 to CTRL.START flips the toggle
if (PWDATA[0]) begin
    start_tgl_p <= ~start_tgl_p;
    ...
// ACLK side: shift the toggle through the chain, an event is a difference
start_sync_a  <= {start_sync_a[1:0], start_tgl_p};
wire start_pulse_a = start_sync_a[2] ^ start_sync_a[1];
```

`start_sync_a[0]` and `[1]` are the two synchronising flip-flops. `[2]` exists only to remember the previous value, so that `[2] ^ [1]` is high for exactly one ACLK cycle when the toggle has arrived. Because the information is in the *change*, it makes no difference how many clock cycles the level stays stable, as long as it stays long enough to be sampled once. One event per toggle: two writes of START in quick succession produce two toggles, and the stream side sees two events, provided they are far enough apart for the slower side to see each change. For START, the stream logic ignores the second event while a run is busy.

This is the START write of scenario 1 in the simulation, from the end of the APB write to the start of the run:

![Waveform of the stream simulation: a START write in PCLK reaches the ACLK side. The write ends at a PCLK edge, where start_tgl_p flips. In ACLK the three flip-flop chain start_sync_a shifts the new level in over three clock edges and the pulse is high for one clock when bits 2 and 1 differ. At the next ACLK edge the run starts and the configuration is copied: cfg_payload changes from 16 to 8.](../diagrams/lab-h03-start-crossing.svg)

1. **The START write.** The access phase of the APB write to CTRL is in PCLK; `apb_write` is high until the PCLK edge at 2290 ns.
2. **The toggle flips.** At that edge `start_tgl_p` changes. The event is the change of level, not a short pulse.
3. **The chain.** ACLK shifts the new level in: `001` at 2292 ns, `011` at 2300 ns and `111` at 2308 ns.
4. **One pulse.** `start_pulse_a` is bit 2 xor bit 1: high for exactly one ACLK clock, whatever the level does after.
5. **Copied at once.** At the next ACLK edge `run` leaves `R_IDLE` and `cfg_payload` takes its value, 8, the one software wrote earlier. It was 16, the reset value.

The same structure carries the event in the other direction. `snap_tgl_a` flips when the stream side has written a new set of snapshot values; `snapv_sync_p` and `snapv_pulse_p` bring it into PCLK.

### Quasi-static values and the order of events

A multi-bit value is never passed through synchronisers. It is kept still, and a toggle says when it is safe to look.

**Configuration, PCLK → ACLK.** Software writes the configuration registers earlier, then writes START. The START toggle takes a few ACLK cycles to arrive. When it does, the stream logic copies all configuration registers into its `cfg_*` registers in a single cycle (see the `R_IDLE` branch of the run state machine). The configuration registers did not change in the meantime, because software does not touch them between writing START and the end of the run. This is the hardware reason for the rule "do not write configuration while busy" in module H01. It is a rule that software has to keep: nothing in the hardware prevents a write while a run is in progress.

`fault` is the one configuration register the stream side also reads *directly*, at the START pulse, for the case where a sequence-number skip applies to frame 0 of the run. The timing tool sees this as a path from PCLK registers into ACLK registers, which will matter below.

**Snapshot, ACLK → PCLK.** On a SNAPSHOT event, or when a run ends, the stream side writes its counters into the `snap_*` registers and flips `snap_tgl_a`. The toggle reaches PCLK after a delay of two to three PCLK cycles; when the pulse appears, the PCLK side copies the whole bank into `snap_*_p` in one cycle. The `snap_*` registers were written *before* the toggle flipped and are not written again until the next snapshot, so they are steady while the PCLK flip-flops sample them. APB reads see only the `_p` copies, which are PCLK flip-flops, so a read never touches a signal from another clock.

Two limits are worth saying plainly. First, the scheme relies on the `snap_*` registers staying unchanged for the few PCLK cycles between the toggle and the copy. The design does not enforce that: two snapshot events within that window, for instance a SNAPSHOT write that lands just as a run ends, could change the registers under a copy in progress. The testbench waits six PCLK cycles after each SNAPSHOT (`snapshot` in `hw/tb/tb_pfdaq_stream_src.v`), and the Linux capture tool sleeps 200 µs after the write, with a comment that a few fabric cycles are enough. Neither checks a "ready" bit, because the block does not have one. That the window cannot be violated in practice was not investigated. Second, simulation in the following experiments cannot see such a violation, for reasons in the section "What simulation cannot show".

**Credit amount.** A write to CREDIT stores the amount in `credit_val_p` and flips `credit_tgl_p` in the same cycle. The ACLK side reads the amount when the pulse arrives. A second CREDIT write that arrives before the first pulse has crossed would overwrite the amount the first pulse is about to read. The RTL does not guard against that, and software issues the writes one at a time. This is an observation from reading the RTL, not a failure that was seen.

### The `done` flag travels with the data

The STATUS register has a `done` bit, which software polls to learn that a run has finished. The obvious design gives `done` its own toggle. That design was built and failed in simulation: the notes record that with two separate toggles the flag was never seen set after the snapshot change (the exact interleaving was not written down, and the raw simulation log is not published). The design that works carries the flag on the same crossing as the data:

```verilog
task end_run;
    ...
    snap_is_done <= 1'b1;        // this snapshot is the end of a run
    snap_tgl_a   <= ~snap_tgl_a; // the one toggle that carries the whole bank
...
// PCLK side, on the one snapshot pulse:
if (snap_is_done)
    done_p <= 1'b1;
```

`done_p` is therefore set in the same PCLK cycle in which the final counters appear in the `_p` bank. When software sees `done`, the counters it then reads are the final ones. A SNAPSHOT command clears `snap_is_done` and flips the toggle too, so a mid-run snapshot copies the bank but cannot set `done`. A write of START clears `done_p` on the PCLK side at once.

The end of the first run in the simulation, from ACLK to PCLK:

![Waveform of the stream simulation, the end of the first run from ACLK to PCLK: at one ACLK edge busy_a falls, snap_tgl_a flips and the snapshot registers hold their final values; in PCLK the chain snapv_sync_p shifts the new level in, snapv_pulse_p is high for one PCLK clock, and at the next PCLK edge the bank snap_frames_p takes 5 and done_p is set with it. The busy bit of STATUS falls one PCLK clock earlier than done_p rises.](../diagrams/lab-h03-snapshot-crossing.svg)

1. **The run ends.** In ACLK, at 2548 ns, `busy_a` falls, `snap_tgl_a` flips, and the final counters are written and then held.
2. **The chain.** In PCLK the toggle is shifted in: `110` at 2550 ns, `100` at 2570 ns, `000` at 2590 ns.
3. **Busy falls first.** The `busy` bit of STATUS falls at 2570 ns, 20 ns before `done`. In this run a STATUS read in that gap returns `0x0`.
4. **One pulse.** `snapv_pulse_p` is high for one PCLK clock. The `snap_*` registers have not changed since 2548 ns.
5. **Data and done together.** At 2590 ns the bank `snap_frames_p` takes 5 and `done_p` rises in the same edge.

### Why the toggles were not the first idea

The first fabric version, v0.2.0, let the APB read multiplexer read the stream-side counters directly. That is a path from ACLK flip-flops through a multiplexer to PCLK flip-flops, and in the first build it failed timing by 0.707 ns. The fix in v0.2.1 was the `snap_*_p` bank described above, and with it the rule that the APB read path contains PCLK flip-flops only. The failure and the fix are recorded in the project's design decisions (D16) and in the failures table of the validation plan; the raw build report of that run is recorded in the project's notes and is not published.

## The constraint file

The timing tool, SmartTime, checks every path between two flip-flops against the period of the capturing clock. For paths between the two domains it has nothing sensible to check. The file's own comment states the problem: both clocks come from the same PLL, so without help SmartTime times every crossing as if it were synchronous. Most crossings would then report violations, which are not real problems, and any *real* problem would be lost among them.

The constraint file `hw/libero/overlay/pfdaq_cdc.sdc` declares the crossings that are intentional as **false paths**, paths the tool must not time. It has five commands:

| # | Path excluded | Why it is intentional |
|---|---|---|
| 1 | from `snap_*` to `snap_*_p*` | the snapshot bank, copied only after the synchronised toggle |
| 2 | from `snap_is_done*` to `done_p*` | the done flag, read on the same pulse |
| 3 | from the PCLK configuration registers, `fault`, `coef_p`, `credit_val_p` and the rest, to the clock named in the file (the PLL output on the stream side) | sampled on the START or credit toggle; the bus is held still |
| 4 | from `*_tgl_p*` to `*_sync_a*` | the toggle going into the first flip-flop of each synchroniser |
| 5 | from `busy_a*` and `snap_tgl_a*` to `*_sync_p*` | the same, in the other direction |

Commands 4 and 5 look strange because they are written by *register name* with wildcards. The register names for bus signals contain brackets, which this constraint language cannot match; three builds were lost to that, and the form that works names each path by the registers at its two ends. That story is in case study K07. Naming the endpoints also gives a neat check: only the *first* flip-flop of each chain is fed from the other domain, so `*_sync_a*` matched from `*_tgl_p*` selects exactly the synchroniser inputs and nothing else.

Command 3 is different in kind. It runs from a *group of cells* into a *clock*, which means "every path from these registers into any flip-flop on that clock". It is broader than the `cfg_*` copies that it is meant for. If someone later reads `rate` in the stream logic at a time other than START, the tool would stay silent, and the design would be using an unprotected value. The safety of command 3 therefore rests on the RTL discipline above (configuration is used only through the `cfg_*` copies, with the one documented exception of `fault`), not on anything the tool verifies. That is the general cost of a false path: it removes the check, and the author takes over the responsibility.

## Run it

1. The simulation, from the repository root:

   ```sh
   make -C hw sim-stream
   ```

   It passes with 20 scenarios. Several of them exercise the crossings in a way that is easy to see: every `P` and `R` line of the trace in module H01 is a snapshot read, and every run ends with a toggle that carries `done`.

2. Read `hw/libero/overlay/pfdaq_cdc.sdc` next to the inventory table. Check each of the five commands against a row of the table.

3. Read the timing record for the real design, [Place and route result](../../evidence/v0.5.1/build/timing-and-utilisation.txt). It shows that the design meets timing in all corners with the constraint file in use, with the build's own line `Timing constraints have been met.` and a worst slack of 1.342 ns.

4. Read the other record, [timing analysis without the clock-crossing constraints](../../evidence/v0.5.1/build/timing-without-cdc-constraints.txt). This is the same placed design analysed again with `pfdaq_cdc.sdc` removed, using `hw/libero/sta_without_cdc.tcl`. Go to the end: the tool reports `Warning: Timing constraints have not been met.`

5. **Two experiments in a scratch copy of `hw/`**, described below.

## What you should see

**Timing with and without the file.** With the constraint file, the design meets setup and hold in all three corner combinations. Without it, the tool reports violations. The published record is the build's console output, which says that timing is not met but does not list the paths. The paths were read from the tool's violation reports for this page; those reports are recorded in the project's notes and are not published. Each report lists only its 20 worst paths, so what follows is what those 40 paths contain, not a count of everything that violated.

| Report | Paths listed | What they are |
|---|---|---|
| Setup (max delay) | 20, worst slack −1.558 ns | all 20 run from the `fault` register to the sequence counter `seq`, which is the direct read of `fault` at START described above |
| Hold (min delay) | 20, worst slack −0.841 ns | all 20 are snapshot registers into their `_p` copies, or `snap_tgl_a` into the first flip-flop of `snapv_sync_p`: `snap_words`, `snap_stall`, `snap_credits`, `snap_time`, `snap_drops`, `snap_flags`, `snap_frames`, `snap_seq` |

Every listed path is wholly inside the stream source and every one is a crossing from the inventory table. There is no path from, say, the DMA controller or the APB bus bridge. That supports the statement "without the file, the violations are the intended crossings", but it does not *prove* "and nothing else", because of the 20-path limit of each report. The statement in the architecture document that this was checked is, strictly, a check of those 40 paths.

**The always-copy experiment.** In a scratch copy, the line `if (snapv_pulse_p) begin` was replaced by `if (1'b1) begin`, so that the PCLK bank copies the stream-side registers on every PCLK cycle instead of waiting for the toggle. The run ended with `RESULT: PASS`. Simulation cannot tell the difference, and the next section says why.

**The decoupled-done experiment.** In another scratch copy, `done_p` was no longer set together with the snapshot but when `busy_sync_p` showed `busy` falling. The data scenarios still passed. The run failed in one check only:

```
RESULT: FAIL
   FORCE_ABORT: STATUS before/after ['5', '6'], expected busy then idle+aborted
```

After a forced abort the flag now rose, because `busy` fell, while the original design leaves `done` clear (a forced abort does not end a run through the snapshot path). So this experiment did show that the flag's meaning depends on the path it travels, but it did *not* reproduce the failure in the project's notes, where two toggles raced and `done` was never set; that exact interleaving was not recreated.

## What simulation cannot show

An RTL simulator models each flip-flop as taking its input at a clock edge. It has no setup and hold window, so it has no metastability. Whatever a flip-flop samples, it samples as a clean 0 or 1, and when the simulator drives all bits of the snapshot bank at one ACLK edge, a PCLK flip-flop sees either all old bits or all new bits, never a mixture. That is why the always-copy experiment passes.

On the chip, the bits of the bank travel on different wires with different delays. A PCLK flip-flop that samples during the change can see a mixed word. Metastability and mixed samples are physical effects that only a timing analysis, a gate-level simulation with delays, or a hardware test can reveal. This design has the first, and it is deliberately switched off for these paths, which is the point of the constraint file: the tool is told not to look because the *protocol* (toggle first, copy later, bank held still) is what makes the paths safe. The always-copy experiment, which would pass in simulation, would be correct only by luck on the chip, and the constraint file would hide the fault.

What simulation does verify is the protocol itself: that the right values appear in the right order, that `done` is set together with the final counters, that no event is lost or doubled, and that backpressure and aborts leave the flag consistent. Those are the properties the 20 scenarios and the Python model check.

## Do the same in your project

Use this when a register block in one clock domain controls logic in another and reads its counters back. The names are this project's: rename them for yours.

1. **List every crossing first.** Write a table like the inventory above: signal, direction, mechanism, receiving register. Every signal that goes from one clock to the other must be in it, and every row must be one of three kinds: a toggle, a level through two flip-flops, or a value held still and sampled on a toggle.
2. **Turn each event into a toggle.** In the source domain, flip a flip-flop on the event (`start_tgl_p`). In the destination domain shift it into a chain, `x_sync_a <= {x_sync_a[1:0], x_tgl_p};`, and take `wire x_pulse_a = x_sync_a[2] ^ x_sync_a[1];` as the one-clock pulse.
3. **Never pass a bus through a synchroniser.** Hold the value still and let a toggle say when to look: copy the configuration into `cfg_*` on the START pulse, and copy the counters into the `snap_*_p` bank on the snapshot pulse (`snapv_pulse_p`).
4. **Pass a flag that must agree with data on the same toggle.** `snap_is_done` is written before `snap_tgl_a` flips and `done_p` is set on the same pulse that copies the bank. A separate toggle for `done` failed in simulation in this project.
5. **Pass a plain level through two flip-flops.** `busy_sync_p <= {busy_sync_p[0], busy_a};` is enough for a flag whose current value is all that matters.
6. **Keep the bus read path in the bus domain.** Reads see only the `_p` registers, which are bus-domain flip-flops. Reading the other domain's counters directly failed timing by 0.707 ns in this project's first build (fabric v0.2.0).
7. **Declare the crossings to the timing tool, one by one.** Copy the five commands of `hw/libero/overlay/pfdaq_cdc.sdc` and rename the registers: the snapshot bank, the done flag, the configuration registers (into the clock), the PCLK toggles into the first stage of the `*_sync_a` chains, and `busy_a` and `snap_tgl_a` into the first stage of the `*_sync_p` chains. Name paths by the registers at their ends; bracketed names cannot be matched (case study K07).
8. **Check the protocol in simulation, the physics in timing.** `make -C hw sim-stream` checks that the right values appear in the right order, with `done` together with the final counters. It cannot show metastability or a mixed word. For that, run the placed design's timing analysis with and without your constraint file, as `hw/libero/sta_without_cdc.tcl` does: with it timing is met, and without it the violating paths should be your crossings (each report lists only its worst 20 paths, so read them as a check, not a proof).

What not to change:

- **Do not write configuration while a run is busy.** Nothing in the hardware stops you; the quasi-static rule is the whole protection.
- **Keep snapshot events apart.** The testbench waits six `PCLK` cycles after each SNAPSHOT; do not start a second snapshot inside the few cycles the copy needs.
- **Write CREDIT one at a time.** A second write before the first pulse has crossed overwrites the amount.
- **Do not give `done` its own toggle.** Carry it on the data's toggle.
- **Do not widen a false path** to a clock group to make a report quieter. It would also hide an unprotected crossing.
- **Do not trust a passing simulation for the crossings.** A bank that is copied on every clock also passes simulation.

## Evidence

- **Simulation:** 20 scenarios, including the `P` and `R` snapshot reads of every scenario, checked against the model's own count of frames, words, drops and sequence numbers. Record: [Stream source simulation](../../evidence/v0.5.1/sim/stream.txt).
- **Build:** timing met in all corners with the constraint file, 64 of 64 I/Os placed, 26.0 % of the fabric. Record: [Place and route result](../../evidence/v0.5.1/build/timing-and-utilisation.txt).
- **Build, without the constraint file:** the tool reports violations. Record: [timing analysis without the clock-crossing constraints](../../evidence/v0.5.1/build/timing-without-cdc-constraints.txt). The path-level reading above is from unpublished reports.
- **Board:** the supervisor's sequence cross-check reads a snapshot with every status report. It recorded no mismatch in the 27-test suite ("supervisor ok" in every supervised row) or in the 10-minute soak on fabric v0.3.0, which has the same snapshot scheme. That is an absence of observed failures on one board, not a proof that the crossing is safe.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| Timing report full of violations between the two clocks | The false paths are missing, not applied to the right tool, or the file list replaced rather than extended (case study K08) |
| Counter reads that are *occasionally* off by a large amount | A multi-bit value sampled while it changed: a snapshot copy without a toggle, or two snapshot events too close together |
| `done` set but counters from the previous run | `done` and the data cross separately; carry the flag on the same toggle |
| A pulse is lost or arrives twice | The event is passed as a level of one cycle rather than as a toggle, or the toggle goes through fewer than two flip-flops |
| Everything passes in simulation, a rare failure on hardware | Metastability or a mixed word: simulation has neither |
| A new register is added and works, but the timing report goes quiet about it | A broad false path now covers it; check that its use matches the protocol |

## Trade-offs and going further

- **Toggles against handshakes.** A toggle is one wire and no acknowledgement: cheap, with a latency of a few cycles. It cannot tell the sender that the event arrived, and two events must be far enough apart for the slower side to see both. A request-acknowledge handshake gives that guarantee at the price of a return path and a longer protocol. At the rates here, a toggle is enough.
- **Snapshot bank against a FIFO or Gray-coded counters.** A bank copied on a toggle suits values read rarely. A value read continuously, like a fill level, would use a Gray-coded counter, in which only one bit changes per step, or an asynchronous FIFO. The bank here costs one extra flip-flop per snapshot bit.
- **False paths against `set_clock_groups`.** A clock-group exception states "these two clocks are asynchronous" and silences every path between them at once. That is shorter, but it would also hide an accidental, unprotected crossing anywhere. The per-path form used here is more work and safer. Whether even that is the right choice is a judgement; command 3 shows its limit.
- **A ready bit.** The snapshot has no "bank valid" flag, so software waits a fixed time. A bit that went high when the copy has happened would remove that wait and remove the unenforced window noted above.
- **Exercise:** change the PCLK or ACLK period in `hw/tb/tb_pfdaq_stream_src.v` (the two `always #` lines) so that the clocks differ by a different ratio, rerun, and predict whether the number of PCLK cycles needed after SNAPSHOT changes.
- **Exercise:** add a check to the Python model that a snapshot read taken sooner than three PCLK cycles after the SNAPSHOT write is either the old bank or the new one but never a mixture. What would the testbench have to do to make that interesting?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [H01 An APB register block that software can trust](H01-apb-register-block.md) |
| **How to run** | Simulation only: Icarus Verilog and Python, no board |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [Fabric clocks and resets](../system/README.md#fabric-clocks) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `start_sync_a`, `snapv_pulse_p`, `snap_is_done`, `end_run`, `busy_sync_p`, `cfg_coef`, `credit_val_p`<br>[`hw/libero/overlay/pfdaq_cdc.sdc`](../../hw/libero/overlay/pfdaq_cdc.sdc)<br>[`hw/tb/tb_pfdaq_stream_src.v`](../../hw/tb/tb_pfdaq_stream_src.v): `snapshot` |
| **Evidence** | [Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Timing analysis without the clock-crossing constraints (the tool's console log; the 40 worst violating paths it lists are the intended crossings)](../../evidence/v0.5.1/build/timing-without-cdc-constraints.txt) (build, fabric v0.5.1, 2026-11-15) |
| **Related** | [H01 An APB register block that software can trust](H01-apb-register-block.md)<br>[H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md)<br>[H10 Timing closure, and the build that lost its pins](H10-timing-closure.md)<br>[K07 Three builds lost to brackets in a constraint file](../case-studies/K07-brackets-in-constraints.md) |
| **Status** | validated |
<!-- nav:end -->
