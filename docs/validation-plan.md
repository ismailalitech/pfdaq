# Validation plan and results

Last updated 2026-11-15 (fabric v0.5.1). Evidence classes: **SIM** (simulation), **HOST** (host-side check), **BUILD** (tool/build output), **BOARD** (measured on the physical board). A result is only claimed in the class shown. Numbers are from single runs unless a count is given.

The final board run is `tests/board/pfdaq_final_suite.sh` (FreeRTOS on U54_4) and `tests/board/pfdaq_bm_suite.sh` (bare-metal U54_4), summarised by `tests/host/summarise_suite.py`. Records are under `evidence/`, each with a manifest entry giving command and conditions:

- **FreeRTOS:** 27/27 functional tests pass, 13 measurements, 2 known issues (`evidence/v0.5.1/board/suite-freertos.md`); repeated on a freshly written card of the final image with the same result (`evidence/v0.5.1/board/suite-fresh-card.md`).
- **Bare metal:** 8/8 functional tests pass (`evidence/v0.5.1/board/suite-baremetal.md`).

## 1. Results by test

| ID | Test | Class | Method | Result | Evidence |
|---|---|---|---|---|---|
| T1a | Baseline boot repeat | BOARD | 20 warm reboots of the vendor image | 20/20, 40.6-41.5 s | raw log, not published |
| T1b | AMP boot repeat | BOARD | Warm reboots over SSH; per cycle: remoteproc, supervisor channel, supervised capture | 12/12, 6/6, 6/6, 30/30 (46-61 s to SSH) | `evidence/v0.5.1/board/reboot-30.csv` (the 30-cycle run) |
| T1c | Fresh-card cold boot | BOARD | Image written to a card, cold start, no manual fixes | 3/3 (v0.3, v0.4 and final v0.5.1 images); on the final card the full suite passes 27/27 | `evidence/v0.5.1/board/suite-fresh-card.md`; console logs not published |
| T2 | Register access | SIM + BOARD | APB reads/writes in the testbench; readback on the board | SIM pass (coefficients, mode, credits included); BOARD ID/VERSION and configuration read back | `evidence/v0.5.1/sim/stream.txt` |
| T3 | Interrupt delivery | BOARD | DMA completion count vs transfers; latency unit EVENTS vs ACKS | One completion per transfer (17,000 in the soak on fabric v0.3.0); EVENTS = ACKS in every U54_4 latency run | `evidence/v0.3.0/board/soak-10min.json`, suite records |
| T4 | Data integrity | SIM + BOARD | Sequence, payload model and CRC-32 per frame against an independent model | SIM: 20 stream scenarios pass. BOARD: counter, PRBS and FIR bursts 0 errors; 10-minute soak 17.4 M frames 0 errors (fabric v0.3.0, counter payload) | `evidence/v0.5.1/sim/stream.txt`, `evidence/v0.5.1/board/suite-freertos.md`, `evidence/v0.3.0/board/soak-10min.json` |
| T5 | DMA boundaries | BOARD | 1-word frames to 8.19 MB bursts, guard words after the buffer | Pass for even frame lengths. **Odd frame lengths in multi-frame bursts fail** (known issue K1) | suite records |
| T6 | Throughput, backpressure, real-time drops | BOARD | Fabric timestamps; real-time slot sweep 1 ms to 5 us per 1 KiB frame with 4 destinations | Fabric 931-947 MB/s (measured on fabric v0.3.0, not repeated on v0.5.1). Real-time: no drops at 1 ms slots, 1 drop in 4,000 at 100 us slots (the 1,000-slot fault run at 100 us dropped none); 20 % drops at 50 us slots (about 20 MB/s offered) and more above; drops from 50 us slots (about 20 MB/s offered) upward, every drop accounted for in DROPS and sequence gaps, 0 data errors at every rate | suite records |
| T7 | Cross-context memory visibility | BOARD | Linux writes a pattern, U54_4 verifies and writes the inverse, Linux verifies; uncached and cached, no cache maintenance | 0 mismatches in 50 × 64 K words each way, uncached and cached, FreeRTOS and bare metal | suite records |
| T8 | Interrupt latency and jitter | SIM + BOARD | Fabric latency unit, 20 ns resolution, 100,000 events per condition | See section 1b | suite records, `evidence/v0.5.1/sim/latency.txt` |
| T9 | Fault handling and recovery | SIM + BOARD | Fabric faults per payload mode; Linux stall vs deadline; consumer killed mid-run; remoteproc stop/start | All injected faults detected exactly (all three modes); deadline: 1 miss, alarm, recovery; after kill -9 the supervisor alarms and the next run passes. Remoteproc stop works, **start fails** (known issue K2) | suite records |
| T10 | Long-run stability | BOARD (short) | Supervised soak | 616.8 s, 17.83 GB, 0 errors, on fabric v0.3.0 with counter payload. Not repeated on v0.5.1; the planned 12 h run was not done | `evidence/v0.3.0/board/soak-10min.json` |
| T11 | Timing closure | BUILD | Multi-corner STA; I/O placement check | v0.5.1: max/min met, 64/64 I/Os placed; without `pfdaq_cdc.sdc` all 40 violating paths the tool lists (its 20 worst per report) are the intended crossings; a complete path list was not kept | `evidence/v0.5.1/build/timing-and-utilisation.txt`, `evidence/v0.5.1/build/timing-without-cdc-constraints.txt` |
| T12 | Programming integrity | BOARD | FlashPro Express PROGRAM, VERIFY, DEVICE_INFO | Passed every time (8 programmings); eNVM digest identical throughout | `evidence/v0.5.1/build/programming.txt` (latest programming; earlier logs not published) |
| T13 | Graceful abort | SIM + BOARD | ABORT during an 8 MB burst (2 ms) and an 8,000-frame FIR burst (50 us) | DMA completes, all frames before the abort intact, rest zero-filled, nothing past the buffer, next run normal | suite records, `evidence/v0.5.1/sim/stream.txt` |

