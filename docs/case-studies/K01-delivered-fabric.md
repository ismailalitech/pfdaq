---
id: K01
title: "The fabric on the board was not the reference design"
track: case-study
level: 2
summary: A delivered board whose programmed design did not match the published one, and how that showed.
repro: doc
status: validated-old
hardware: [fabric, mss-uarts, envm, debug-usb]
sources:
  - hw/libero/program.sh:DEVICE_INFO
evidence: [programming]
related: [B06, F05, K08]
---

# The fabric on the board was not the reference design

The Discovery Kit boots Linux out of the box, which makes it easy to assume that the logic inside the FPGA is the published reference design. On this board it was not. This page records how that was noticed, how it was proven, what was done about it, and what can no longer be recovered.

## Symptom

The board was first powered with the vendor's current Linux image (the 2026.04 release) and the Hart Software Services (HSS) already in its boot flash. Linux booted and all four application cores came up. Three things were wrong:

- **Eight kernel warnings at every boot**, all of the form `ccc@38400000_pll0_out*: Zero divisor`, raised by the Linux clock-divider code. The device tree of the 2026.04 image describes the fabric's clock-conditioning circuit (CCC); something read back from the fabric did not match it.
- **An Ethernet PHY message with error `-22`** in the boot log.
- **No output on the third USB serial port.** The real-time core's console is on MMUART0, and the reference design routes MMUART0 through the fabric to that port. On this board MMUART0 was configured and transmitting (its line-status register looked correct), and nothing arrived.

None of these stopped Linux. They stopped the project's plan, which needed the third console for the real-time core and a fabric whose clocks and interrupts were known.

## Isolation

The suspect was the fabric, because everything else was pinned: the SD card had just been overwritten with the checksummed vendor image, and the HSS reported the version that matches that release.

The programmer can read back what is on the device without changing it. The FlashPro Express action `DEVICE_INFO` (the project's `hw/libero/program.sh` runs it after every programming) prints the design name, a 16-bit design checksum, a design version and a digest for each programmable component. Read before touching anything, the board reported:

| Item | On the board as delivered | Reference design v2026.04, built locally |
|---|---|---|
| Design name | `MPFS_LINUX` | `MPFS_DISCOVERY_KIT` |
| Design checksum | `1d6e` | `1370` |
| Design version | `0000` | not applicable |

The names and checksums differ, so the programmed design is not the reference design built from the published scripts. The notes also record that it had been programmed with the 2024.2 version of the tools, where this project uses 2026.1. That, and the missing MMUART0 route, fit one reading: the board carried an earlier, general-purpose fabric whose clock configuration the 2026.04 Linux did not expect.

What was *not* shown is why the delivered design was what it was. It may be a deliberate earlier vendor release. Where it came from was not investigated, and nobody supplied a source file for it.

## Cause

**Established:** the fabric programmed on this board is a different design from reference design v2026.04, and the project's firmware and device tree assume the latter.

**Inferred, not proven:** that the clock warnings and the dead console are consequences of that mismatch. The evidence for the inference is that all three symptoms disappeared after the fabric was replaced (below), not that the mismatch was traced to a specific register.

**Not established:** anything about the old design beyond what the read-back shows. The read-back also reported `Device Integrity Status: FAIL`, with all device-integrity bits zero. The notes record that this was already the case before programming. It was not investigated and it did not stop programming or booting.

## Fix

With the board owner's approval, the reference design v2026.04 was built from Microchip's published scripts and programmed over JTAG. Two choices limited the risk:

1. **Only the FABRIC and SNVM components were programmed.** The boot flash (eNVM), which holds the HSS, was never written. That is the one step that could leave the board unable to boot, and the HSS already matched.
2. **The result was read back and verified.** The same programming run executed `PROGRAM`, `VERIFY` and `DEVICE_INFO`, and all three passed. Afterwards the board reported design `MPFS_DISCOVERY_KIT`, checksum `1370`, and a programming-cycle count one higher than before. The eNVM digest was identical before and after.

The eNVM digest has been the same through every programming run since, including the latest, which is public: [the v0.5.1 programming record](../../evidence/v0.5.1/build/programming.txt) shows `ENVM digest` beginning `55b85278`, the value the notes recorded on the first read.

On the reprogrammed board the eight clock warnings were gone, the PHY message was gone, and the real-time core's console appeared on the third serial port, with the vendor ping-pong demo exchanging 216 payloads with an error count of 0.

## How to recognise it

| What you see | What it tells you |
|---|---|
| Kernel warnings about clock dividers from a block in the *fabric* | The device tree describes a fabric that is not the one programmed |
| A peripheral that clocks, resets and transmits but produces nothing at the pins | The fabric does not route it; the MSS side cannot fix that |
| A design name or checksum from `DEVICE_INFO` that differs from the job you think is on the board | Trust the read-back. A working boot proves little about which design is loaded |
| Only the Linux image was updated when the board arrived | The fabric is a separate artifact; nothing about an image update replaces it |

Habits that followed:

- **Read the device before changing it.** `DEVICE_INFO` is non-destructive, and the "before" record was the only evidence of what had been there.
- **Keep a copy of every design you replace.** None was kept here, and the delivered fabric **cannot be restored**: no copy of its bitstream exists, and the read-back digest cannot be turned into one.
- **Program the smallest set of components.** Leaving the eNVM alone is why a reprogramming of a boot-critical device was a low-risk step.
- **Check the programming job against the board's identity first.** A design built for the sister Icicle Kit board must never be programmed on this one.

The raw read-back from before programming is recorded in the project's notes; the log is not published.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [FPGA fabric](../system/README.md#fabric) · [MMUART 0, 1 and 4](../system/README.md#mss-uarts) · [eNVM boot flash](../system/README.md#envm) · [FlashPro5 on USB-C](../system/README.md#debug-usb) |
| **Source** | [`hw/libero/program.sh`](../../hw/libero/program.sh): `DEVICE_INFO` |
| **Evidence** | [Programming, verify and device read-back of the v0.5.1 fabric](../../evidence/v0.5.1/build/programming.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [B06 Programming safely: components, digests and what never to touch](../modules/B06-programming-safely.md)<br>[F05 The tool flow in batch: Libero, the IP vault and FlashPro Express](../modules/F05-tool-flow-in-batch.md)<br>[K08 Timing met, board dead: the missing pin constraints](K08-timing-met-board-dead.md) |
| **Status** | validated on an earlier fabric |
<!-- nav:end -->
