---
id: I02
title: "Fabric interrupts in user space, and the double wake-up"
track: integration
level: 3
summary: Waiting for a fabric interrupt with a blocking read, and why each event arrived twice.
needs: [I01, F03]
repro: base
status: validated
hardware: [pfdaq-latency, plic, u54-linux]
software: [pfdaq-lat, uio]
sources:
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c:open_uio,empty_wakeups,L_STATUS,L_ACK
  - yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts
evidence: [suite-freertos-raw, suite-freertos, boot-uboot-linux]
related: [F03, I03, K05, P02, H07]
---

# Fabric interrupts in user space, and the double wake-up

After this module you can write a Linux program that sleeps until a signal in the FPGA fires, answers it, and counts the interrupts it was woken for without anything to do. You will know what the generic UIO driver hands you, in what order the program has to do things, and why the answer to "how many wake-ups per event?" was two on this board.

The story of how the double wake-up was found and explained is a case study, [K05](../case-studies/K05-two-interrupts-per-event.md). This page is the other half: the program that has to live with it.

## Before you start

- **Level 3.** You should know how a fabric line becomes a PLIC source (module [F03](F03-fabric-interrupts.md)) and how a user program reaches fabric registers (module I01, planned).
- **Board:** the shared baseline, fabric v0.5.1 with the pfdaq image. Everything on this page about the board is quoted from the recorded suite; it was not run again while writing.

## The idea

A device that sits outside the kernel's usual drivers can still be reached from user space through **UIO**, the userspace I/O framework. A UIO device gives a program two things:

| What | How the program sees it |
|---|---|
| The device's registers | a page of memory it maps with `mmap` |
| The device's interrupt | a file descriptor: `poll` or a blocking `read` returns when the interrupt has fired |

The interrupt contract, as the generic driver implements it and as this project's own driver copies it in its `kernel_ack=0` mode (see [I03](I03-kernel-driver-fabric-interrupt.md)):

1. The kernel's interrupt handler **masks** the line and wakes whoever waits on the file.
2. `read()` returns a 4-byte count of the interrupts seen so far.
3. The program does whatever the device needs to drop its interrupt, then **writes a 4-byte `1`** to the file to unmask the line again.

The kernel cannot do step 3's first half for you: it has no idea which register of an arbitrary device acknowledges the interrupt. That is the point of UIO, and it is also where the trouble starts, because there is a gap between "the program has acknowledged" and "the kernel has unmasked".

## On this board

### The device

The device-tree node that creates the UIO device is in the project's tree:

```dts
pfdaq_src: uio@40000f00 {
    compatible = "generic-uio";
    linux,uio-name = "pfdaq_stream_src";
    reg = <0x0 0x40000f00 0x0 0x100>;
    interrupt-parent = <&plic>;
    interrupts = <119>;
};
```

`compatible = "generic-uio"` only becomes a driver binding because the kernel command line names it: the recorded boot log shows `uio_pdrv_genirq.of_id=generic-uio`. The interrupt is PLIC source 119, fabric line F2M[1] (module F03). The register window is the 256 bytes of the project's APB slot, which sits inside the page at `0x4000_0000`; the tools therefore map one page and add the offset themselves (`SLOT_OFF 0xF00`).

Finding the device does not depend on its number. The tool searches by the name that the node gives it:

```c
snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
...
if (strcmp(val, name)) continue;
snprintf(path, sizeof(path), "/dev/%s", e->d_name);
```

### The interrupt source

The interrupt comes from channel 0 of the fabric's latency unit (module [H07](H07-latency-unit.md)). It is a **level**: the line stays high until software writes the channel's `ACK` register. `STATUS` bit 0 says whether the interrupt is pending. Those two facts decide the whole shape of the program below.

### The program

`pfdaq_lat -c 0` waits for the interrupt in user space. The relevant loop, shortened:

```c
write(fd, &one, 4);                          /* enable the interrupt line */
while (poll(&p, 1, 0) > 0 && read(fd, &cnt, 4) == 4)   /* drop old events */
    write(fd, &one, 4);
lat[L_CTRL / 4] = (jitter << 16) | 1;        /* start the fabric timer */
for (;;) {
    if (poll(&p, 1, timeout_ms) <= 0) { ... }
    if (read(fd, &cnt, 4) != 4) break;
    if (lat[L_STATUS / 4] & 1)
        lat[L_ACK / 4] = 1;                  /* stops the fabric timer */
    else
        empty_wakeups++;
    write(fd, &one, 4);                      /* re-enable the interrupt line */
    if (!(lat[L_STATUS / 4] & 3)) break;     /* run finished */
}
```

In words:

| Step | Why |
|---|---|
| enable the line, then drain old events | an earlier program may have left an event count behind |
| start the run | the fabric now raises one interrupt per period |
| `poll` with a timeout | a lost interrupt must not hang the program forever |
| `read` | returns when the kernel handler has run |
| **read `STATUS`; acknowledge only if bit 0 is set** | the rule this page is about |
| `write(1)` | unmask the line so the next event can arrive |

