---
id: F04
title: "Clocks and resets: one PLL, two bus clocks"
track: fundamentals
level: 2
summary: Where the 50 MHz and 125 MHz clocks come from and which logic lives in which domain.
needs: [F01]
repro: doc
status: validated
hardware: [fabric-clocks, fic0, fic3, pfdaq-core]
sources:
  - hw/libero/overlay/PFDAQ.tcl:CLOCKS_AND_RESETS_0,FIC_3_CLK,RESETN_FIC_3_CLK
  - hw/libero/overlay/pfdaq_cdc.sdc
  - hw/rtl/pfdaq_stream_src.v:ACLK,PCLK,RSTN,PRESETN
  - hw/rtl/pfdaq_core.v:pfdaq_core
evidence: [timing, timing-without-cdc]
related: [F01, H01, H03, H07, K07]
---

# Clocks and resets: one PLL, two bus clocks

After this module you can say which clock every block of the pfdaq fabric runs on, where those clocks come from, how the fabric gets its resets, and why a design with two related clocks still needs constraints that say which crossings are intentional.

## Before you start

- **Level 2.** Module F01 explains the two halves of the chip and the interfaces between them. You should know what a clock period and a flip-flop are.
- **Tools:** none. This is a reading page; the facts come from the project's own Libero script and RTL, from the Discovery Kit reference design's clock block, and from the recorded build.

## The idea

Every register in a digital design changes on an edge of some clock. A set of registers that share a clock is a **clock domain**. Inside one domain, the timing tool can check that a signal gets from one register to the next in time. Between two domains it can only do that if it knows how the two clocks relate.

A **PLL** makes new clocks from a reference by multiplying and dividing its frequency. Clocks made by one PLL have a known relationship, which is what lets the tool time paths between them. A **reset** is the other half of the same story: a register that comes out of reset at a random moment relative to its clock can start in a wrong state, so a design usually has one reset per clock domain, released in step with that clock.

## On this board

### Where the clocks come from

The clocks of the fabric are made by one clock-conditioning circuit (a `PF_CCC`, instance name `CCC_FIC_x_CLK`) inside the reference design's clock-and-reset block. Its reference input is a 50 MHz clock that arrives on a port called `REF_CLK_50MHz` and passes through a clock buffer macro (`CLKINT`). Its four outputs are the clocks of the four fabric interfaces:

| PLL output | Clock net | Frequency | Goes to |
|---|---|---|---|
| OUT0 | `FIC_0_CLK` | 125 MHz | the fabric side of FIC0, and the MSS's `FIC_0_ACLK` input |
| OUT1 | `FIC_1_CLK` | 125 MHz | the MSS's `FIC_1_ACLK` input; nothing else in this design |
| OUT2 | `FIC_2_CLK` | 125 MHz | the MSS's `FIC_2_ACLK` input; nothing else in this design |
| OUT3 | `FIC_3_CLK` | 50 MHz | the fabric side of FIC3, and the MSS's `FIC_3_PCLK` input |

The frequencies are the output settings in the reference design's configuration script (v2026.04). Two things follow from the table. The clocks of the fabric interfaces are **supplied by the fabric to the MSS**, not the other way round: the MSS wrapper has them as inputs. And the project only uses two of the four: 125 MHz for the stream and the DMA, and 50 MHz for the register bus.

The project's own names for them are the port names in its RTL:

| Name in the RTL | Net | Domain |
|---|---|---|
| `ACLK`, reset `RSTN` | `FIC_0_CLK` (OUT0), reset `RESETN_FIC_0_CLK` | 125 MHz: the stream |
| `PCLK`, reset `PRESETN` | `FIC_3_CLK` (OUT3), reset `RESETN_FIC_3_CLK` | 50 MHz: the registers |

The multiplier and divider values of the PLL are in the reference design's script, which this repository does not copy (the build fetches it); this page does not reproduce them and does not need them.

### Where the resets come from

The reference design gives each of the four clocks its own reset generator (a `CORERESET` core), so there are four resets, `RESETN_FIC_0_CLK` to `RESETN_FIC_3_CLK`, each tied to its clock. Their inputs, as wired in the reference design's script:

| Input | What it is |
|---|---|
| PLL lock | the clock is not trusted until the PLL has locked |
| `INIT_DONE`, `FPGA_POR_N` | the device's initialisation monitor: the fabric has finished powering up |
| a common reset input | the AND of the external reset, `MSS_DLL_LOCKS` and `MSS_TO_FABRIC_RESETN`: held low until the MSS's own DLLs have locked and the MSS releases the fabric |

The PLL's power-down input is the AND of the four generators' power-down outputs. The result is that **nothing in the fabric leaves reset until the PLL is locked, the device is initialised and the MSS is ready**, and each domain is released on its own clock.

The pfdaq RTL uses its resets asynchronously: `always @(posedge PCLK or negedge PRESETN)` in the register block, `always @(posedge ACLK or negedge RSTN)` in the generator.

### Who lives where

| Block | Clock | Reset |
|---|---|---|
| APB register block of the stream source (`u_src`, APB side) | `PCLK`, 50 MHz | `PRESETN` |
| Latency channels 0 and 1 (`pfdaq_latency_ch`) | `PCLK` | `PRESETN` |
| Stream generator, frame logic, CRC, FIR (`u_src`, stream side) | `ACLK`, 125 MHz | `RSTN` |
| DMA controller and its AXI initiator (FIC0 block) | `ACLK` | the FIC0 reset |
| Mailbox, GPIO and the other reference-design peripherals on FIC3 | `PCLK` | the FIC3 reset |

