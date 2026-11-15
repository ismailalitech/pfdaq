---
id: F03
title: "Interrupts from fabric to hart: F2M lines, the PLIC and contexts"
track: fundamentals
level: 2
summary: How a wire in the fabric becomes an interrupt on one chosen hart, and how two operating systems share one controller.
needs: [F01]
repro: base
status: validated
hardware: [plic, pfdaq-latency, dma, ihc, u54-linux, u54-4]
software: [pfdaq-lat, supervisor]
sources:
  - hw/libero/overlay/PFDAQ.tcl:IRQ_LAT0,IRQ_LAT1
  - hw/rtl/pfdaq_core.v:IRQ_LAT0,IRQ_LAT1
  - yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts:pfdaq_src
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:PLIC_f2m_5_IRQHandler,PLIC_EnableIRQ,PLIC_SetPriority
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c:empty_wakeups
evidence: [suite-freertos-raw, supervisor-console]
related: [H07, I02, P02, K05, K06, F06, M02]
---

# Interrupts from fabric to hart: F2M lines, the PLIC and contexts

After this module you can follow an interrupt from a signal in the fabric to the handler that answers it, work out its interrupt number without looking it up, and explain why Linux on three harts and a bare-metal program on the fourth can use the same interrupt controller without getting in each other's way.

## Before you start

- **Level 2.** You should know what an interrupt is and what a device tree does. No RISC-V knowledge beyond "machine mode is the most privileged mode, supervisor mode is where an operating system kernel runs".
- **Tools:** the board with the shared baseline (fabric v0.5.1 and the pfdaq image), and a shell on it over SSH or the serial console.
- **Time:** about two minutes of run time.

## The idea

A processor core has very few interrupt inputs of its own. Everything that comes from peripherals is collected by one block, the **platform-level interrupt controller (PLIC)**, which decides which core is told about which interrupt. Four ideas describe it completely.

**Sources.** Every interrupt wire that enters the PLIC has a number, its *source*. Source 0 means "no interrupt".

**Contexts.** The PLIC does not deliver to a core as such. It delivers to a *context*: one privilege mode of one hart. A hart that has both machine mode and supervisor mode therefore appears twice. Each context has its own copy of three things:

| Per context | Purpose |
|---|---|
| Enable bits, one per source | "this context wants this source" |
| Threshold | sources with a priority at or below it are held back |
| Claim/complete register | read to take an interrupt, written to finish it |

**Priority.** There is one priority value per source, shared by every context. Priority 0 means "never deliver".

**Claim and complete.** When an enabled source is pending, the PLIC raises the external-interrupt input of that context. The handler reads the claim register and gets the source number; that read is the *claim*. When it has finished, it writes the same number back; that is the *complete*.

Most sources are **level-triggered**: the wire stays high for as long as the peripheral wants attention. The PLIC cannot make it go low. Only the handler can, by doing whatever the peripheral considers an acknowledgement, such as writing a register. If the handler completes while the wire is still high, the PLIC treats that as a new request and the interrupt arrives again.

## On this board

### From fabric signal to source number

The MSS has 64 interrupt inputs from the fabric, named `MSS_INT_F2M[0]` to `MSS_INT_F2M[63]` (F2M: fabric to MSS). They enter the PLIC as consecutive sources:

> **PLIC source = 118 + F2M index**

This was confirmed three ways:

1. **The vendor's hardware abstraction layer.** In `mss_plic.h` of the firmware tree (`src/platform/mpfs_hal/common/`), the source enumeration contains

   ```c
   #define OFFSET_TO_MSS_GLOBAL_INTS 13U
   ...
   PLIC_F2M_0_INT_OFFSET            = 105 + OFFSET_TO_MSS_GLOBAL_INTS,
   PLIC_F2M_5_INT_OFFSET            = 110 + OFFSET_TO_MSS_GLOBAL_INTS,
   PLIC_F2M_63_INT_OFFSET           = 168 + OFFSET_TO_MSS_GLOBAL_INTS,
   ```

   so F2M[0] is 118, F2M[5] is 123 and F2M[63] is 181.

2. **The vendor's kernel device tree.** `mpfs-disco-kit-fabric.dtsi` gives the fabric DMA controller, which the reference design wires to F2M[2], `interrupts = <120>`.

3. **The board.** The project's two new lines were wired with this formula and nothing else, and both deliver interrupts: 100,000 of 100,000 events acknowledged on each in the board suite.

