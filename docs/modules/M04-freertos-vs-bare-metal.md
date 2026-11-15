---
id: M04
title: "Same core, same code: FreeRTOS against bare metal"
track: multicore
level: 3
summary: One supervisor source built both ways, and what the RTOS adds in time and in structure.
needs: [M03]
repro: base
status: validated
hardware: [u54-4, plic, ddr-u54-4-image]
software: [supervisor, hss]
sources:
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:PFDAQ_BM,now_ms,delay_ms,sup_recv,lat1_task,PLIC_f2m_5_IRQHandler
  - firmware/u54_4-supervisor/disco-kit/Makefile.bm:DPFDAQ_BM
  - firmware/u54_4-supervisor/disco-kit/Makefile.freertos:PFDAQ_SUPERVISOR
  - firmware/build_payload.sh:VAR
evidence: [suite-baremetal, suite-freertos, suite-freertos-raw, boot-hss, secure-boot-sign-verify]
related: [M01, M03, M07, P02, K06]
---

# Same core, same code: FreeRTOS against bare metal

After this module you can say exactly where the two builds of the supervisor differ, which tests were run on both, and what the recorded numbers do and do not show about the cost of having an RTOS. It rests on one fact that makes the comparison fair: both builds come from the same source file.

## Before you start

- **Level 3.** Module M03 describes what the supervisor does.
- **Board:** the shared baseline. Switching to the bare-metal build means writing a different payload to the SD card; the steps are the ones the project used, described below. This module was written while the board was not available and re-ran nothing.
- **Tools:** to rebuild: the RISC-V GCC from SoftConsole and a Yocto build tree. To read: none.

## The idea

An **RTOS** gives a small program a scheduler (several tasks with priorities), blocking primitives (semaphores, queues, timeouts) and a time base. **Bare metal** gives it none of those: one loop and the hardware. The RTOS makes a program easier to structure and costs memory and some time; bare metal is the opposite.

Whether the cost is worth paying is an empirical question, so the project builds the same program both ways. The vendor allows two software contexts on the chip, so the two builds are alternatives for hart 4 and are never present together.

## On this board

### One source, one flag

The supervisor is a single file, `pfdaq_supervisor.c`. The two vendor build projects differ by exactly one compiler flag, and the difference between the two Makefiles shows it:

```text
56c56
< override EXT_CFLAGS += -DPFDAQ_SUPERVISOR
---
> override EXT_CFLAGS += -DPFDAQ_SUPERVISOR -DPFDAQ_BM
```

Inside the file, the flag `PFDAQ_BM` selects four things.

| What | FreeRTOS build | Bare-metal build |
|---|---|---|
| Time base | `xTaskGetTickCount()`, ticks of 1 ms | the machine timer at 1 MHz, divided by 1000 |
| `delay_ms()` | `vTaskDelay()`, the task sleeps | a busy-wait on the timer |
| Waiting for a message with a timeout | the RPMsg queue call takes the timeout | the queue ignores it, so `sup_recv()` polls the queue size against the clock |
| Latency interrupt, acknowledged in a task | a high-priority task and a binary semaphore (`lat1_task`) | not available; a request for it is turned into "handler acknowledges" |

Two smaller differences: the bare-metal build must enable interrupts itself (`__enable_irq()`), and it sets bit 31 of the `uptime_ms` field of every acknowledgement, so Linux can tell which build it is talking to (module M02).

Everything else, the message handling, the deadline logic, the sequence check, the memory-visibility check, is the same text. The time base is the only reason the deadline code reads identically in both: the code only calls `now_ms()`.

![One source file, two builds. pfdaq_supervisor.c is built by Makefile.freertos with -DPFDAQ_SUPERVISOR and by Makefile.bm with -DPFDAQ_SUPERVISOR -DPFDAQ_BM. The flag selects the time base, delay_ms, the receive with timeout and the task path for the latency interrupt. The FreeRTOS payload with U-Boot is 1,376,656 bytes and the SD image carries it; the bare-metal payload is 842,208 bytes and is built by firmware/build_payload.sh.](../diagrams/lab-m04-one-source.svg)