## 1b. Interrupt-to-acknowledge latency (BOARD, fabric v0.5.1, 100,000 events each)

| Who acknowledges | Load | Min us | Mean us | p99 <= us | Max us | Missed |
|---|---|---|---|---|---|---|
| Bare-metal interrupt handler (U54_4) | idle | 0.50 | 0.50 | 0.62 | 3.10 | 0 |
| Bare-metal interrupt handler (U54_4) | Linux loaded | 0.50 | 0.51 | 0.94 | 2.44 | 0 |
| FreeRTOS interrupt handler (U54_4) | idle | 0.52 | 0.52 | 0.62 | 2.10 | 0 |
| FreeRTOS interrupt handler (U54_4) | Linux loaded | 0.52 | 0.53 | 0.94 | 1.94 | 0 |
| FreeRTOS task (U54_4) | idle | 2.00 | 2.04 | 2.54 | 11.66 | 0 |
| FreeRTOS task (U54_4) | Linux loaded | 2.00 | 2.09 | 3.82 | 9.36 | 0 |
| Linux kernel handler (`pfdaq_uio`) | idle | 1.36 | 4.41 | 10.22 | 1298 | 6 |
| Linux kernel handler | loaded | 1.50 | 3.95 | 10.22 | 1280 | 11 |
| Linux user space (UIO) | idle | 4.18 | 78.3 | 122.9 | 5131 | 89 |
| Linux user space, SCHED_FIFO 80 | idle | 2.58 | 76.1 | 122.9 | 1674 | 7 |
| Linux user space, SCHED_FIFO 80 | loaded | 2.24 | 73.6 | 122.9 | 1815 | 18 |

- **Event period:** 200 us for the U54_4 and kernel rows, 500 us for user space.
- **"Loaded":** one busy loop on each Linux CPU plus a continuous capture loop.
- **"Missed":** an event that became due while the previous one was still unacknowledged.
- **Reading the table:** the U54_4 context never missed an event. FreeRTOS adds about 20 ns over bare metal in the interrupt handler, and about 1.5 us when a task does the work. Linux, even in the kernel handler, has rare stalls of 1-5 ms, so it is characterised here rather than given a bound.

## 2. Failures found and what they led to

