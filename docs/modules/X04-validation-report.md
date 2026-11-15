---
id: X04
title: "The validation report"
track: system
level: 3
summary: Every result, with the conditions it was measured under and the evidence file behind it.
needs: [X01]
repro: doc
status: validated
hardware: [soc, pfdaq-core, dma, u54-linux, u54-4]
software: [pfdaq-cap, pfdaq-lat, supervisor]
evidence: [suite-freertos, suite-freertos-raw, suite-fresh-card, suite-baremetal, reboot-30, sim-stream, sim-latency, timing, timing-without-cdc, programming, soak-v030, fabric-throughput-v030]
related: [X01, X03, P02, K10, K11, K12, H11]
---

# The validation report

This is the project's results in one place, organised by the question each answers. For every result it gives the method, the conditions, the fabric version it was measured on, and the record behind it. Anything the project did not do is listed at the end, in the same detail.

## Before you start

- **Level 3.** Module X01 describes the system; module X03 shows how a board is brought to the state these results were measured in.
- **How to read the tables.** Every row ends with a **Kind**: *board* (measured on the physical board), *sim* (RTL simulation), *build* (tool output). They are different forms of evidence and are never mixed in one claim.
- **Single board.** All board results come from one unit. All are fabric v0.5.1 and the final image unless a row says otherwise.

## The idea

A validation report answers a short list of questions, and for each answer says how sure it is and why. A number with no conditions is not a result. The tables below follow one rule: the number, the command or method that produced it, and the condition that matters most (load, payload, version, how many runs) are in the same row, and the **Record** column names a file anyone can open. Where a result exists only in the project's own notes and its raw log is not published, the row says so.

The suite itself (`tests/board/pfdaq_final_suite.sh`, summarised by `tests/host/summarise_suite.py`) prints one line per test; the full output of the FreeRTOS run is the [suite record](../../evidence/v0.5.1/board/suite-freertos.md), with the raw JSON beside it.

## 1. Does it work?

| Result | Method | Conditions | Kind | Record |
|---|---|---|---|---|
| **27 of 27** functional tests pass, plus 13 measurements and 2 known issues | `pfdaq_final_suite.sh` on the board, FreeRTOS on hart 4 | final image, fabric v0.5.1, 3 Linux CPUs | board | [suite](../../evidence/v0.5.1/board/suite-freertos.md) |
| The same **27 of 27**, from a freshly written card | image written with `bmaptool`, cold start, same suite | nothing carried over from earlier work | board | [fresh-card suite](../../evidence/v0.5.1/board/suite-fresh-card.md) |
| **8 of 8** functional tests with the bare-metal supervisor instead of FreeRTOS | `pfdaq_bm_suite.sh` | same fabric and Linux; only the hart 4 firmware differs | board | [bare-metal suite](../../evidence/v0.5.1/board/suite-baremetal.md) |
| **30 of 30** warm reboots end with remoteproc attached, the supervisor channel present and a supervised capture passing | `amp_reboot_test.sh 30`: reboot over SSH, then check | 45.9 to 61.4 s from the command to SSH (mean 48.7 s) | board | [reboot test](../../evidence/v0.5.1/board/reboot-30.csv) |
| Cold boot from a fresh card worked first time on three images (v0.3, v0.4, final) | write the card, power-cycle, no manual fixes beyond the forced password change and the SSH key | only the final image has a published record | board | recorded in the project's notes; the raw consoles of the earlier two are not published |
| 20 of 20 warm reboots of the vendor's Linux image | `boot_cycle_test.py` | the board's baseline before the project's image, 40.6 to 41.5 s | board | recorded in the project's notes; the raw log is not published |
| Fabric **PROGRAM, VERIFY and DEVICE_INFO** pass; eNVM digest unchanged | FlashPro Express | passed in all 8 programmings; only the latest is published | board | [programming](../../evidence/v0.5.1/build/programming.txt) |

## 2. Is the data right?

