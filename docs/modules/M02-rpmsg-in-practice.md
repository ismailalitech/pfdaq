---
id: M02
title: "RPMsg in practice: announce, bind, ping"
track: multicore
level: 3
summary: The smallest working message exchange between Linux and the real-time hart.
needs: [M01]
repro: base
status: validated
hardware: [u54-linux, u54-4, ihc, ddr-rpmsg]
software: [remoteproc-rpmsg, pfdaq-cap, supervisor]
sources:
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:sup_open,sup_xfer,sup_msg,sup_ack
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:pfdaq_supervisor,rpmsg_ns_announce,fabric_snapshot
evidence: [boot-uboot-linux, supervisor-console, suite-freertos, suite-freertos-raw, reboot-30, soak-v030]
related: [M01, M03, M04, F03, K11, K12]
---

# RPMsg in practice: announce, bind, ping

After this module you can follow one message from a Linux program to the real-time core and back: how the real-time side makes itself known, how Linux finds it and opens it, what a message looks like on the wire, and what a round trip costs. The module describes the exchange the project really uses. A minimal "ping" program is described at the end as an exercise; it does not exist in the repository yet.

## Before you start

- **Level 3.** Module M01 explains why the two systems are separate.
- **Board:** the shared baseline, fabric v0.5.1 with the pfdaq image. The commands below are the ones the project ran; their results are in the evidence records, and this module was written while the board was not available, so nothing here was run again for it.
- **Tools:** an SSH session to the board, root.

## The idea

**RPMsg** is a small message protocol for processors that share memory. It is built from three parts:

1. **Shared memory** holding two rings, one for each direction, and a pool of fixed-size buffers. A sender takes a buffer, fills it and puts its number on a ring.
2. **A notification** that tells the other side to look: here, an interrupt through the Mi-V inter-hart communication block. Nothing is copied between the processors except the buffer's contents, and nothing is polled.
3. **A name service.** One side *announces* a named channel; the other side learns that the name exists and creates a matching endpoint. After that, each message carries a source and a destination address, like a very small network.

Linux implements it on top of virtio, the same framework used for virtual machines. The real-time side uses RPMsg-Lite, a small implementation for firmware.

## On this board

The shared memory is reserved in the device tree (module M01): two 32 KiB rings at `0x91D0_0000` and `0x91D0_8000`, a 256 KiB buffer pool at `0x91D1_0000`, and a resource table at `0x91D5_0000` that describes them.

### Announce: the supervisor makes itself known

When the supervisor starts, it creates an endpoint at address 0 and announces it under a name. These are the two calls in `pfdaq_supervisor.c`:

```c
rp->ctrl_ept = rpmsg_lite_create_ept(rp->my_rpmsg, PFDAQ_SUP_EPT_ADDR, rpmsg_queue_rx_cb, rp->ctrl_q);
rpmsg_ns_announce(rp->my_rpmsg, rp->ctrl_ept, PFDAQ_SUP_CHANNEL, RL_NS_CREATE);
```

The name is `pfdaq-supervisor`. The supervisor prints that it has done so, and Linux, once it has attached to the running core (module M01), prints that the channel has appeared:

```text
[pfdaq-sup] channel "pfdaq-supervisor" announced, state IDLE          (hart 4 console)
[    1.938361] virtio_rpmsg_bus virtio0: creating channel pfdaq-supervisor addr 0x0   (Linux console)
```

The supervisor is already running when Linux boots, but it announces the channel only after Linux has attached and reported the link up. The project's reboot test therefore waits three seconds after SSH comes up before it looks for the channel.

### Bind: pfdaq_cap opens the channel

A channel on its own is only a device in sysfs, `/sys/bus/rpmsg/devices/<name>`. A user program needs a character device. `sup_open()` in `pfdaq_cap.c` gets one in three steps:

1. **Find the device** whose name contains `pfdaq-supervisor`, and take the number after the last dot as the destination address.
2. **Bind it to the generic character driver**:

   ```c
   write_str(path, "rpmsg_chrdev");                                      /* .../driver_override */
   write_str("/sys/bus/rpmsg/drivers/rpmsg_chrdev/bind", dev);   /* fails harmlessly if already bound */
   ```

3. **Open the device node.** On this kernel (Linux 6.18), binding the channel to `rpmsg_chrdev` creates `/dev/rpmsgN` directly; `sup_open()` polls for it for up to a second. Older flows, such as the vendor's ping-pong example, instead expose an `rpmsg_ctrl` device on which an endpoint has to be created first; `sup_open()` handles both. The project found that the vendor flow did not match this kernel, which is why both exist.