| Finding | Class | Consequence |
|---|---|---|
| Delivered fabric did not match v2026.04 (no MMUART0 route, 8 kernel clock warnings) | BOARD | Reference fabric programmed |
| Status readback path failed timing (-0.707 ns) | BUILD | Snapshot copied into the bus clock domain |
| "Done" never set after that change | SIM | Single crossing carrying data and flag |
| Python register writes arrived byte by byte | BOARD | `libmmio.so`; C tools use real 32-bit stores |
| Packets above about 360 bytes corrupted, DMA hung | BOARD | Two words per clock, continuous TVALID |
| Re-armed descriptor ignored once a packet was waiting | BOARD | Arm-before-start protocol; real-time mode uses credits so this can't happen |
| Kernel driver clears the DMA status flag | BOARD | Completion from UIO interrupt + descriptor bit |
| `/dev/mem` refused for the source registers | BOARD | UIO device-tree node |
| Vendor RPMsg example flow did not match this kernel | BOARD | Endpoint discovery handles both layouts |
| Every Linux latency event woke the process twice | BOARD | Acknowledge only when the fabric reports pending |
| FreeRTOS task-mode latency: line stayed masked after the first event | BOARD | Mask in the handler before waking the task (the port switches tasks inside the ISR) |
| Kernel UIO driver could not be mapped | BOARD | Page-aligned memory map with offset, as in `uio_pdrv_genirq` |
| Brackets in register names unmatched by SDC patterns (three failed builds) | BUILD | Paths named by source and destination register groups |
| Build script dropped all I/O constraints; board stuck in an HSS reset loop | BOARD | File list fixed; builds now stop unless every I/O is placed |
| An early TLAST does not complete a DMA transfer (abort left the DMA waiting) | BOARD | Abort finishes the frame, then zero-fills to the full length |
| One-cycle TVALID gap when an abort hit a frame boundary | SIM | First fill beat sent in the same clock |
| Odd frame lengths in multi-frame bursts lose 2 KB chunks | BOARD | **Known issue K1**, refused by `pfdaq_cap` unless `-O` |
| Remoteproc restart rejected by HSS | BOARD | **Known issue K2**, recovery is a reboot |
| FreeRTOS did not start on one early boot | BOARD | **Open**, 1 in about 60 boots, not seen since |

## 3. Not covered

- **Repeated cold power cycles:** these need someone at the power cable or a switchable supply.
- **The 12 h soak:** not run; only a 10-minute soak was done.
- **Root cause of K1:** this would need the vendor DMA RTL in simulation.
- **Electrical and environmental:** no temperature, voltage or electrical measurement.
- **A second board:** every BOARD result is from one unit.

## 4. Instrumentation limits

- **Fabric timestamps:** resolution is one 125 MHz period (8 ns), and the counter wraps at 32 bits (about 34 s).
- **Latency unit:** resolution is 20 ns, counted from the first cycle the line is high to the APB write of ACK.
- **"START to interrupt":** includes the register write, the transfer, interrupt delivery and the wake-up of a user process.
- **RPMsg round trips:** include two user/kernel transitions on Linux and a 1 ms delay in the supervisor's snapshot.
- **"Delivered MB/s" in real-time runs:** measured over the whole loop including the final wait, so it understates the rate. The sent/dropped counts are the meaningful figures.

## 5. How to rerun

| What | Command |
|---|---|
| Stream simulation | `make -C hw sim-stream` |
| Latency simulation | `make -C hw sim-latency` |
| Full board suite | copy `tests/board/pfdaq_final_suite.sh` to the board, run it, then `tests/host/summarise_suite.py <output>` on the PC |
| Bare-metal comparison | `firmware/build_payload.sh`, write `build/payload-bm/payload.bin` to SD partition 2, reboot, run `tests/board/pfdaq_bm_suite.sh`; write the image's own `payload.bin` back afterwards |
| Reboot test (PC) | `tests/host/amp_reboot_test.sh 30 out.csv <board address> <freertos console log>` |
| Single runs (board) | `pfdaq_cap -r -m fir -S 333 -w 252 -f 1024 -n 32`, `pfdaq_cap -m prbs -w 252 -T 12500 -n 4000 -d 4`, `pfdaq_cap -A 2000 -m prbs -w 4092 -f 500`, `pfdaq_lat -c 1 -p 200 -n 100000`, `pfdaq_cap -X 65536 -n 50 [-c]` |
