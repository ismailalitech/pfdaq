---
id: M07
title: "Deferred interrupt handling, and a port that switches tasks inside the handler"
track: multicore
level: 4
summary: Moving work from an interrupt handler to a task, and the detail of this FreeRTOS port that breaks the usual pattern.
needs: [M03]
repro: base
status: validated
hardware: [plic, pfdaq-latency, u54-4]
software: [supervisor]
sources:
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:PLIC_f2m_5_IRQHandler,lat1_task,lat1_sem
  - tests/board/pfdaq_final_suite.sh
evidence: [suite-freertos, suite-baremetal]
related: [K06, P02, F03, M03, M04, K05]
---

# Deferred interrupt handling, and a port that switches tasks inside the handler

After this module you can say why an RTOS interrupt handler usually does as little as possible and hands the rest to a task, write that pattern for a level-triggered interrupt, and name the one property of a port that decides whether the usual pattern is safe. You will also have the measured price of the hand-over on this board.

## Before you start

- **Level 4.** You should know how a fabric interrupt reaches a hart (module F03) and what a real-time supervisor does (module M03). Case study K06 is the story of the bug that this page's pattern avoids; read it for the detail of what went wrong.
- **Board:** the shared baseline with the FreeRTOS supervisor. The numbers below were recorded earlier; nothing here was run while the page was written.

## The idea

An interrupt handler runs with other work held off. The longer it runs, the longer everything else waits, and the fewer RTOS services it may call: only the variants marked as safe for interrupt context. The usual remedy is to split the work in two:

- the **handler** (the "top half") does the minimum that cannot wait: silence the source so the interrupt does not return, and record what happened;
- a **task** (the "bottom half") does the rest, with all the RTOS services available, and runs when the scheduler lets it.

In FreeRTOS the hand-over is a semaphore. The handler gives it with the interrupt-safe call `xSemaphoreGiveFromISR`, and a task blocked in `xSemaphoreTake` becomes ready. The handler then asks for a context switch on the way out, so that the woken task runs immediately if it has the highest priority. The standard macro for that request is `portEND_SWITCHING_ISR`.

Two properties of this arrangement matter for a **level-triggered** interrupt, which is what the fabric delivers here (the line stays high until it is acknowledged, module H07):

1. The source must be **masked** between the handler and the task's acknowledgement. Otherwise the line, still high, interrupts again at once.
2. Whoever unmasks it must do so **after** whoever masked it. The order is the whole design.

## On this board

The supervisor on hart 4 answers latency channel 1, fabric line F2M[5] (PLIC source 123), in one of two ways. Linux selects the way with a message, and the tool does it with `pfdaq_cap -L 0` or `-L 1`.

### Mode 1: acknowledge in the handler

```c
SRC_REG(LAT1_ACK) = 1u;         /* this write drops the line and stops the fabric timer */
lat1_isr_count++;
return EXT_IRQ_KEEP_ENABLED;
```

One register write releases the line. Nothing is masked, nothing is deferred, no RTOS service is called. This is the cheap case, and it is also what the bare-metal build does.

### Mode 2: acknowledge in a task

```c
/* in the handler */
BaseType_t woken = pdFALSE;
PLIC_DisableIRQ(PLIC_F2M_5_INT_OFFSET);          /* mask the source here */
(void)xSemaphoreGiveFromISR(lat1_sem, &woken);
portEND_SWITCHING_ISR(woken);
return EXT_IRQ_KEEP_ENABLED;

/* the task */
(void)xSemaphoreTake(lat1_sem, portMAX_DELAY);
SRC_REG(LAT1_ACK) = 1u;                          /* release the line */
lat1_isr_count++;
PLIC_EnableIRQ(PLIC_F2M_5_INT_OFFSET);           /* unmask */
```

The semaphore is binary, created with the task at start-up. The task has the highest priority the kernel offers (`configMAX_PRIORITIES - 1`), so it runs as soon as it is woken. The mode switch is a plain variable, set by the `LATMODE` message; in the bare-metal build the whole task path is compiled out (`#ifndef PFDAQ_BM`) because there are no tasks.

### The detail that decides it

The vendor's interrupt dispatcher offers a tidy way to mask a source: return `EXT_IRQ_DISABLE`, and the dispatcher masks it after the handler returns. The first version of mode 2 used it. It answered one event and then never again.

The reason is in this RISC-V port of FreeRTOS, where `portEND_SWITCHING_ISR` does not merely ask for a switch. It switches **there and then, inside the handler**. The woken task runs to its next blocking call before the handler's own tail executes. The order of events was therefore:

| Step | What runs |
|---|---|
| 1 | The handler gives the semaphore |
| 2 | `portEND_SWITCHING_ISR` switches to the task immediately |
| 3 | The task acknowledges the line and **unmasks** the source |
| 4 | The task blocks; control returns to the handler's tail |
| 5 | The handler returns `EXT_IRQ_DISABLE` and the dispatcher **masks** the source |