### One message, one answer

The two sides share two fixed-size structures, little-endian 32-bit words, defined once in each source file:

| Linux to supervisor (`struct sup_msg`, 7 words) | Meaning |
|---|---|
| `magic` | `0x53444650`, the bytes "PFDS" |
| `type` | ARM 1, STATUS 2, DISARM 3, QUERY 4, LATMODE 5, MEMCHK 6 |
| `burst`, `next_seq`, `errors` | what Linux has verified so far (for MEMCHK: address, word count, seed) |
| `deadline_ms`, `max_lag` | ARM: the deadline, and how far the fabric may run ahead |

| Supervisor to Linux (`struct sup_ack`, 9 words) | Meaning |
|---|---|
| `magic` | `0x41444650`, "PFDA" |
| `type`, `state` | the type being answered; IDLE 0, ARMED 1, ALARM 2 |
| `status_rx`, `deadline_miss`, `seq_mismatch` | the supervisor's counters |
| `fabric_seq`, `fabric_frames`, `uptime_ms` | what the supervisor read from the fabric, and its uptime (bit 31 set means the bare-metal build) |

Every message is answered by exactly one acknowledgement. Linux sends one message and waits for the answer, with a one-second limit, in `sup_xfer()`:

```c
if (write(sup_fd, &m, sizeof(m)) != sizeof(m) || poll(&p, 1, 1000) <= 0 ||
    read(sup_fd, ack, sizeof(*ack)) != sizeof(*ack) || ack->magic != SUP_ACK_MAGIC || ack->type != type) {
	sup_fail++;
	return -1;
}
```

The reply is accepted only if both words, the magic and the echoed type, match. A reply that does not match counts as a failure, not as an answer to a different question.

### What a round trip includes

`sup_xfer()` times the call from just before the write to just after the read, so a round trip includes two system calls, the transfer through the ring in each direction, the wake-up of the supervisor (a task under FreeRTOS, a polling loop on bare metal) and the wake-up of the Linux process.

It also includes something else that is easy to miss. On every message, before it looks at the type, the supervisor reads the fabric's counters, and that read starts with a deliberate wait:

```c
SRC_REG(SRC_CTRL) = SRC_CTRL_SNAPSHOT;
delay_ms(1);
```

The comment in the source says the fabric needs only a few bus clocks, so one millisecond is a generous margin. The consequence is that no round trip can be shorter than about 1 ms, and the measured minimum agrees:

| Run | Messages | Min | Mean | Max |
|---|---|---|---|---|
| Soak, fabric v0.3.0, FreeRTOS | 17,000 | 1,223 µs | 1,572 µs | 10,010 µs |
| Suite `T4-counter`, v0.5.1, FreeRTOS | 32 | 1,249 µs | 1,974 µs | 10,020 µs |
| Suite `T4-counter`, v0.5.1, bare metal | 32 | 1,222 µs | 1,658 µs | 9,431 µs |

Subtracting the wait, at most about 0.22 ms of the best round trip is the transport and the two wake-ups. That is arithmetic on these records, not a separate measurement. The mean figures are a few hundred microseconds above the minimum, and every run has a worst case near 10 ms. What causes the 10 ms was not investigated.

## Run it

These are the commands the project ran.

1. On the board, check that the channel exists (this is the check in the 30-boot reboot test):

   ```sh
   ls /sys/bus/rpmsg/devices/ | grep pfdaq-supervisor
   ```

2. Run a supervised capture, which opens the channel, sends ARM, one STATUS per burst and DISARM:

   ```sh
   pfdaq_cap -r -w 252 -f 1024 -n 8
   ```

3. Watch the hart 4 console (MMUART0) while it runs.

To read the recorded results instead of running anything:

```sh
grep -n 'announced\|ARMED\|DISARM' evidence/v0.5.1/board/supervisor-console.txt | head -n 4
```

## What you should see

Step 1 prints one line, the channel's device name. In step 2, the capture's own JSON summary ends with the supervisor's view. This is the object the suite recorded for its first supervised test:

```json
"supervisor": {"pass": true, "status_rx": 32, "deadline_miss": 0, "seq_mismatch": 0,
               "fabric_seq": 32768, "fabric_frames": 32768, "xfer_fail": 0, "uptime_ms": 52846,
               "rtt_us": {"min": 1249, "mean": 1974, "max": 10020}}
```