| Result | Method | Conditions | Kind | Record |
|---|---|---|---|---|
| **20 stream scenarios** pass | a testbench records the stream; an independent Python model recomputes every frame (header, sequence, payload for three modes, CRC-32, `TLAST`, `TDEST`, drops, abort) | random `TREADY` backpressure, frame lengths from 5 to 504 words | sim | [stream simulation](../../evidence/v0.5.1/sim/stream.txt) |
| **0 sequence, payload and CRC errors** in 32,768 frames (33.6 MB) for each of the counter, PRBS and FIR payloads | `pfdaq_cap -r -m <mode> -w 252 -f 1024 -n 32` | 1 KiB frames, supervised | board | [suite](../../evidence/v0.5.1/board/suite-freertos.md) |
| 0 errors at both extremes of burst size: 64 frames of one payload word each, and 2,000 frames (32.8 MB) in one burst | `T5-min-frame`, `T5-max-burst` | even frame lengths only; see section 6 for the odd-length defect | board | [suite](../../evidence/v0.5.1/board/suite-freertos.md) |
| **Every injected fault is detected, with the exact count**, in all three payload modes: 4 corrupted CRCs gave 4 CRC errors; 5 skipped sequence numbers gave 5 sequence errors; 2 flipped payload bits gave 2 payload and 2 CRC errors | `pfdaq_cap -r -F crc\|seq\|payload` per mode | 20,480 frames per run | board | [suite](../../evidence/v0.5.1/board/suite-freertos.md) |
| **Abort** completes the DMA: 119 of 500 frames real, then zero fill, in the 2 ms case; 128 of 8,000 in the 50 µs case; nothing written past the buffer; the next run is normal | `pfdaq_cap -A 2000 ...` and `-A 50 ...` | an 8 MB PRBS burst and an 8,000-frame FIR burst | board and sim | [suite](../../evidence/v0.5.1/board/suite-freertos.md), [stream simulation](../../evidence/v0.5.1/sim/stream.txt) |
| **Memory shared between Linux and hart 4** is seen consistently from both sides, uncached and cached, with no cache maintenance: 0 mismatches in 50 passes over 65,536 words each way | `pfdaq_cap -X 65536 -n 50` (and `-c` for cached) | FreeRTOS and bare metal both | board | [suite](../../evidence/v0.5.1/board/suite-freertos.md), [bare-metal suite](../../evidence/v0.5.1/board/suite-baremetal.md) |
| **17.4 million frames, 17.83 GB, no error** in a supervised run of 616.8 s | `pfdaq_cap -r -w 252 -f 1024 -n 17000` | **fabric v0.3.0**, counter payload only, FreeRTOS supervisor | board | [soak](../../evidence/v0.3.0/board/soak-10min.json) |

The model is written separately from the RTL on purpose; module H11 explains why that matters and what it cannot catch.

## 3. How fast is it?

Three different rates are called "throughput" in this project. They are not interchangeable.

| Rate | Value | Method and conditions | Fabric | Kind | Record |
|---|---|---|---|---|---|
| **Hardware path** into memory | 931.6 to 947.3 MB/s | the fabric's own 125 MHz timestamps over one burst, for bursts of 4,160 bytes up to 8.19 MB. The bursts of 96, 512 and 4,096 bytes printed 0.0; why was not investigated | **v0.3.0** | board | [fabric throughput](../../evidence/v0.3.0/board/suite-python-x3.txt) |
| **Copy** out of the DMA buffer | 60.4 to 76.3 MB/s | `memcpy` of a finished burst while the DMA fills the other buffer; range over the `T4` rows and the 2,000-frame burst | v0.5.1 | board | [raw suite](../../evidence/v0.5.1/board/suite-freertos.jsonl) |
| **Verified end to end** | 29.6 MB/s counter, 28.1 MB/s PRBS, **8.7 MB/s FIR**; 25.8 MB/s for one 2,000-frame burst | START to the last frame checked, `-m counter\|prbs\|fir`, 1 KiB frames | v0.5.1 | board | [raw suite](../../evidence/v0.5.1/board/suite-freertos.jsonl) |
| Verified end to end, long run | 28.9 MB/s | the 17.83 GB run above | v0.3.0 | board | [soak](../../evidence/v0.3.0/board/soak-10min.json) |

The raw suite also records the verification step alone: 55.4 MB/s for the counter payload and 10.2 MB/s for FIR. The FIR figure is the software model recomputing the filter; it explains why FIR is slower end to end, though no profile of the verification code is recorded.

### Real-time mode: what happens when the consumer cannot keep up

Frames become due on a fixed schedule; a frame with no buffer is dropped and counted. 4,000 slots per row, 1 KiB frames, four descriptors, PRBS payload, board idle. Every drop is accounted for in the fabric's counter and the sequence numbers, and every row has **0 sequence, payload and CRC errors**.

| Slot interval | Offered | Sent | Dropped |
|---|---|---|---|
| 1,000 µs | 1.0 MB/s | 4,000 | 0 |
| 100 µs | 10.2 MB/s | 3,999 | 1 (0.0 %) |
| 50 µs | 20.5 MB/s | 3,208 | 792 (19.8 %) |
| 20 µs | 51.2 MB/s | 1,318 | 2,682 (67.0 %) |
| 10 µs | 102.4 MB/s | 586 | 3,414 (85.3 %) |
| 5 µs | 204.8 MB/s | 463 | 3,537 (88.4 %) |