1. One source file, `pfdaq_supervisor.c`, holds the whole supervisor. The macro `PFDAQ_BM` is the only switch.
2. The FreeRTOS build takes its time from the tick count and sleeps in `vTaskDelay()`. The receive call takes the timeout, and the latency interrupt can be answered in a task.
3. The bare-metal build adds `-DPFDAQ_BM`. It reads the 1 MHz machine timer, busy-waits in `delay_ms()` and polls the queue size to get a timeout. It has no tasks.
4. Each build sets bit 31 of `uptime_ms` in its acknowledgements, or leaves it clear, so Linux can tell which build it is talking to.
5. Each build is wrapped with U-Boot into one payload. Yocto builds the FreeRTOS one for the SD image; `firmware/build_payload.sh` builds the bare-metal one.

### The cost in memory

Measured with the toolchain's `size` tool on the two build outputs for this page. These are not an evidence record; they come from the images built for the board suites.

| Image | text | data | bss |
|---|---|---|---|
| FreeRTOS firmware | 56,664 | 527,792 | 44,048 |
| bare-metal firmware | 46,552 | 3,472 | 566,528 |

The code is about 10 KB larger with the RTOS. The large `data` of the FreeRTOS build is the reason its loadable image is bigger: the payload, which carries U-Boot as well, is 1,376,656 bytes for FreeRTOS (the boot monitor's console line, `boot-hss`) and 842,208 bytes for the bare-metal configuration (the signed-payload demonstration of module S03, `secure-boot-sign-verify`). Why the totals of the two builds are nearly equal while the split differs was not investigated.

### Building and switching

- The FreeRTOS firmware is built by Yocto and is what the SD image carries.
- The bare-metal firmware is built outside Yocto with `firmware/build_payload.sh`, which takes a variant (`bm` or `freertos`), wraps the result with U-Boot into an HSS payload, and writes it to a build directory. It passes `MPFS_HAL_FIRST_HART=4` and `MPFS_HAL_LAST_HART=4`, so the build is for hart 4 only.
- To run the comparison, the project wrote the payload to the SD card's payload partition (partition 2), rebooted, ran the bare-metal suite, and then wrote the image's own payload back. Check the partition number twice before writing: a write to the wrong one destroys the card's contents.

## Run it

These are the commands the project ran.

1. Build the bare-metal payload (no board):

   ```sh
   firmware/build_payload.sh
   ```

2. On the board, with the bare-metal payload in place, run the suite for it:

   ```sh
   tests/board/pfdaq_bm_suite.sh
   ```

   Its first line reports which build is running (`INFO u54_4_context=…`), which `pfdaq_cap` takes from the same bit 31 described above.

To read the recorded results:

```sh
cat evidence/v0.5.1/board/suite-baremetal.md
```

## What you should see

Each suite record starts with a line that names the build that ran on hart 4. This is how to read the two records side by side:

![Two recorded suite summaries read with grep. Each starts with a context line naming the build that ran on hart 4, freertos or bare-metal. The interrupt-handler latency row at idle reads mean 0.52 microseconds and worst 2.10 for FreeRTOS, and mean 0.50 and worst 3.10 for bare metal.](../diagrams/lab-m04-which-build.svg)

1. The first line of a suite record names the context. Linux reads it from the supervisor's build tag, bit 31.
2. FreeRTOS, handler acknowledging, at idle: 100,000 events, mean 0.52 µs, worst 2.10 µs.
3. The same suite on bare metal. Only the payload on the SD card changed, and the context line says `bare-metal`.
4. Bare metal, the same row: mean 0.50 µs, which is 0.02 µs less, one count of the 20 ns instrument. The worst case of 3.10 µs is a single sample.

`suite-baremetal` records eight tests, all passing. They are eight of the FreeRTOS suite's tests, run with the same command lines:

| Test | Bare metal | FreeRTOS |
|---|---|---|
| `T4-counter`, `T4-fir` (32,768 frames each) | pass, 0 errors | pass, 0 errors |
| `T9-crc-prbs` (injected CRC faults) | pass, 4 detected | pass, 4 detected |
| `T9-deadline` (stall past the deadline) | pass, 1 miss | pass, 1 miss |
| `T7-noncached`, per check | 29.88 ms | 31.15 ms |
| `T7-cached`, per check | 18.77 ms | 20.10 ms |
| `T8` interrupt latency, handler, idle | mean 0.50 µs, max 3.10 µs | mean 0.52 µs, max 2.10 µs |
| `T8` interrupt latency, handler, Linux loaded | mean 0.51 µs, max 2.44 µs | mean 0.53 µs, max 1.94 µs |

The FreeRTOS suite also measures the task-acknowledged case, which the bare-metal build cannot do: idle, mean 2.04 µs and worst 11.66 µs; loaded, mean 2.09 µs and worst 9.36 µs.

The supervisor's round trip, from the raw records, in microseconds:

| Test | Build | Min | Mean | Max |
|---|---|---|---|---|
| `T4-counter` | bare metal | 1,222 | 1,658 | 9,431 |
| `T4-counter` | FreeRTOS | 1,249 | 1,974 | 10,020 |
| `T4-fir` | bare metal | 1,232 | 1,653 | 9,429 |
| `T4-fir` | FreeRTOS | 1,257 | 2,134 | 10,012 |

## Do the same in your project

You have a firmware program and you want to know what an RTOS would cost it, or what it would give you, on your own hardware. Build the one program both ways, so that the comparison is fair.

1. **Put every operating-system call behind a few small functions.** The supervisor has four places: the time base (`now_ms()`), the delay (`delay_ms()`), the receive with a timeout (`sup_recv()`) and the interrupt path that wakes a task (`PLIC_f2m_5_IRQHandler`, `lat1_task`). Everything else calls those and nothing else.
2. **Select the variant with one macro.** Here it is `PFDAQ_BM`. Write the RTOS side and the bare-metal side of each function in one `#ifdef PFDAQ_BM` block, so that the difference is visible in one place.
3. **Keep two build projects that differ by one compiler flag.** `Makefile.freertos` has `-DPFDAQ_SUPERVISOR`; `Makefile.bm` has `-DPFDAQ_SUPERVISOR -DPFDAQ_BM`. Rename the flags to yours and check that a `diff` of the two Makefiles shows only that line.
4. **Do what the bare-metal side must do for itself.** Enable interrupts (`__enable_irq()`), supply a time base (the machine timer here), and poll where the RTOS would block.
5. **Make each build identify itself.** The supervisor sets bit 31 of `uptime_ms` in every acknowledgement on the bare-metal build, and the host reads it (`pfdaq_cap -L 0` prints the `context`). Have your test suite print that as its first line, as `INFO u54_4_context=…`, so a record always says which build produced it.
6. **Build the second variant outside the main build.** `firmware/build_payload.sh [outdir] [bm|freertos]` builds the firmware for hart 4 only (it passes `MPFS_HAL_FIRST_HART=4` and `MPFS_HAL_LAST_HART=4`), wraps it with U-Boot into a payload and writes it with `SHA256SUMS`. It reads `PFDAQ_AMP_EXAMPLES`, `RISCV_TOOLCHAIN` and `PFDAQ_YOCTO_BUILD` from the environment.
7. **Switch and compare.** Write the payload to the SD card's payload partition (partition 2), reboot, and run `tests/board/pfdaq_bm_suite.sh`. Then write the image's own payload back. Run the same command lines on both builds and compare counts for equality first, then timings.

What not to change:

- **The partition number.** Check it twice before writing: a write to the wrong partition destroys the card's contents.
- **The one-source rule.** Once the two builds have their own copies of the logic, any difference in the table may come from the logic and not from the base layer.
- **The conclusions you draw from single runs.** Here the mean handler cost differs by one count of the 20 ns instrument, and the worst cases are single samples out of 100,000. They do not rank the builds.
- **The mode the bare-metal build cannot do.** A request for the task-acknowledged mode is turned into the handler mode there, by design.

## Evidence

All values: board, fabric v0.5.1, single runs.

- **Same behaviour.** The tests that both builds ran give the same results where results are counts: no errors, the same number of faults detected, one deadline miss.
- **The handler costs the same.** The mean latency of the interrupt handler differs by 0.02 µs, one count of the fabric instrument's 20 ns resolution (module P02). Both builds run the same handler function. The trap-entry code around it differs between the two builds and was not compared, so the record shows that the total cost is the same to within the instrument's resolution, not why. The two worst cases (3.10 µs and 2.10 µs at idle) are single samples out of 100,000 and do not rank the builds.
- **The task costs about 1.5 µs more.** FreeRTOS handler 0.52 µs against FreeRTOS task 2.04 µs, idle. This is the price of moving the work out of the handler into a task, with the context switch the port performs (module M07).
- **The structure costs a few hundred microseconds on the message round trip.** The FreeRTOS means are 0.3 to 0.5 ms above the bare-metal means. The cause was not investigated: it could be the scheduler, the tick, or the different receive paths. The minimum differs by only about 25 µs, so the difference is in the tail of the distribution.
- **The memory-visibility check is about 1.3 ms slower on FreeRTOS** in both regions. Also not investigated.

## When it goes wrong

| Symptom | Cause |
|---|---|
| The bare-metal suite reports the context as `freertos`, or the reverse | the payload on the SD card is not the one you think; the context is read from the supervisor's own build tag |
| No supervisor output after writing the bare-metal payload | the payload was written to the wrong partition, or the file is not the one the build produced |
| The task-acknowledged latency run fails on bare metal | not available there by design; the request is quietly turned into the handler mode |
| The board does not boot after a payload write | restore the saved payload to partition 2; the boot monitor in the chip's flash is not involved |

The rows are what the code and the procedure imply; no failure of this kind is recorded in the project's evidence.

## Trade-offs and going further

- **When bare metal is enough.** One job, one interrupt, no timeouts to juggle: here the handler is the same speed and the code is 10 KB smaller. The structure cost is real, though: the bare-metal receive path polls, so the core is never idle, while the FreeRTOS task blocks.
- **When the RTOS earns its place.** As soon as work has to be moved out of the interrupt without losing the interrupt line: the task-acknowledged mode needs a semaphore and a priority, and a port-specific detail has to be right (K06).
- **A comparison is only as fair as the common source.** Because both builds share the file, any difference in the table is from the base layer, not from differently written logic.
- **Exercise:** list from the source every `#ifdef PFDAQ_BM` and every `#ifndef PFDAQ_BM`. Which of them change timing, and which change only structure?
- **Exercise:** the bare-metal receive loop polls the queue size. What would you need to add to put the core to sleep between messages, and what would then limit the interrupt latency?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [U54 hart 4](../system/README.md#u54-4) · [Platform-level interrupt controller](../system/README.md#plic) · [Hart 4 image](../system/README.md#ddr-u54-4-image) · [pfdaq supervisor](../system/README.md#supervisor) · [Hart Software Services](../system/README.md#hss) |
| **Source** | [`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `PFDAQ_BM`, `now_ms`, `delay_ms`, `sup_recv`, `lat1_task`, `PLIC_f2m_5_IRQHandler`<br>[`firmware/u54_4-supervisor/disco-kit/Makefile.bm`](../../firmware/u54_4-supervisor/disco-kit/Makefile.bm): `DPFDAQ_BM`<br>[`firmware/u54_4-supervisor/disco-kit/Makefile.freertos`](../../firmware/u54_4-supervisor/disco-kit/Makefile.freertos): `PFDAQ_SUPERVISOR`<br>[`firmware/build_payload.sh`](../../firmware/build_payload.sh): `VAR` |
| **Evidence** | [Board suite with the bare-metal supervisor on hart 4, 8 of 8](../../evidence/v0.5.1/board/suite-baremetal.md) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Signing an HSS payload offline and verifying it with two independent implementations](../../evidence/host/secure-boot/sign-and-verify.txt) (host, 2026-11-15) |
| **Related** | [M01 Asymmetric multiprocessing on one chip](M01-asymmetric-multiprocessing.md)<br>[M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md)<br>[M07 Deferred interrupt handling, and a port that switches tasks inside the handler](M07-deferred-interrupt-handling.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md)<br>[K06 The interrupt that stayed masked after one event](../case-studies/K06-interrupt-stayed-masked.md) |
| **Status** | validated |
<!-- nav:end -->
