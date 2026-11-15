---
id: P06
title: "Soak and reboot testing"
track: performance
level: 3
summary: What a ten-minute soak and thirty reboots do and do not prove.
repro: base
status: validated
hardware: [u54-4, sd-card, ethernet]
software: [hss, u-boot, linux-kernel, remoteproc-rpmsg, supervisor, pfdaq-cap]
sources:
  - tests/host/amp_reboot_test.sh
evidence: [reboot-30, soak-v030, boot-hss, boot-uboot-linux, suite-fresh-card, supervisor-console]
related: [K11, K12, B01, B02, P01, P02, X04]
---

# Soak and reboot testing

After this module you can say what a long run and a repeated boot test each look for, read the two records this project has, and, more importantly, say how much they do **not** show. A passing test tells you less than it seems to, and the amount can be worked out.

## Before you start

- **Level 3.** You should know the boot sequence at the level of module B01 (boot monitor, U-Boot, Linux, then Linux attaching to the real-time core) and what the supervisor and the capture tool do (M03, I06).
- **Tools:** a PC and the board on the network for the reboot test; nothing but a calculator for the arithmetic.
- **Time:** the soak takes ten minutes, the thirty reboots about half an hour.

## The idea

Two different questions hide behind "is it reliable?".

- **Does it keep working?** A **soak** runs the system continuously for a long time and looks for something that wears out or leaks, or an error that is rare per operation but certain over millions of them.
- **Does it start every time?** A **reboot** test cycles power or reset many times and checks that the system comes up correctly each time, because a start-up has many chances to go wrong that steady operation never meets.