The unmask comes before the mask, and the line stays masked for ever. The fix is in the code above: the handler masks the source itself, before waking the task, and returns "keep enabled". Case study K06 has the full trace.

The general rule: in a port that switches inside the macro, **anything the handler leaves for after the macro runs after the task has already run.** Check what your port's macro does before relying on an "on return" action.

## Run it

The project ran these commands on the board (they are in `tests/board/pfdaq_final_suite.sh`, with `N=100000` events):

```sh
pfdaq_cap -L 0                          # supervisor answers in the interrupt handler
pfdaq_lat -c 1 -p 200 -n 100000         # channel 1, one event every 200 us
pfdaq_cap -L 1                          # supervisor answers in a task
pfdaq_lat -c 1 -p 200 -n 100000
pfdaq_cap -L 0                          # back to the default
```

`pfdaq_lat` configures the fabric's latency unit, which raises the interrupt and measures, in 20 ns steps, how long it takes to be acknowledged (module H07).

## What you should see

Interrupt-to-acknowledge latency on hart 4, in microseconds, from the FreeRTOS suite (`suite-freertos`). Each row is one run of 100,000 events at a 200 µs period; "loaded" means one busy loop on each Linux core plus a continuous capture loop. No event was missed in any row.

| Who acknowledges | Load | Min | Mean | p99 ≤ | Max |
|---|---|---|---|---|---|
| Handler | idle | 0.52 | 0.52 | 0.62 | 2.10 |
| Handler | loaded | 0.52 | 0.53 | 0.94 | 1.94 |
| Task | idle | 2.00 | 2.04 | 2.54 | 11.66 |
| Task | loaded | 2.00 | 2.09 | 3.82 | 9.36 |

The bare-metal suite (`suite-baremetal`) has only the handler rows, since it has no tasks: 0.50 µs mean idle, 3.10 µs worst; 0.51 µs mean loaded, 2.44 µs worst.

## Evidence

- **The price of deferring is about 1.5 µs on average** (mean 2.04 against 0.52, idle) and the worst case grows from 2.10 µs to 11.66 µs. Module P02 discusses these numbers in the context of all five responders.
- **The cost is in the extra path:** masking the source, giving the semaphore, saving the interrupted context and restoring the task's, running the task, and writing the acknowledgement and unmask. How the 1.5 µs divides among them was not measured.
- **The pattern is stable.** All four rows cover 100,000 events with no overrun; the line is re-armed correctly for every event, which is exactly what the first version could not do.
- **Evidence for the failure** is the case study K06's reasoning from the code; the failing run's capture was not kept.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| The first event is answered and the rest never arrive | The source is masked at the end of the first event: the unmask happened before a later mask (above) |
| The handler runs again and again with no progress | The source was never masked and the line is still high |
| Events arrive but acknowledgements lag by tens of microseconds | The task is not the highest priority, or something of higher priority is running |

## Trade-offs and going further

- **Deferral buys flexibility, not speed.** In this design the whole job is one register write, so deferring it gains nothing and costs 1.5 µs. The task path exists to measure the cost and to show the pattern; real work (parsing, queues, anything that blocks) would justify it.
- **A binary semaphore coalesces events.** If the source were not masked between events, a second event arriving before the task ran would add nothing, because the semaphore holds at most one. The masking here prevents that case from arising.
- **Priority is the second knob.** The task has the top priority so that it runs immediately. A lower priority would let other tasks delay the acknowledgement; that was not measured.
- **Exercise:** the handler masks with `PLIC_DisableIRQ`. Which of the five steps in the table would change if it masked by writing a status register in the fabric instead?
- **Exercise:** the fabric latency unit counts from the first cycle the line is high. Where, in the timeline of mode 2, does the counter stop, and why is that not the moment the task starts?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [Platform-level interrupt controller](../system/README.md#plic) · [Latency unit (two channels)](../system/README.md#pfdaq-latency) · [U54 hart 4](../system/README.md#u54-4) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Source** | [`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `PLIC_f2m_5_IRQHandler`, `lat1_task`, `lat1_sem`<br>[`tests/board/pfdaq_final_suite.sh`](../../tests/board/pfdaq_final_suite.sh) |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with the bare-metal supervisor on hart 4, 8 of 8](../../evidence/v0.5.1/board/suite-baremetal.md) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [K06 The interrupt that stayed masked after one event](../case-studies/K06-interrupt-stayed-masked.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md)<br>[M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md)<br>[M04 Same core, same code: FreeRTOS against bare metal](M04-freertos-vs-bare-metal.md)<br>[K05 Two interrupts for every event](../case-studies/K05-two-interrupts-per-event.md) |
| **Status** | validated |
<!-- nav:end -->
