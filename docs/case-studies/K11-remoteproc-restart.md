---
id: K11
title: "remoteproc cannot restart the hart"
track: case-study
level: 4
summary: "An open limitation in the boot monitor, traced to one address check."
needs: [M01]
repro: doc
status: validated
hardware: [e51, u54-4]
software: [hss, remoteproc-rpmsg]
sources:
  - tests/board/pfdaq_final_suite.sh
evidence: [suite-freertos]
related: [M06, M01, B02]
---

# remoteproc cannot restart the hart

Linux can stop the real-time hart through remoteproc. It cannot start it again: the boot monitor refuses the request and logs "rproc buffer not in DDR". This page follows the refusal from the board's test, through the Linux driver, to the one check in the monitor's source that produces it. It also says what that source does not tell us, and why the limitation is documented here instead of fixed.

## Symptom

The board suite has a test, `T9-remoteproc`, that stops the supervisor hart and starts it again. It is the one functional test in the suite that does not pass; the suite lists it under known issues. These are the steps it takes, from `tests/board/pfdaq_final_suite.sh`:

```sh
echo stop > $R/state ...
echo start > $R/state ...
```

with the firmware name set in between. The record of one run (`suite-freertos`) reads:

| Field | Value |
|---|---|
| State before | `attached` |
| State after `stop` | `offline` |
| Firmware name set | `polarfire-soc-freertos-amp-demo.elf` |
| State after `start` | `offline` |
| RPMsg channel `pfdaq-supervisor` present | no |
| Capture afterwards | not possible, no supervisor |

Stop works. Start leaves the hart off, and nothing on the Linux side says why. The reason appears only on the boot monitor's console, on its own serial port, once for each attempt:

```
HSS_Boot_IPIHandler: rproc buffer not in DDR
```

That line is in the boot monitor's console captures of the cold-boot runs, including the run on the freshly written SD card. The captures are not published; they are recorded in the project's notes.

The board needs a reboot to get the supervisor back.

## Isolation

Following one `start` request through the layers:

1. **Linux.** The remoteproc driver for this chip (`mchp_ipc_sbi_remoteproc.c` in the Microchip kernel) implements `start` as one call into the firmware layer under it, an SBI call with the channel number and the boot address. It does not allocate or pass a buffer. If the call returns an error, the driver returns it; a success is followed by a five-second wait for the remote side to announce itself.
2. **The firmware layer in the boot monitor.** OpenSBI inside the monitor receives the call. For the configuration this board's monitor uses (`CONFIG_SERVICE_OPENSBI_RPROC_IPC` is set in the board's default configuration of release v2026.04.1), the handler `sbi_ecall_rproc_ipc_handler` in `services/opensbi/opensbi_rproc_ipc_ecall.c` fills in a small record, `rproc_data`, with the target hart, and sends a boot request to the E51 hart, the one that runs the boot service. The request carries the address of `rproc_data`.
3. **The boot service.** The handler `HSS_Boot_IPIHandler` in `services/boot/hss_boot_service.c` receives it. Before it reads the record it checks that the address, and the address plus the record's size, lie in DDR. If not, it logs the message above and returns failure. The check calls `HSS_DDR_IsAddrInDDR` in `services/ddr/ddr_api.c`, which accepts only the two DDR address ranges.

The stop request takes a different route. The same firmware-layer handler sends a shutdown message to the remote context directly over the inter-hart mailbox and never reaches the boot service. That is why stop works while start does not.

## What is known and what is not

**Established**

- The message comes from the check in `HSS_Boot_IPIHandler`, and the request is refused there: the hart is never restarted. The text is in the monitor's source, in the same version (v2026.04.1) that the board reports.
- The record whose address is checked is declared `static struct RemoteProcMsg rproc_data;` inside the firmware-layer handler. So the address is that of a variable inside the monitor, not of a buffer that Linux supplies.
- `T9-remoteproc` fails the same way on every run.

**Inferred, not verified**

- The check is probably failing because `rproc_data` does not live in DDR. The monitor's own data may well sit in on-chip memory, and the check accepts only DDR. This follows from reading the source. It has not been checked against the memory map of the monitor binary in the board's boot flash, and the address that was actually passed is not printed.
- The design intent seems to be that the buffer for this request lives in DDR, so that all harts can see it. How the monitor's authors expected `rproc_data` to get there is not clear from the source.

**Not known**

- What the Linux driver reports after a refused start. It waits up to five seconds for the remote side to announce itself, so a timeout is expected, but the suite does not record it.
- Whether another build of the monitor, or another Linux driver version, restarts correctly on this board.
- Whether the same request works on a different Microchip board, such as the Icicle Kit. The path was only tried here.

## Containment

The fix is in the boot monitor, which lives in the chip's internal boot flash. The project never reprograms that flash. The monitor on the board is the one that came on it, and a bad write there is the one mistake that could leave the board unable to boot. That decision is recorded as D35 in the project's decision log: restart is not needed for the application, so the risk is not taken.

Normal operation does not depend on restart, for this reason: the supervisor hart is started by the boot monitor at boot, from the payload, before Linux runs. Linux does not start it; it only attaches to it afterwards. A restart would be useful only to reload new firmware without rebooting, and that is done by reboot instead. The suite keeps `T9-remoteproc` as a known issue so that the limitation stays visible. It runs near the end, because it leaves the supervisor stopped; the one test after it, the odd-length case of K10, does not use the supervisor.

## How to recognise it

| What you see | What it tells you |
|---|---|
| After `echo start > /sys/class/remoteproc/remoteproc0/state` the state stays `offline` | The request did not restart the hart. The suite discards the command's error output, so whether the write itself reported an error was not recorded |
| The boot monitor's console prints `rproc buffer not in DDR` | The check described above; the hart was not restarted |
| The supervisor's RPMsg channel never reappears after a stop | The hart is off; reboot to recover |
| `stop` works and returns `offline` | Expected; the stop path is separate |

Two habits carry over from this case. First, a driver reporting success is not proof that the other side acted: check the state after the call, as the test does. Second, a failure message can live on a console you are not watching: the boot monitor has its own serial port, and it is the first place to look when Linux says nothing.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [M01 Asymmetric multiprocessing on one chip](../modules/M01-asymmetric-multiprocessing.md) |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [E51 (hart 0)](../system/README.md#e51) · [U54 hart 4](../system/README.md#u54-4) · [Hart Software Services](../system/README.md#hss) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) |
| **Source** | [`tests/board/pfdaq_final_suite.sh`](../../tests/board/pfdaq_final_suite.sh) |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [M06 remoteproc: attach, stop, and why restart fails here](../modules/M06-remoteproc-attach-stop.md)<br>[M01 Asymmetric multiprocessing on one chip](../modules/M01-asymmetric-multiprocessing.md)<br>[B02 The HSS payload: starting two operating systems](../modules/B02-hss-payload.md) |
| **Status** | validated |
<!-- nav:end -->