Both produce evidence of the form "N trials, no failure". The useful thing to know is what that statement bounds. If a failure has probability *p* per trial, and trials are independent, then *n* clean trials make it unlikely, but not impossible, that *p* is large. A common rough rule, the **rule of three**, says that after *n* trials with no failure, *p* is below about 3/*n* with 95 % confidence. It is only as good as its assumptions: independent trials, and a check that really catches every failure.

## On this board

### The soak

A single supervised capture, run for ten minutes. The command recorded with the result is:

```sh
pfdaq_cap -r -w 252 -f 1024 -n 17000
```

That is 17,000 bursts of 1 MiB with the counter payload, every frame checked by the verifier on Linux, and every burst reported to the FreeRTOS supervisor with its 500 ms deadline. The record:

| Quantity | Value |
|---|---|
| Duration | 616.8 s |
| Bursts, frames, bytes | 17,000 bursts, 17,408,000 frames, 17,825,792,000 bytes |
| Bad header, bad sequence, bad payload, bad CRC | 0, 0, 0, 0 |
| DMA failures, injected faults | 0, 0 |
| Supervisor | 17,000 status messages, 0 deadline misses, 0 sequence mismatches |
| Fabric counters at the end | sequence and frame counters equal, 17,416,192, counted since the fabric was last reset: no sequence number skipped |
| Supervisor round trip | min 1.22 ms, mean 1.57 ms, max 10.0 ms |
| End-to-end rate | 28.9 MB/s |

**Conditions that matter:** fabric v0.3.0, the counter payload only, the FreeRTOS supervisor, the board otherwise idle. The soak was **not** repeated on the final fabric (v0.5.1), and the PRBS payload, the FIR payload and real-time mode were not soaked at all. The 12-hour run that was planned was never done.

### The reboot test

A script on the PC (`tests/host/amp_reboot_test.sh`) reboots the board over SSH, waits for it to come back, and on every cycle logs one row with three things:

1. The remote-processor state, which is recorded (it was `attached` in all 30 cycles).
2. Whether the supervisor on the fourth core has announced its message channel, which shows that it really started.
3. Whether a supervised capture passes: `pfdaq_cap -r -w 252 -f 1024 -n 8`, which exercises the whole chain from the fabric to the supervisor.

The closing line of the file counts a cycle as a full pass only if the channel is present and both the capture and the supervisor's own result are good.

The time column is measured from the `reboot` command to the first successful SSH, after a fixed 20 s wait and then polling every two seconds (a failed connection attempt can take up to four seconds more), so its resolution is a few seconds.

The 30-cycle record, summarised (the numbers below are from a short Python snippet that I ran over the published CSV):

| | |
|---|---|
| Cycles with all three checks passing | 30 of 30 |
| Time to SSH, all cycles | min 45.9 s, median 48.1 s, mean 48.7 s, max 61.4 s |
| Cycle 1 | 61.4 s |
| Cycles 2 to 30 | min 45.9 s, max 52.0 s, mean 48.3 s |
| FreeRTOS console bytes per cycle | 501 to 647; exactly 581 in 21 cycles |

The first cycle is 9 s slower than the slowest of the others. Why is not known. The console figure counts the bytes the supervisor's console log grew by per boot; its variation was not investigated.

These are **warm reboots** over SSH on the board that stays powered. Separately, the final image was also started cold from a freshly written SD card (record `suite-fresh-card`), and the full 27-test suite was run on that boot. Earlier images were cold-started the same way. Only the 30-cycle record and the fresh-card suite are published.

### What a good boot looks like

For orientation, the published boot records show the stages the three checks depend on: the boot monitor finds the boot image on the SD card at about 4.4 s of its own clock and brings the four application cores to running at about 22.5 s (record `boot-hss`); on the Linux console, the remote processor is attached 1.75 s after the kernel starts, the supervisor's channel appears 0.19 s later, and `/sbin/init` starts at 2.06 s (record `boot-uboot-linux`).

## What these results prove and what they do not

Here is the arithmetic, which I ran:

| Question | Answer |
|---|---|
| 30 clean boots: how large could the per-boot failure rate still be? | up to about 10 % (95 %), by the rule of three |
| If the true failure rate were 1 in 60, how likely are 30 clean boots? | 0.60: more likely than not |
| How many clean boots would show a rate below 1 %? | about 300 |
| Soak: 17.4 million frames without an error: bound per frame? | below about 1.7 × 10⁻⁷ (95 %) |
| The same per bit (1.43 × 10¹¹ bits)? | below about 2.1 × 10⁻¹¹ |

The second row is the important one. This project did see **one** boot in about sixty where the supervisor never started (case study K12). Thirty clean boots would have been the expected outcome 60 % of the time even if that rate were real, so the 30-of-30 result does **not** show that the problem is gone. What does count against it is the total: about 57 boots accounted for since the failure, none failing, which would have been about 38 % likely at that rate ((59/60)^57). That is still not small, and those boots span three fabric versions, so they are not repeats of one experiment. Neither number settles it.

The per-frame and per-bit bounds hold only if the check catches every corrupted word and the frames are independent. The verifier recomputes every payload, header and CRC independently of the fabric (module P03 shows it catching injected faults), so the first assumption is reasonable; the second cannot be proven for a memory system.

What the soak does not cover:

- **Anything that needs hours.** Leaks, wear, and thermal effects are not tested at ten minutes. No temperature or supply measurement was taken.
- **Any other mode.** Real-time mode loses frames by design under load and was not soaked.
- **The final hardware.** The fabric changed after the soak.
- **A second board.** Every result is from one unit.

What the reboot test does not cover:

- **Power-cycle behaviour.** Cold starts need someone at the power cable or a switchable supply; there are three fresh-card cold boots, not thirty.
- **Variation in timing.** Every cycle waits the same 20 s, so the test always catches the board at about the same point.
- **What it did not look for.** It checks three things per boot. A boot that passes them but is subtly wrong passes.

## Run it

These are the commands the project ran. The board was not used to write this page; the records are above.

On the board, the soak:

```sh
pfdaq_cap -r -w 252 -f 1024 -n 17000
```

On the PC, the reboot test (30 cycles, results to a CSV, board address and the supervisor's console log as optional arguments):

```sh
tests/host/amp_reboot_test.sh 30 out.csv <board address> <freertos console log>
```

For the arithmetic, a snippet that reads the published CSV and prints the table above. It is the one I ran; it needs only Python:

```python
import csv, statistics as st
rows = [r for r in csv.reader(open('evidence/v0.5.1/board/reboot-30.csv')) if r and r[0].isdigit()]
up = [float(r[1]) for r in rows]
print(len(rows), min(up), st.median(up), st.mean(up), max(up))
print((59/60)**30, 3/30, 3/17408000, (59/60)**57)
```

## What you should see

```text
30 45.91211345 48.076036414499995 48.73406599563333 61.420734919
0.6039803893781186 0.1 1.7233455882352942e-07 0.38365723429718834
```

and the last line of the CSV file itself:

```text
# supervisor channel present: 30/30, full pass: 30/30
```

## Evidence

- **Board, fabric v0.5.1:** the 30-cycle reboot record (`reboot-30`), the boot records (`boot-hss`, `boot-uboot-linux`), the full suite from a freshly written card (`suite-fresh-card`) and its supervisor console (`supervisor-console`).
- **Board, fabric v0.3.0:** the ten-minute soak (`soak-v030`).
- **Recorded in the project's notes, raw logs not published:** the earlier reboot tests (12, 6 and 6 cycles), 20 warm reboots of the vendor's image, and the two earlier cold boots.
- **Computed here:** the statistics and the bounds.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| A row in the CSV reads `TIMEOUT` | SSH did not come back within the script's patience (the 20 s wait plus up to 40 attempts); the board did not boot or has no network |
| The supervisor channel count is 0 but Linux is up | the supervisor did not start or did not announce; this is the K12 symptom |
| `capture_pass` is `none` | the capture tool failed to run or print its result line |
| Remote-processor state other than `attached` | the supervisor core was not started or Linux could not attach to it; not seen in the 30 cycles |

## Trade-offs and going further

- **Cheap trials first, expensive ones when they matter.** Warm reboots are fast and automatable; cold starts are slow and need a person or a switched supply. Both belong in a plan.
- **Record the rare event, not only the verdict.** The one failure this project saw was not reproduced, and the data that would have explained it was not captured. Logging all three consoles on every cycle costs nothing during a reboot test.
- **Vary the conditions.** A fixed wait hides timing-dependent problems. A randomised wait between reboots and a mix of warm and cold starts would not.
- **Exercise:** how many consecutive clean boots would you want before claiming the K12 failure rate is below 1 in 100 at 95 % confidence? Compare it with what the project has.
- **Exercise:** the soak ran at 28.9 MB/s for ten minutes. At the same rate, how long would the planned 12 hours move, and how many frames would that be?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [U54 hart 4](../system/README.md#u54-4) · [microSD card](../system/README.md#sd-card) · [Gigabit Ethernet](../system/README.md#ethernet) · [Hart Software Services](../system/README.md#hss) · [U-Boot](../system/README.md#u-boot) · [Linux 6.18.17](../system/README.md#linux-kernel) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) · [pfdaq supervisor](../system/README.md#supervisor) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`tests/host/amp_reboot_test.sh`](../../tests/host/amp_reboot_test.sh) |
| **Evidence** | [Thirty warm reboots, both systems up and a supervised capture passing each time](../../evidence/v0.5.1/board/reboot-30.csv) (board, fabric v0.5.1, 2026-11-15)<br>[Ten-minute supervised soak, 17.8 GB, no errors](../../evidence/v0.3.0/board/soak-10min.json) (board, fabric v0.3.0, 2026-11-15)<br>[Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15)<br>[The same suite on a freshly written SD card, 27 of 27](../../evidence/v0.5.1/board/suite-fresh-card.md) (board, fabric v0.5.1, 2026-11-15)<br>[Supervisor console during the fresh-card suite, with the alarm and recovery messages](../../evidence/v0.5.1/board/supervisor-console.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [K11 remoteproc cannot restart the hart](../case-studies/K11-remoteproc-restart.md)<br>[K12 One boot in sixty, FreeRTOS never started](../case-studies/K12-freertos-no-start.md)<br>[B01 From power-on to login, stage by stage](B01-power-on-to-login.md)<br>[B02 The HSS payload: starting two operating systems](B02-hss-payload.md)<br>[P01 Where 940 MB/s becomes 30: throughput layer by layer](P01-throughput-layers.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md)<br>[X04 The validation report](X04-validation-report.md) |
| **Status** | validated |
<!-- nav:end -->
