---
id: B06
title: "Programming safely: components, digests and what never to touch"
track: boot
level: 2
summary: What a programming job contains, how to prove what is on the device, and why the boot flash is left alone.
needs: [F05]
repro: doc
status: validated
hardware: [envm, debug-usb, host, soc]
software: [hss]
sources:
  - hw/libero/program.sh:ACTIONS,DEVICE_INFO
  - hw/libero/export_job.tcl:export_fpe_job,GENERATEPROGRAMMINGDATA
  - tools/host/99-mpfs-disco-uart.rules
evidence: [programming, timing, boot-hss]
related: [F05, S02, K01, K08, B05]
---

# Programming safely: components, digests and what never to touch

After this module you can say what is in a FlashPro Express programming job, which of its parts this project ever writes, how to prove afterwards which design is on the chip, and what the rules are for the one part that could stop the board booting.

## Before you start

- **Level 2.** You should know the three places the chip's software comes from: the fabric design, the boot monitor in the chip's own flash, and the SD card (modules F05 and B02).
- **Tools:** none to read it. To repeat the programming you need FlashPro Express (part of Libero SoC) and the board's USB debug connection.

## The idea

A PolarFire SoC is programmed in **components**. A programming job is a file that can hold several, and the programmer writes the ones you select:

| Component | What it holds | Written by this project |
|---|---|---|
| FABRIC | the FPGA configuration: all the logic | yes |
| SNVM | the small secure non-volatile memory that the design can carry | yes, as part of the same job |
| ENVM | the embedded non-volatile memory that holds the boot monitor (HSS) | **never** |

The boot monitor is the first software the chip runs. It trains the DDR memory, finds the SD card, loads the payload and starts every core. If it were damaged or replaced with something unsuitable, the board could no longer start from the SD card, and the normal way to fix a bad SD card (writing a new one) would not help. That is the project's reasoning for the rule on this page; it was never tested by trying, and the project's decision log records only that the step was avoided because it "could stop the board booting".

## On this board

### What the project's jobs contain

The job is exported with one line, in `hw/libero/export_job.tcl`:

```tcl
export_fpe_job $::env(PFDAQ_JOB_NAME) $::env(PFDAQ_JOB_DIR) "FABRIC SNVM" 0
```

The quoted list is the components in the job. `ENVM` is not in it. The job file carries only the listed components, so selecting the boot flash later in the programmer has nothing to act on. That is the intent; it was not tested by trying.

### Programming and checking

`hw/libero/program.sh` runs three actions in order against the device named `MPFS095T`:

| Action | What it does |
|---|---|
| `PROGRAM` | writes the selected components |
| `VERIFY` | reads them back and compares them with the job |
| `DEVICE_INFO` | reads the device's own report of what it holds: the design name, a checksum, the design version and digests of each component |

Linux must be shut down on the board first, because the fabric is about to change under it. The on-board programmer shows up as several USB interfaces; the **JTAG** one must not be claimed by the serial driver, otherwise the programmer cannot open it. The project's udev rule (`tools/host/99-mpfs-disco-uart.rules`) exposes the UART interfaces and deliberately leaves JTAG alone.

### Proving what is on the device

The record of the v0.5.1 programming (`programming`) shows what the read-back looks like:

```text
Design Name: MPFS_DISCOVERY_KIT
EXPORT CHECKSUM[16] = d78d
EXPORT DESIGNVER[16] = 0006
EXPORT ENVM digest[256] = 55b852781b9995a44c939b64e441ae2724b96f99c8f4fb9a141cfc9842c4b0e3
EXPORT Fabric component bitstream digest[256] = a138b144d6dd5ee3c4552c82efe92c173552b1f693136f717bd806b9dcff6504
EXPORT sNVM component bitstream digest[256] = b0a9e2be29f47fa951a6a131a8dd9a9016e5ba87de5833c742b7f654428ea6e7
Executing action DEVICE_INFO PASSED.
Executing action PROGRAM PASSED.
Executing action VERIFY PASSED.
```

Read it as a set of claims you can check:

- **`DESIGNVER` is the design version you passed to the build.** The fabric build takes `DESIGN_VERSION:6`, and the device reports `0006`. This is how you tell two builds of the same design apart.
- **The component digests identify the content.** Two programmings of the same job give the same fabric digest; a different build gives a different one. The fabric digest was also the way the project showed that a rebuild from a new folder layout produced the identical fabric.
- **The `ENVM` digest is the one to watch.** It is the digest of the boot flash, which the job does not touch. The project's notes record that it was identical before and after every programming, eight in all. A changed value there would mean the boot flash had changed.