Record: [suite](../../evidence/v0.5.1/board/suite-freertos.md). The "delivered MB/s" column of that record understates the rate because it is computed over the whole loop including the final wait; the sent and dropped counts are the figures that mean something. Module H06 explains the mechanism.

## 4. How responsive is it?

The fabric raises an interrupt and counts 20 ns cycles until software acknowledges it. 100,000 events per row; fabric v0.5.1; "loaded" means one busy loop per Linux CPU plus a continuous capture loop. Module P02 explains each row and what it cannot show.

| Who answers | Load | Mean | Worst seen | Events missed |
|---|---|---|---|---|
| Bare-metal handler on hart 4 | idle | 0.50 µs | 3.10 µs | 0 |
| FreeRTOS handler on hart 4 | idle | 0.52 µs | 2.10 µs | 0 |
| FreeRTOS handler on hart 4 | Linux loaded | 0.53 µs | 1.94 µs | 0 |
| FreeRTOS task on hart 4 | idle | 2.04 µs | 11.66 µs | 0 |
| Linux kernel handler | idle | 4.41 µs | 1,298 µs | 6 |
| Linux user space, default scheduling | idle | 78.31 µs | 5,131 µs | 89 |
| Linux user space, `SCHED_FIFO` 80 | loaded | 73.58 µs | 1,815 µs | 18 |

Records: [FreeRTOS suite](../../evidence/v0.5.1/board/suite-freertos.md) and [bare-metal suite](../../evidence/v0.5.1/board/suite-baremetal.md), with the latency unit's simulation in [latency simulation](../../evidence/v0.5.1/sim/latency.txt). The conclusion the data supports: hart 4 answered every one of the 600,000 events of its six rows (bare-metal and FreeRTOS handler, FreeRTOS task, each idle and loaded) within 12 µs. Linux, even in a kernel handler, shows stalls of more than a millisecond and is characterised here, not bounded.

## 5. How robust is it?

| Result | Method | Conditions | Kind | Record |
|---|---|---|---|---|
| **Deadline supervision**: a stalled Linux gives 1 missed deadline and an alarm; reports resume and the supervisor returns to normal | `pfdaq_cap -r -D 300 -P 5:900` | 300 ms deadline, a 900 ms pause injected | board | [suite](../../evidence/v0.5.1/board/suite-freertos.md), [supervisor console](../../evidence/v0.5.1/board/supervisor-console.txt) |
| **The consumer is killed** mid-run: the supervisor alarms and the next run passes with 0 errors | `kill -9` of `pfdaq_cap`, then a normal run | `T9-after-kill`: 16,384 frames, 0 errors | board | [suite](../../evidence/v0.5.1/board/suite-freertos.md) |
| **Timing closure**: all multi-corner timing constraints met; 64 of 64 pins placed; 24,334 logic elements (26.02 % of 93,516) | place and route and Verify Timing | worst slack 1.342 as printed by the tool | build | [timing and utilisation](../../evidence/v0.5.1/build/timing-and-utilisation.txt) |
| Without the clock-crossing constraint file, every violating path the tool lists is one of the intended crossings | Verify Timing without `pfdaq_cdc.sdc` | the 20 worst paths in each of two reports, 40 in all; a complete list was not kept | build | [timing without constraints](../../evidence/v0.5.1/build/timing-without-cdc-constraints.txt) |
| **Timing and boot survive a rebuild**: the same fabric component digest came out of the rebuild from the restructured repository | rebuilt as design version 7 | digest compared; the rebuilt fabric was not programmed | build | recorded in the project's notes; the raw log is not published |

## 6. What fails

Three things do not work, and each has its own case study.

| Issue | What is known | Case study |
|---|---|---|
| **Odd frame lengths in multi-frame bursts corrupt data.** 2 KB pieces are lost or misplaced at destination boundaries; the DMA still reports completion. Even lengths are clean. The suite's last test shows it: 148 frames, errors 3 sequence / 4 payload / 4 CRC, no fault injected | correct in simulation, timing met, cause not found; `pfdaq_cap` refuses the case unless told otherwise | K10 |
| **Linux cannot restart hart 4.** Stop works; start fails, because the boot monitor rejects the request. The suite records state `offline` after the start | the monitor is in the chip's boot flash, which this project never reprograms; recovery is a reboot | K11 |
| **Hart 4 once did not start**, in about one boot in sixty, with no console output and no channel | not reproduced since, including the 30 reboots above; unexplained | K12 |

