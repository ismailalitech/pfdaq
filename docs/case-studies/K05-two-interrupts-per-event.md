---
id: K05
title: "Two interrupts for every event"
track: case-study
level: 3
summary: A level interrupt, a kernel that masks it, and a process woken twice.
repro: doc
status: validated
hardware: [pfdaq-latency, plic]
software: [pfdaq-lat, uio, pfdaq-uio]
sources:
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c:empty_wakeups
  - hw/rtl/pfdaq_latency.v:pending,spurious
  - yocto/meta-pfdaq/recipes-kernel/pfdaq-uio/files/pfdaq_uio.c:pfdaq_uio_handler,pfdaq_uio_irqcontrol
evidence: [suite-freertos-raw, suite-freertos]
related: [F03, I02, P02, H07]
---

# Two interrupts for every event

The latency unit raises one interrupt per measurement. Linux user space was woken twice for each. This page records how that was found, the explanation the project settled on, what has and has not been checked, and the small rule in the tool that keeps the measurement correct.

## Symptom

`pfdaq_lat` measures how long Linux takes to answer the fabric's interrupt. It waits for the interrupt with a blocking `read()` on the UIO device, acknowledges by writing the latency unit's `ACK` register, and re-enables the interrupt line with a 4-byte write.

When the Linux latency channel was first measured, every event woke the process twice. The first wake-up found the interrupt pending. The second found nothing to acknowledge. The notes record this finding and the change to the tool, but not the first runs' output, so no log of the first double wake-ups is available. The figures below come from the final suite.

## Isolation

The latency unit makes the question cheap to ask, because it knows whether an interrupt is pending. Its status register has a pending bit (`STATUS` bit 0), and the RTL counts an `ACK` that arrives with nothing pending:

```verilog
end else begin
    spurious <= spurious + 32'd1;
end
```

Reading `STATUS` before acting therefore tells the tool which kind of wake-up it has. The tool counts the second kind as `empty_wakeups` and reports it. Repeating the measurement for every responder shows where the double wake-up occurs:

| Who answers | Events | `empty_wakeups` |
|---|---|---|
| Linux user space, default priority, idle | 100,000 | 99,967 |
| Linux user space, `SCHED_FIFO` 80, idle | 100,000 | 99,977 |
| Linux user space, `SCHED_FIFO` 80, loaded | 100,000 | 99,955 |
| Linux kernel handler (acknowledges inside the handler) | 100,000 | 0 |
| Hart 4, interrupt handler or task (four rows) | 100,000 each | 0 |

The figures are from the [raw suite record](../../evidence/v0.5.1/board/suite-freertos.jsonl), fabric v0.5.1, one run each. `spurious` is 0 in all nine latency rows, because the tool never acknowledges without a pending interrupt.

The double wake-up occurs only where Linux's generic UIO path stands between the interrupt and the acknowledge. It does not occur when the acknowledge happens inside the kernel handler, or on hart 4, which has its own interrupt path.

## Cause

**Established:** in the generic path, each event produces a wake-up with the interrupt pending and a second wake-up with it already acknowledged. The count is close to one empty wake-up per event, about 99.97 % in the idle default-priority run.

**The explanation recorded when it was found:** the interrupt is a *level*: it stays high until `ACK` is written. The generic UIO handler masks the line when it runs (`disable_irq_nosync`) and lets user space unmask it. The project's own driver, `pfdaq_uio`, has a mode (`kernel_ack=0`) that behaves like that generic path:

```c
spin_lock(&p->lock);
if (!__test_and_set_bit(0, &p->masked))
    disable_irq_nosync(irq);
spin_unlock(&p->lock);
return IRQ_HANDLED;
```

The notes say that the interrupt controller latches the still-high level again when the kernel handler completes, and delivers it as soon as the process re-enables the line, although the process has acknowledged it by then. In that reading, the second wake-up is the first event delivered a second time.

What this does **not** rest on: nobody traced the interrupt controller's claim and complete sequence, and the kernel's UIO core was not instrumented. The explanation fits every observation above, including the zeros in the kernel-handler row, but it is an explanation, not a measurement.

**Not explained:** why the count is a little below the number of events (99,967 of 100,000). It could be events whose two wake-ups were merged into one `read()`; that was not investigated.

## Fix

The latency unit needed no change. The tool acknowledges only when the fabric says an interrupt is really pending, counts the other wake-ups, and re-enables the line either way:

```c
if (lat[L_STATUS / 4] & 1)
    lat[L_ACK / 4] = 1;                  /* stops the fabric timer */
else
    empty_wakeups++;
if (write(fd, &one, 4) != 4) break;      /* re-enable the interrupt line */
```

The check matters for two reasons:

- An unconditional `ACK` on the second wake-up would increment `SPURIOUS` and, in principle, could acknowledge a *new* event that became pending in between, ending its measurement before the process had seen it. That is reasoning from the RTL, not something observed.
- The measured latency is still correct. The fabric stops its counter at the first `ACK` of an event, so the extra wake-up does not enter the figure. It does cost time in the process, and how much was not measured.

For the kernel-handler measurement the driver acknowledges inside the handler, returning `IRQ_NONE` when nothing is pending, so a second delivery is ignored:

```c
if (!(readl(p->base + LAT0_STATUS) & 1))
    return IRQ_NONE;
writel(1, p->base + LAT0_ACK);
```

## How to recognise it

| What you see | What it tells you |
|---|---|
| More wake-ups than interrupts the peripheral raised | Look at the peripheral's pending state at each wake-up |
| `SPURIOUS` or an equivalent "acknowledge with nothing pending" counter rising | You acknowledge unconditionally |
| Second wake-ups only on the generic UIO path | The path from the mask-and-notify handler is involved, not the peripheral |
| A level-triggered source and a handler that masks the line | The line is delivered again when unmasked unless the source is cleared first |

Habits that followed:

- **Let the hardware say whether it has anything to acknowledge.** A pending bit and a spurious counter turned a vague double wake-up into a countable fact.
- **Report the quirk, not just the fix.** `empty_wakeups` is in the tool's output, so the measurement shows its own side effects.
- **Pick the acknowledge point for the question.** Module P02 uses all three places (hart 4 handler, kernel handler, user space), and each reads a different part of the same interrupt's path.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Latency unit (two channels)](../system/README.md#pfdaq-latency) · [Platform-level interrupt controller](../system/README.md#plic) · [pfdaq_lat](../system/README.md#pfdaq-lat) · [UIO devices](../system/README.md#uio) · [pfdaq_uio kernel driver](../system/README.md#pfdaq-uio) |
| **Source** | [`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c): `empty_wakeups`<br>[`hw/rtl/pfdaq_latency.v`](../../hw/rtl/pfdaq_latency.v): `pending`, `spurious`<br>[`yocto/meta-pfdaq/recipes-kernel/pfdaq-uio/files/pfdaq_uio.c`](../../yocto/meta-pfdaq/recipes-kernel/pfdaq-uio/files/pfdaq_uio.c): `pfdaq_uio_handler`, `pfdaq_uio_irqcontrol` |
| **Evidence** | [Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](../modules/F03-fabric-interrupts.md)<br>I02 Fabric interrupts in user space, and the double wake-up *(planned)*<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](../modules/P02-interrupt-latency.md)<br>[H07 Measuring software from hardware: the latency unit](../modules/H07-latency-unit.md) |
| **Status** | validated |
<!-- nav:end -->