The project script wires the register clock and reset of the block that holds the stream source from the clock block. `ACLK` and its reset come from the FIC0 clock and reset in the reference design's own wiring.

```tcl
sd_connect_pins -sd_name {MPFS_DISCOVERY_KIT} -pin_names {"CLOCKS_AND_RESETS_0:RESETN_FIC_3_CLK" "FIC_0_PERIPHERALS_0:PRESETN"}
sd_connect_pins -sd_name {MPFS_DISCOVERY_KIT} -pin_names {"CLOCKS_AND_RESETS_0:FIC_3_CLK" "FIC_0_PERIPHERALS_0:PCLK"}
```

Notice the block's name: `FIC_0_PERIPHERALS` contains the stream source, and the source's *register* side is clocked from the **FIC3** clock because its registers are reached over FIC3. One block, two clocks.

The latency unit lives entirely in the 50 MHz domain, which is why its resolution is 20 ns (one period of 50 MHz) and why it has no clock crossing; module H07 explains the design. The fabric's timestamp counter runs on `ACLK`, so its resolution is one 125 MHz period, 8 ns.

### The crossing, and why constraints are needed

A burst starts with a write in the 50 MHz domain and runs in the 125 MHz one; its status comes back the other way. Module H03 explains how: toggles through two-flip-flop synchronisers and snapshots copied on a synchronised toggle. The timing tool needs to be told too. The project's constraint file begins:

> `PCLK = FIC3 clock (50 MHz), ACLK = FIC0 clock (125 MHz); both come from the same PLL, so without these lines SmartTime times every crossing as if it were synchronous.`

The ratio is 2.5, so the edges of the two clocks do not line up at every period. If the two outputs are phase-aligned they coincide only every 40 ns (five 125 MHz periods, two 50 MHz periods). The tool times each crossing as a path between related clocks and reports violations on crossings that the RTL handles by design. The constraint file therefore declares exactly those paths false (see module K07 for the syntax it took to say so).

## Evidence

- **Build** (fabric v0.5.1): the place-and-route log reports `Timing constraints have been met` and a worst slack of 1.342 with the constraint file in place (the log prints no unit; Libero reports slack in nanoseconds).
- **Without the constraint file** the same design reports `Timing constraints have not been met`. The tool lists the 20 worst violating paths in each report, 40 in all, and every one is an intended crossing. It does not list the rest, so this is a check of the worst paths, not of all of them.

These are build results: they show the design meets timing with the crossings declared. They say nothing about metastability, which no RTL simulation shows either.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| Timing passes in the tool but a crossing misbehaves on the board | the constraint hides a path that is not really safe: every false path must be a path the RTL handles |
| Timing fails on a path from `PCLK` to `ACLK` after you add a register | a new crossing without a synchroniser, which the tool now times as if both clocks were related |
| Fabric registers read as zeros right after power-up | the fabric reset is still held: the PLL, the initialisation monitor or the MSS has not released it |
| A change to the constraint file loses all pin assignments | see module K08 |

## Trade-offs and going further

- **Two clocks, not one.** The 50 MHz register clock is the vendor's choice for FIC3, and the project did not change it. Running everything on one clock would remove the crossing, but would mean altering the vendor's FIC3 clock; that was not tried. The cost of the two clocks is the synchronisers and the constraints.
- **One reset per domain.** The reference design builds one reset per clock for this reason. The pfdaq RTL uses the reset as it is given, asserted asynchronously, and adds no reset synchronisers of its own.
- **Exercise:** the fabric timestamp counter is 32 bits at 125 MHz. How long before it wraps? Check your answer against the figure in the validation plan's list of instrumentation limits.
- **Exercise:** the latency unit's period register counts 50 MHz cycles. What period in microseconds does the value 10,000 give?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [F01 The chip and the board: MSS, fabric and the interfaces between them](F01-chip-and-board.md) |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Fabric clocks and resets](../system/README.md#fabric-clocks) · [FIC0 (AXI4, 125 MHz)](../system/README.md#fic0) · [FIC3 (APB, 50 MHz)](../system/README.md#fic3) · [pfdaq_core](../system/README.md#pfdaq-core) |
| **Source** | [`hw/libero/overlay/PFDAQ.tcl`](../../hw/libero/overlay/PFDAQ.tcl): `CLOCKS_AND_RESETS_0`, `FIC_3_CLK`, `RESETN_FIC_3_CLK`<br>[`hw/libero/overlay/pfdaq_cdc.sdc`](../../hw/libero/overlay/pfdaq_cdc.sdc)<br>[`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `ACLK`, `PCLK`, `RSTN`, `PRESETN`<br>[`hw/rtl/pfdaq_core.v`](../../hw/rtl/pfdaq_core.v): `pfdaq_core` |
| **Evidence** | [Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Timing analysis without the clock-crossing constraints (the tool's console log; the 40 worst violating paths it lists are the intended crossings)](../../evidence/v0.5.1/build/timing-without-cdc-constraints.txt) (build, fabric v0.5.1, 2026-11-15) |
| **Related** | [F01 The chip and the board: MSS, fabric and the interfaces between them](F01-chip-and-board.md)<br>[H01 An APB register block that software can trust](H01-apb-register-block.md)<br>[H03 Crossing clock domains: toggles, snapshots and constraints](H03-clock-domain-crossing.md)<br>[H07 Measuring software from hardware: the latency unit](H07-latency-unit.md)<br>[K07 Three builds lost to brackets in a constraint file](../case-studies/K07-brackets-in-constraints.md) |
| **Status** | validated |
<!-- nav:end -->
