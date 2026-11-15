---
id: K06
title: "The interrupt that stayed masked after one event"
track: case-study
level: 4
summary: A task that never ran again after its first wake-up.
repro: doc
status: validated
hardware: [pfdaq-latency, plic, u54-4]
software: [supervisor]
sources:
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:PLIC_f2m_5_IRQHandler,lat1_task
evidence: [suite-freertos, suite-freertos-raw]
related: [M07, F03, P02, H07]
---

# The interrupt that stayed masked after one event

The supervisor on hart 4 can answer the fabric latency interrupt in two ways: directly in the interrupt handler, or in a FreeRTOS task that the handler wakes. The first way worked at once. The second answered exactly one event and then never again. This page records why, and why the fix has to be written the way it is.

## Symptom

In task mode the latency measurement worked for the first event and then stopped: the interrupt was never taken again. The project's notes record it in one line, "line stayed masked after the first event", made during the latency work on the board. Handler mode, where the interrupt handler writes the acknowledge register itself, was not affected.

The raw capture of the failing run was not kept, so there are no counts to quote. What is published is the result after the fix, at the end of this page.

## Isolation

Only two things are recorded: that the symptom appeared in task mode alone, and which part of the code the fix changed. The reasoning that led there is reconstructed here from the code, not from a log.

- **Handler mode worked.** The same fabric unit, the same interrupt line and the same acknowledge write were used in both modes, so the fabric and the line itself were not the problem. Only what happens between the interrupt and the acknowledge differs.
- **The first event worked.** The semaphore, the task and the acknowledge write were therefore all functional; a task that could not run would have failed on event one.
- **What is left is the per-source enable at the interrupt controller.** Task mode has to mask the source while the task has not yet acknowledged. A level interrupt stays high until it is acknowledged, so a source left enabled would be delivered again at once, before the task had run, and the core would loop in the handler. The design therefore masks and unmasks the line, and the symptom means the final state of that enable bit was "off".

The design was right. The order in which its steps actually ran was not the one it assumed.

## Cause

**Established.** The vendor interrupt dispatcher in the platform support code has this shape (from the hardware abstraction layer of polarfire-soc-amp-examples v2025.07):

```c
disable = ext_irq_handler_table[int_num]();   /* the handler registered for this source */
PLIC_CompleteIRQ(int_num);
if (EXT_IRQ_DISABLE == disable)
    PLIC_DisableIRQ((PLIC_IRQn_Type)int_num);
```

A handler can ask for its source to be masked by returning `EXT_IRQ_DISABLE`; the dispatcher does it *after* the handler has returned. That is the natural way to write "mask until the task is done". The first version of task mode did exactly that:

```c
(void)xSemaphoreGiveFromISR(lat1_sem, &woken);
portEND_SWITCHING_ISR(woken);
return EXT_IRQ_DISABLE;
```

The catch is `portEND_SWITCHING_ISR`. In most FreeRTOS ports it only requests a context switch, which happens when the handler has returned. In the RISC-V port used here it is defined as

```c
#define portEND_SWITCHING_ISR( xSwitchRequired ) if( xSwitchRequired ) vPortYieldISR()
```

and `vPortYieldISR` in `portasm.S` saves the interrupted context, runs `vTaskSwitchContext` and restores the context of the next task **right there, inside the handler**. The comment in the vendor assembly says that on return from the switch "the context is restored and a normal return issued instead of mret", which returns "to the tail of the ISR".

So the order of events on the first interrupt was:

| Step | What runs |
|---|---|
| 1 | The handler gives the semaphore. The woken task is created at the highest priority available (`configMAX_PRIORITIES - 1`), so it is chosen to run next. |
| 2 | `portEND_SWITCHING_ISR` switches to the task at once, still inside the handler. |
| 3 | The task writes the acknowledge and calls `PLIC_EnableIRQ`. The source is enabled. |
| 4 | The task blocks on the semaphore again. Control returns to the tail of the handler. |
| 5 | The handler returns `EXT_IRQ_DISABLE`. The dispatcher completes the interrupt and **disables the source**. |