## Run it

The commands below are the ones the project ran, as lines of the board suite (`tests/board/pfdaq_final_suite.sh`). They were not run while this page was written; the results are the recorded ones.

```sh
pfdaq_lat -c 0 -p 500 -n 100000            # user space, default scheduling, idle
pfdaq_lat -c 0 -p 500 -n 100000 -R 80      # the same with SCHED_FIFO 80 and locked memory
```

`-p` is the event period in microseconds, `-n` the number of events, `-R` a real-time priority. The tool prints one line of JSON.

## What you should see

From the recorded run (fabric v0.5.1, 100,000 events each, one run):

| Run | `events` | `acks` | `empty_wakeups` | `spurious` | `overrun` |
|---|---|---|---|---|---|
| user space, default priority, idle | 100,000 | 100,000 | 99,967 | 0 | 89 |
| user space, `SCHED_FIFO` 80, idle | 100,000 | 100,000 | 99,977 | 0 | 7 |
| user space, `SCHED_FIFO` 80, loaded | 100,000 | 100,000 | 99,955 | 0 | 18 |

Read it like this:

- **`acks` equals `events`:** every event was answered, so nothing was lost.
- **`empty_wakeups` is about 99.97 % of `events`:** for almost every event the program was woken a second time with nothing pending. Those wake-ups cost CPU time and nothing else, because the program looked at `STATUS` before touching `ACK`.
- **`spurious` is 0:** the fabric never saw an `ACK` with nothing pending, which is exactly what the `STATUS` check prevents. Without the check, the second wake-up of each event would have been an unnecessary write to a register.
- **`overrun`** counts events that became due while the previous one was still unanswered. It is small, and it is the other side of the latency story: module [P02](P02-interrupt-latency.md) shows the same runs as latency figures.

For comparison, the rows in which Linux's generic UIO path is not involved all show `empty_wakeups` of 0: the in-kernel acknowledge of module I03, and the four runs in which hart 4 answers.

## Evidence

- **Board, fabric v0.5.1:** the table above comes from the [raw suite record](../../evidence/v0.5.1/board/suite-freertos.jsonl) (rows `T8-linux-uio-idle`, `T8-linux-uio-fifo-idle`, `T8-linux-uio-fifo-load`), one run each.
- **Kernel command line:** the boot record shows the `uio_pdrv_genirq.of_id=generic-uio` that binds the node.

The explanation of *why* the second wake-up happens is the one recorded when it was found, summarised in K05 and F03: the level is latched again by the interrupt controller when the kernel handler completes, and delivered once the line is unmasked, although the program has acknowledged by then. The PLIC was not traced to confirm it. What is established is the count, and that the `STATUS` check makes the program correct regardless of the cause.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| `read()` never returns | the line was never unmasked: the program forgot the `write(1)`, or an earlier program died while the line was masked |
| Each event is counted twice by a program that does not check `STATUS` | the double wake-up above |
| A stale event right at the start | an earlier run left an event count; drain with `poll` and `read` before starting, as the tool does |
| `open` of the UIO device fails with `ENODEV` | the device-tree node is missing or the kernel was not told `of_id=generic-uio` |
| Latency is dominated by the scheduler | user-space response is a different order of magnitude from the kernel's; see P02 |

## Trade-offs and going further

- **User space is the easy place to write the handler and the slow place to run it.** The recorded mean is 78.31 µs for the default-priority run, with stalls to 5.13 ms; the in-kernel handler of I03 has a mean of 4.41 µs. A program that must answer in microseconds does not belong here.
- **Check the device, not the wake-up.** The program trusts `STATUS`, not the number of times it was woken. That is the portable rule for any level interrupt shared with a layer that can wake you for stale reasons.
- **Real-time priority helps the tail, not the mean.** `SCHED_FIFO` 80 with locked memory changed the mean from 78.31 to 76.10 µs and the worst case from 5.13 ms to 1.67 ms.
- **Exercise (needs the board):** remove the `STATUS` test so the program writes `ACK` on every wake-up, and predict what `spurious` will count before you run it.
- **Exercise:** the tool writes `1` to unmask the line only after acknowledging. What would go wrong if it unmasked first?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [I01 Reaching fabric registers from Linux](I01-fabric-registers-from-linux.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [Latency unit (two channels)](../system/README.md#pfdaq-latency) · [Platform-level interrupt controller](../system/README.md#plic) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [pfdaq_lat](../system/README.md#pfdaq-lat) · [UIO devices](../system/README.md#uio) |
| **Source** | [`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_lat.c): `open_uio`, `empty_wakeups`, `L_STATUS`, `L_ACK`<br>[`yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts`](../../yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts) |
| **Evidence** | [Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md)<br>[I03 A small kernel driver for a fabric interrupt](I03-kernel-driver-fabric-interrupt.md)<br>[K05 Two interrupts for every event](../case-studies/K05-two-interrupts-per-event.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md)<br>[H07 Measuring software from hardware: the latency unit](H07-latency-unit.md) |
| **Status** | validated |
<!-- nav:end -->