## The rules

| Rule | Why |
|---|---|
| Never program the ENVM and never use a boot-mode programming command | the boot monitor in the chip is the only way to start from the SD card |
| Never program user keys, passcodes or security locks | the pages read for this project do not say which can be undone (see module S02) |
| Export every job with the component list `FABRIC SNVM`, so ENVM cannot be selected | the safe choice is made once, when the job is made, not every time it is used |
| Run `PROGRAM`, `VERIFY` and `DEVICE_INFO` together and compare the ENVM digest with the previous one | a result you did not look at is not a check |
| Keep the previous job file | it is the way back |

## What you should see

The output above, in full. Two `PASSED` lines for `PROGRAM` and `VERIFY` mean the device now holds exactly what the job holds. A digest line you do not recognise is the signal to stop and compare.

## Evidence

- **Programming v0.5.1:** `PROGRAM`, `VERIFY` and `DEVICE_INFO` passed, with the checksum, design version and component digests above (record `programming`). The boot monitor that is in the chip's flash is the one named in the boot record: version 0.99.53-v2026.04.1 (record `boot-hss`).
- **Repeated:** the project's notes record eight programmings, each with PROGRAM and VERIFY passing and the ENVM digest unchanged. Only the last has a published record.
- **Recovery:** after a build that met timing but left the board stuck early in the boot monitor (case study K08), programming the previous job over JTAG brought the board back. That worked once, in the one case it was needed. The expectation that it can always be done rests on the programmer talking to the chip's JTAG interface, which does not depend on anything the fabric or the processors are running. That is a reason, not a test: no one ever tried to break it.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| The programmer cannot find the device | the JTAG USB interface is claimed by the serial driver, or Linux on the board still has the fabric in use |
| `VERIFY` fails | the job and the device differ: program again, and look at the cable and the board's power |
| The device report shows an integrity warning | on this board a "Device Integrity Status: FAIL" was already reported before the first programming of this project, with all integrity bits zero. It is not understood. Treat it as the baseline, not as a result of your job. |
| The device report names a design you did not expect | you are looking at the delivered design (case study K01), or at an earlier job |

## Trade-offs and going further

- **A smaller job is a smaller risk.** Leaving ENVM out costs nothing the application needs. Fixing the open limitation in the boot monitor (case study K11) would need exactly the component this page keeps out of reach, which is why it was not done.
- **Digests are cheap evidence.** They cost one line of output and make "what is on the board?" a question with an answer.
- **Exercise:** from a `DEVICE_INFO` output, write down the three values you would compare after a programming, and what each change would mean.
- **Exercise:** why is it safer to choose the component list when the job is made than when it is programmed?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [F05 The tool flow in batch: Libero, the IP vault and FlashPro Express](F05-tool-flow-in-batch.md) |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [eNVM boot flash](../system/README.md#envm) · [FlashPro5 on USB-C](../system/README.md#debug-usb) · [Development PC](../system/README.md#host) · [MPFS095T system-on-chip](../system/README.md#soc) · [Hart Software Services](../system/README.md#hss) |
| **Source** | [`hw/libero/program.sh`](../../hw/libero/program.sh): `ACTIONS`, `DEVICE_INFO`<br>[`hw/libero/export_job.tcl`](../../hw/libero/export_job.tcl): `export_fpe_job`, `GENERATEPROGRAMMINGDATA`<br>[`tools/host/99-mpfs-disco-uart.rules`](../../tools/host/99-mpfs-disco-uart.rules) |
| **Evidence** | [Programming, verify and device read-back of the v0.5.1 fabric](../../evidence/v0.5.1/build/programming.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [F05 The tool flow in batch: Libero, the IP vault and FlashPro Express](F05-tool-flow-in-batch.md)<br>[S02 Secure boot on PolarFire SoC: the chain of trust, and what we left alone](S02-secure-boot.md)<br>[K01 The fabric on the board was not the reference design](../case-studies/K01-delivered-fabric.md)<br>[K08 Timing met, board dead: the missing pin constraints](../case-studies/K08-timing-met-board-dead.md)<br>[B05 Updating a running board over SSH](B05-update-running-board.md) |
| **Status** | validated |
<!-- nav:end -->