The task unmasks the line in step 3 and the dispatcher masks it in step 5. The last write wins, and nothing is left to unmask it again. The next event is raised by the fabric and held at the controller for ever.

**Not established.** Whether other handlers are affected by the same ordering was not investigated. The project's own code has one handler that wakes a task; the vendor RPMsg layer also switches tasks from its interrupt path and was not examined for this.

## Fix

The handler masks the source itself, before it wakes the task, and returns without asking the dispatcher to do anything. This is the code that runs now, in `firmware/u54_4-supervisor/pfdaq_supervisor.c`:

```c
BaseType_t woken = pdFALSE;
PLIC_DisableIRQ(PLIC_F2M_5_INT_OFFSET);
(void)xSemaphoreGiveFromISR(lat1_sem, &woken);
portEND_SWITCHING_ISR(woken);
return EXT_IRQ_KEEP_ENABLED;
```

and the task:

```c
(void)xSemaphoreTake(lat1_sem, portMAX_DELAY);
SRC_REG(LAT1_ACK) = 1u;
lat1_isr_count++;
PLIC_EnableIRQ(PLIC_F2M_5_INT_OFFSET);
```

Now every write to the enable bit is in the order the design needs: mask (handler), acknowledge and unmask (task). The dispatcher's tail has nothing to disable because the handler returned `EXT_IRQ_KEEP_ENABLED`.

The command that switches the supervisor into task mode also drops any event that is pending and unmasks the line, so a board left in the stuck state by an earlier run recovers when the mode is set again.

## Result after the fix

On fabric v0.5.1, 100,000 events at a 200 µs period were answered by the task, with the board idle and with Linux loaded. In both runs every event was acknowledged: `events` equals `acks`, with no overrun and no timeout (rows `T8-u54_4-task-idle` and `T8-u54_4-task-load` in the suite record).

| Run | Mean | Worst | Missed |
|---|---|---|---|
| Task, board idle | 2.04 µs | 11.66 µs | 0 |
| Task, Linux loaded | 2.09 µs | 9.36 µs | 0 |

Module P02 compares these with the handler-only and Linux responders. The 1.5 µs or so that the task adds over the handler is the cost of the semaphore and the context switch, and it is the price of putting the acknowledge in task context.

## How to recognise it

| What you see | What it tells you |
|---|---|
| Interrupt-driven code works for one event and then stops, while the peripheral keeps raising events | Suspect the interrupt controller's enable for that source before the peripheral or the task |
| The interrupted task or the woken task runs "inside" the handler, and the code after the switch call runs later than you expect | The port switches contexts in the handler; anything that must happen before the task runs has to be written before the switch call |
| A handler returns a value that the dispatcher acts on afterwards (here, a request to mask) | The dispatcher's action happens after any task switch inside the handler. State changed by the woken task can be undone by it |
| The status register of the peripheral shows an event pending but the core never entered the handler | The line is masked at the controller; check the source's enable bit |

Two habits follow: read the context-switch macro of the port you use, because `portEND_SWITCHING_ISR` is not one thing across FreeRTOS ports; and keep the masking and unmasking of a source in code you can read in one place instead of splitting it between a handler's return value and a task.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Latency unit (two channels)](../system/README.md#pfdaq-latency) · [Platform-level interrupt controller](../system/README.md#plic) · [U54 hart 4](../system/README.md#u54-4) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Source** | [`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `PLIC_f2m_5_IRQHandler`, `lat1_task` |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [M07 Deferred interrupt handling, and a port that switches tasks inside the handler](../modules/M07-deferred-interrupt-handling.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](../modules/F03-fabric-interrupts.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](../modules/P02-interrupt-latency.md)<br>[H07 Measuring software from hardware: the latency unit](../modules/H07-latency-unit.md) |
| **Status** | validated |
<!-- nav:end -->