The PLIC of this chip has sources 1 to 186 (`riscv,ndev = <186>` in the kernel's `mpfs.dtsi`), at address `0x0C00_0000`.

### The lines this design uses

| Fabric signal | F2M line | PLIC source | Taken by | Wired in |
|---|---|---|---|---|
| Latency unit, channel 0 (`IRQ_LAT0`) | 1 | 119 | Linux | `hw/libero/overlay/PFDAQ.tcl` |
| DMA controller, transfer complete | 2 | 120 | Linux | reference design |
| Latency unit, channel 1 (`IRQ_LAT1`) | 5 | 123 | hart 4, machine mode | `hw/libero/overlay/PFDAQ.tcl` |
| DIP switches 1 to 4 | 6 to 9 | 124 to 127 | nobody in this design | reference design |
| Push button `SWITCH1` | 10 | 128 | nobody in this design | reference design |
| Mi-V IHC, hart 4 | 59 | 177 | hart 4 | reference design |
| Mi-V IHC, harts 3, 2, 1 | 60, 61, 62 | 178, 179, 180 | Linux | reference design |
| Mi-V IHC, E51 | 63 | 181 | HSS | reference design |

The reference design ties F2M[1] and F2M[5] to ground. The integration script removes the tie-off and connects the latency unit instead:

```tcl
sd_clear_pin_attributes -sd_name {MPFS_DISCOVERY_KIT} -pin_names {MSS_WRAPPER_0:MSS_INT_F2M[1:1]}
sd_clear_pin_attributes -sd_name {MPFS_DISCOVERY_KIT} -pin_names {MSS_WRAPPER_0:MSS_INT_F2M[5:5]}
sd_connect_pins -sd_name {MPFS_DISCOVERY_KIT} -pin_names {"FIC_0_PERIPHERALS_0:IRQ_LAT0" "MSS_WRAPPER_0:MSS_INT_F2M[1:1]"}
sd_connect_pins -sd_name {MPFS_DISCOVERY_KIT} -pin_names {"FIC_0_PERIPHERALS_0:IRQ_LAT1" "MSS_WRAPPER_0:MSS_INT_F2M[5:5]"}
```

### Nine contexts, two owners

The five harts give the PLIC nine contexts. The vendor header numbers them, and the kernel's `plic` node lists them in the same order:

| Context | Hart and mode | Used by |
|---|---|---|
| 0 | hart 0 (E51), machine | HSS |
| 1, 3, 5 | harts 1 to 3, machine | belong to the machine-mode firmware under Linux; not used by this project |
| 2, 4, 6 | harts 1 to 3, supervisor | Linux |
| 7 | hart 4, machine | the pfdaq supervisor |
| 8 | hart 4, supervisor | nobody: `cpu4` is disabled in the device tree |

This is what lets two operating systems share the controller. Each side writes only its own contexts' enable bits and reads only its own claim registers, so a source that only one side has enabled can never reach the other.

**Linux** learns its sources from the device tree. The project adds one node for the latency unit's channel 0:

```dts
pfdaq_src: uio@40000f00 {
    compatible = "generic-uio";
    linux,uio-name = "pfdaq_stream_src";
    reg = <0x0 0x40000f00 0x0 0x100>;
    /* latency unit channel 0: fabric F2M[1] = PLIC source 119 (118 + F2M index) */
    interrupt-parent = <&plic>;
    interrupts = <119>;
};
```

Source 123 appears nowhere in the device tree, so Linux never enables it.

**Hart 4** enables its one source directly, in `firmware/u54_4-supervisor/pfdaq_supervisor.c`:

```c
PLIC_SetPriority(PLIC_F2M_5_INT_OFFSET, 2);
PLIC_EnableIRQ(PLIC_F2M_5_INT_OFFSET);
```

`PLIC_EnableIRQ` reads the hart number and, on hart 4, sets the bit in that hart's machine-mode enable array only. `PLIC_SetPriority` writes the one priority register that source 123 has. Priorities are the single thing the two sides share; it causes no conflict here because every fabric source in the table has exactly one owner.

The handler is found by name. The vendor's trap code claims the interrupt, calls the function at that source's position in a table, and completes it afterwards. The table entry for source 123 is `PLIC_f2m_5_IRQHandler`, a weak empty function that the supervisor replaces:

```c
uint8_t PLIC_f2m_5_IRQHandler(void)
{
    ...
    SRC_REG(LAT1_ACK) = 1u;         /* this write drops the line and stops the fabric timer */
    lat1_isr_count++;
    return EXT_IRQ_KEEP_ENABLED;
}
```

### Level-triggered, and what that costs

Both latency channels hold their line high from the event until software writes the channel's `ACK` register. That write is the acknowledgement; claim and complete alone do not lower the line.

On hart 4 the handler writes `ACK` before it returns, so the line is already low when the trap code completes the interrupt. One event, one interrupt.

Under Linux the generic UIO driver does something different, because the acknowledgement belongs to a user program the kernel knows nothing about. Its handler masks the interrupt and wakes the program; the program writes `ACK` and then asks for the interrupt to be unmasked. On this board that sequence produced **two wake-ups per event**: the line was still high when the kernel handler completed, the PLIC recorded a new request, and delivered it when the line was unmasked, although the program had acknowledged by then. `pfdaq_lat` therefore acknowledges only when the fabric's `STATUS` register says an interrupt is really pending, and counts the other wake-ups. In the recorded run of 100,000 events it counted 99,967 empty wake-ups. Module I02 and case study K05 cover this in detail.

```mermaid
flowchart LR
  subgraph fabric["FPGA fabric"]
    lat0["Latency channel 0"]
    dma["DMA controller"]
    lat1["Latency channel 1"]
  end
  subgraph mss["MSS"]
    plic["PLIC"]
    linux["Harts 1 to 3<br>supervisor contexts"]
    h4["Hart 4<br>machine context"]
  end
  lat0 -.->|"F2M[1] = 119"| plic
  dma -.->|"F2M[2] = 120"| plic
  lat1 -.->|"F2M[5] = 123"| plic
  plic -.->|"119, 120 enabled"| linux
  plic -.->|"123 enabled"| h4
```

## Evidence

- **Board, fabric v0.5.1:** in the board suite, channel 0 (source 119, Linux) and channel 1 (source 123, hart 4) each delivered 100,000 of 100,000 events in every run, and the DMA interrupt (source 120) completed every transfer of every capture test. The raw suite output holds the two JSON objects quoted above.
- **Board:** the supervisor console shows the two messages quoted above.
- **Source inspection:** the formula 118 + n, the nine contexts and the per-context registers are read from the vendor header and device tree named above, not measured.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| The fabric raises the line but no handler runs | The source is enabled in a different context from the one you are watching, or its priority is still 0 |
| Linux reports an interrupt storm, or the board appears to hang, as soon as a source is enabled | A level-triggered line that nothing acknowledges: complete without acknowledge delivers it again at once |
| Every event wakes a UIO program twice | The level is latched again when the kernel handler completes (above, and case study K05) |
| A handler on hart 4 runs once and never again | The source was left masked: something disabled it after the last enable (case study K06) |
| The wrong device answers | Off by one in the formula: F2M[0] is source 118, not 119 |
| A fabric line that should be idle reads as active | The reference design ties unused F2M inputs to ground; a new design must do the same |

## Trade-offs and going further

- **One owner per source.** Nothing in the PLIC stops two contexts from enabling the same source; whichever claims first gets it. This design never does that. Sharing a source between Linux and hart 4 would need an agreement outside the PLIC.
- **Machine mode on hart 4 pays no toll.** The supervisor's context is a machine-mode context, so the interrupt reaches its handler without passing through any other software. Linux's interrupts arrive in supervisor mode. Module P02 measures the difference.
- **Level against edge.** A level line cannot lose an event while the handler is busy, which is why the latency unit uses one. The price is the rule that the handler must acknowledge before it completes, and the double wake-up under generic UIO.
- **Exercise:** DIP switch 1 drives F2M[6]. Work out its source number, then decide what a device-tree node for it would need, and what would happen with a level-triggered handler while the switch stays on.
- **Exercise:** the supervisor sets priority 2 for source 123, and the vendor start-up code sets hart 4's threshold to 0. Which priority values would stop delivery?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [F01 The chip and the board: MSS, fabric and the interfaces between them](F01-chip-and-board.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [Platform-level interrupt controller](../system/README.md#plic) · [Latency unit (two channels)](../system/README.md#pfdaq-latency) · [CoreAXI4DMAController](../system/README.md#dma) · [Mi-V inter-hart communication](../system/README.md#ihc) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [pfdaq_lat](../system/README.md#pfdaq-lat) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Source** | [`hw/libero/overlay/PFDAQ.tcl`](../../hw/libero/overlay/PFDAQ.tcl): `IRQ_LAT0`, `IRQ_LAT1`<br>[`hw/rtl/pfdaq_core.v`](../../hw/rtl/pfdaq_core.v): `IRQ_LAT0`, `IRQ_LAT1`<br>[`yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts`](../../yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts): `pfdaq_src`<br>[`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `PLIC_f2m_5_IRQHandler`, `PLIC_EnableIRQ`, `PLIC_SetPriority`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c): `empty_wakeups` |
| **Evidence** | [Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Supervisor console during the fresh-card suite, with the alarm and recovery messages](../../evidence/v0.5.1/board/supervisor-console.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [H07 Measuring software from hardware: the latency unit](H07-latency-unit.md)<br>[I02 Fabric interrupts in user space, and the double wake-up](I02-fabric-interrupts-user-space.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md)<br>[K05 Two interrupts for every event](../case-studies/K05-two-interrupts-per-event.md)<br>[K06 The interrupt that stayed masked after one event](../case-studies/K06-interrupt-stayed-masked.md)<br>F06 GPIO from both sides: MSS GPIO and fabric CoreGPIO *(planned)*<br>[M02 RPMsg in practice: announce, bind, ping](M02-rpmsg-in-practice.md) |
| **Status** | validated |
<!-- nav:end -->