The project's failures table, from the first boards that did not match the reference design to the early-TLAST abort, is in `docs/validation-plan.md`, with a case study for most entries.

## 7. What was not done

Stated as plainly as the results:

- **No 12-hour soak.** The only long run is 616.8 s, 17.83 GB, on fabric v0.3.0 with the counter payload. It has not been repeated on v0.5.1, nor with the PRBS or FIR payloads.
- **No repeated cold power cycles.** Three fresh-card cold boots were done, once per image. A controllable power supply is needed to automate cold cycles; the 30-cycle test is warm reboots only.
- **One board.** Nothing has been repeated on a second Discovery Kit; unit-to-unit variation is not measured.
- **The fabric throughput was not re-measured on v0.5.1.** The 931 to 947 MB/s figures come from v0.3.0; the current C tool does not print the fabric-side rate.
- **The root cause of the odd-length defect is not found.** It would need the vendor DMA core's source in a simulator, which was not available.
- **The rebuilt image has not been booted.** The kernel image, kernel module and payload built from the restructured repository are not byte-identical to the proven ones and have not been run on the board yet.
- **No electrical or environmental testing.** No temperature, supply voltage or signal-integrity measurements.
- **No coverage measurement.** The 20 simulation scenarios are the coverage statement; no tool measured which behaviours they exercise.
- **No determinism tuning of Linux.** Interrupt affinity, CPU isolation and a real-time kernel were not tried; the 1 to 5 ms stalls are characterised, not explained.

## Evidence

Every record named above is in `evidence/`, each with a manifest entry giving the command and the conditions. They are the only source for the numbers on this page, except where a row says the figure is recorded in the project's notes.

## Trade-offs and going further

- **Different layers, different numbers.** 940 MB/s and 30 MB/s are both true; they measure the hardware path and the verified path. A report that gives only one misleads.
- **A pass is only as strong as its checker.** Module H11 discusses what an independent model catches and what the scenario list leaves untested.
- **Exercise:** pick any number from section 3 or 4 and write down which of its conditions you would have to change to get a result that is no longer comparable.
- **Exercise:** from the section 7 list, choose the one that would most change a reader's confidence, and say how you would measure it.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [X01 pfdaq in one page](X01-pfdaq-in-one-page.md) |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [MPFS095T system-on-chip](../system/README.md#soc) · [pfdaq_core](../system/README.md#pfdaq-core) · [CoreAXI4DMAController](../system/README.md#dma) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [pfdaq_lat](../system/README.md#pfdaq-lat) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[The same suite on a freshly written SD card, 27 of 27](../../evidence/v0.5.1/board/suite-fresh-card.md) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with the bare-metal supervisor on hart 4, 8 of 8](../../evidence/v0.5.1/board/suite-baremetal.md) (board, fabric v0.5.1, 2026-11-15)<br>[Thirty warm reboots, both systems up and a supervised capture passing each time](../../evidence/v0.5.1/board/reboot-30.csv) (board, fabric v0.5.1, 2026-11-15)<br>[Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Latency unit simulation, four phases checked by the Python model](../../evidence/v0.5.1/sim/latency.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Timing analysis without the clock-crossing constraints (the tool's console log; the 40 worst violating paths it lists are the intended crossings)](../../evidence/v0.5.1/build/timing-without-cdc-constraints.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Programming, verify and device read-back of the v0.5.1 fabric](../../evidence/v0.5.1/build/programming.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Ten-minute supervised soak, 17.8 GB, no errors](../../evidence/v0.3.0/board/soak-10min.json) (board, fabric v0.3.0, 2026-11-15)<br>[Fabric-side throughput of single bursts, 931 to 947 MB/s](../../evidence/v0.3.0/board/suite-python-x3.txt) (board, fabric v0.3.0, 2026-11-15) |
| **Related** | [X01 pfdaq in one page](X01-pfdaq-in-one-page.md)<br>[X03 Reproduce the board from an empty SD card](X03-reproduce-from-empty-sd.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md)<br>[K10 Odd frame lengths lose 2 KB chunks](../case-studies/K10-odd-frame-lengths.md)<br>[K11 remoteproc cannot restart the hart](../case-studies/K11-remoteproc-restart.md)<br>[K12 One boot in sixty, FreeRTOS never started](../case-studies/K12-freertos-no-start.md)<br>[H11 Verification with an independent model](H11-independent-model.md) |
| **Status** | validated |
<!-- nav:end -->