Thirty-two bursts, thirty-two STATUS messages received, none lost (`xfer_fail` 0). The hart 4 console shows the same exchange from the other end:

```text
[pfdaq-sup] ARMED: deadline 500 ms, max lag 1040 frames, fabric seq 0
[pfdaq-sup] DISARM from ARMED: 32 status, 0 deadline misses, 0 seq mismatches, linux errors 0, latency irqs 0
```

## Evidence

- **The channel appears every time.** In the 30-cycle reboot test, the supervisor channel was present after each warm reboot and a supervised capture passed each time: 30 of 30 (`reboot-30`).
- **Every supervised test passed.** All 18 supervised rows of the FreeRTOS board suite report "supervisor ok" (`suite-freertos`), and the raw record carries the counters quoted above (`suite-freertos-raw`).
- **Volume.** The ten-minute soak on fabric v0.3.0 sent 17,000 STATUS messages with no failure (`soak-v030`).
- **When the core is stopped the channel goes.** After `remoteproc` stops hart 4 the suite finds `supervisor_channel=0`; the restart then fails (K11).

## When it goes wrong

| Symptom | Cause |
|---|---|
| `supervisor RPMsg endpoint: No such device` | the channel did not appear: hart 4 is not running or has not announced yet; once in about sixty boots it did not start (K12) |
| `supervisor did not acknowledge ARM` | the channel opened but no answer came within one second: the supervisor is stuck or the link is down |
| `/dev/rpmsgN` never appears after the bind | the kernel exposes the older `rpmsg_ctrl` flow: `sup_open()` handles it, other tools may not (this was met on the board when the vendor example was tried on Linux 6.18) |
| The channel is gone after `remoteproc` stop | expected; restart is not possible with this boot monitor (K11) |

The first two rows are the code's own error messages, which follow from the symptoms; the project met the vendor-flow mismatch and the stop behaviour on the board.

## Trade-offs and going further

- **Request and acknowledge, one at a time.** It is simple, and the acknowledgement doubles as proof of life. It also means Linux's reporting loop waits for the supervisor on every burst, so the 1 ms wait adds to every burst's reporting time. At the burst rates of the board suite that is small; at much higher message rates it would matter.
- **Fixed-size structures with a magic word** are the whole protocol. There is no version field. A change to either structure must be made in both source files at once, and nothing checks that they agree.
- **The supervisor sends with a blocking call.** Read from the code, a reply waits for a free buffer if the ring is full; that was not provoked in testing.
- **Planned exercise: a ping.** The simplest useful program is not in the repository yet: open the channel as `sup_open()` does, send a QUERY (type 4), and print the acknowledgement and the round-trip time. About sixty lines. It would show the round trip with no capture running. Since the supervisor answers a QUERY after its normal snapshot wait, it would still be at least 1 ms.
- **Exercise:** take the supervisor's `delay_ms(1)` out in a scratch copy and predict the new minimum from the table before you rebuild. What else in the supervisor's design could break?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [M01 Asymmetric multiprocessing on one chip](M01-asymmetric-multiprocessing.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [Mi-V inter-hart communication](../system/README.md#ihc) · [RPMsg shared memory](../system/README.md#ddr-rpmsg) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Source** | [`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `sup_open`, `sup_xfer`, `sup_msg`, `sup_ack`<br>[`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `pfdaq_supervisor`, `rpmsg_ns_announce`, `fabric_snapshot` |
| **Evidence** | [Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Supervisor console during the fresh-card suite, with the alarm and recovery messages](../../evidence/v0.5.1/board/supervisor-console.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Thirty warm reboots, both systems up and a supervised capture passing each time](../../evidence/v0.5.1/board/reboot-30.csv) (board, fabric v0.5.1, 2026-11-15)<br>[Ten-minute supervised soak, 17.8 GB, no errors](../../evidence/v0.3.0/board/soak-10min.json) (board, fabric v0.3.0, 2026-11-15) |
| **Related** | [M01 Asymmetric multiprocessing on one chip](M01-asymmetric-multiprocessing.md)<br>[M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md)<br>[M04 Same core, same code: FreeRTOS against bare metal](M04-freertos-vs-bare-metal.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md)<br>[K11 remoteproc cannot restart the hart](../case-studies/K11-remoteproc-restart.md)<br>[K12 One boot in sixty, FreeRTOS never started](../case-studies/K12-freertos-no-start.md) |
| **Status** | validated |
<!-- nav:end -->
