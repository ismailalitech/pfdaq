# Architecture (as built)

Last updated 2026-11-15 (fabric v0.5.1: payload modes, real-time mode, packet-safe abort, latency unit; FreeRTOS and bare-metal U54_4 builds). This replaces the M0 draft. Status tags: **B** = measured or observed on the board, **S** = simulation, **BUILD** = tool/build output, **V** = verified from a primary source (vendor document or source code), **I** = inference, **U** = unresolved. Evidence is listed in `validation-plan.md` and recorded under `evidence/`.

## 1. Application

**Instrumented stream acquisition with real-time supervision.**

What runs today (B):

- The fabric generates framed, sequence-numbered, timestamped, CRC-protected test data and pushes it through an AXI4-Stream bridge and DMA into non-cached DDR.
- Linux (U54_1-3) configures the source, receives bursts into two alternating buffers, verifies every frame and reports each verified burst.
- FreeRTOS (U54_4) supervises Linux over RPMsg: it enforces a deadline on the reports and cross-checks the reported sequence number against the fabric's own counters, which it reads directly.
- Faults can be injected in the fabric (bad CRC, skipped sequence number, flipped payload bit) and in Linux (deliberate stall), and each is detected.

- A fabric latency unit raises interrupts to Linux and to the U54_4 context and measures, in fabric clock cycles, how long each takes to acknowledge (B).
- Payload modes: counter, PRBS, and an 8-tap FIR filter running on the fabric's multipliers over a generated triangle wave; Linux recomputes all three (B).
- Real-time mode: frames become due on a fixed schedule like an ADC; with no credit or a busy source they are dropped and counted, and the sequence number shows the gap. Four DMA destinations rotate so software can keep up to four transfers armed (B).
- The U54_4 context exists as FreeRTOS (in the SD image) and as bare metal (separate payload for comparison) running the same supervisor (B).

Planned in the M0 draft but not built: a separate FIFO stage and an NCO/sine source. Burst mode is paced by software and loses nothing; real-time mode is where drops happen and are counted.

## 2. Capability inventory

| Item | On this board | We use |
|---|---|---|
| Harts | E51 + 4x U54 (V) | E51: HSS. U54_1-3: Linux. U54_4: FreeRTOS (B) |
| DDR | 1 GB DDR4 (V); Linux saw 545 MB on the baseline image (B) | FreeRTOS image and RPMsg regions at 0x91C0_0000..0x91D5_0FFF; stream descriptor and buffers in the non-cached window at 0xC800_0000, 32 MiB (B) |
| Boot storage | eNVM + microSD only (V) | HSS in eNVM (untouched by us), everything else on the SD card (B) |
| Debug / programming | embedded FlashPro5, USB 1514:2008 (B) | FlashPro Express batch over USB interface 0 |
| UART (USB interfaces 1-3) | MMUART1, MMUART4, MMUART0 through fabric (B with the v2026.04 fabric) | HSS console, Linux console (ttyS1), FreeRTOS console |
| Fabric | 93,516 logic elements (BUILD) | 24,334 LE = 26.0 % in the project design v0.5.1 (histograms in RAM blocks; v0.4.0 used 29.2 % with register histograms) (BUILD) |
| MSS-fabric | FIC0 AXI4, FIC3 APB, F2M interrupts (V) | FIC3: source registers at 0x4000_0F00 and Mi-V IHC at 0x5000_0000. FIC0: DMA registers at 0x6001_0000 and DMA writes to DDR. F2M[2] = DMA interrupt (PLIC 120), F2M[1] = latency channel 0 to Linux (PLIC 119), F2M[5] = latency channel 1 to FreeRTOS (PLIC 123), F2M[60..62] = IHC (PLIC 178-180) (B) |
| Ethernet | 1 GbE (V) | SSH for deployment and test automation (B) |
| Expansion, transceivers, MIPI | present (V) | not used |

Tools and pins: Libero SoC 2026.1 with a node-locked-to-this-PC floating licence, reference design v2026.04, HSS v2026.04.1 (as found in the board's eNVM), meta-mchp linux4microchip-2026.04 (Scarthgap), Linux 6.18.17, U-Boot v2025.07-mchp, polarfire-soc-amp-examples v2025.07 + the project's firmware overlay (`firmware/u54_4-supervisor`), CoreAXI4DMAController 2.2.107, MIV_IHC 2.0.100, Icarus Verilog 12.0.

## 3. Hart and resource ownership

| Resource | Owner | Notes |
|---|---|---|
| E51 | HSS, M-mode | Boots the payload from the SD card, starts all harts, keeps running (B) |
| U54_1-3 | Linux SMP, S-mode | `cpu4` disabled in the device tree (B) |
| U54_4 | FreeRTOS (or the bare-metal build), M-mode | Started by HSS at boot (early boot, `skip-opensbi`); Linux attaches through remoteproc. Remoteproc can stop it but not restart it (HSS rejects the restart request, see section 7) (B) |
| Third context | none | Two contexts is the vendor maximum (V) |
| 0x91C0_0000, 1 MiB | FreeRTOS image | reserved, no-map (B) |
| 0x91D0_0000 / 0x91D0_8000 / 0x91D1_0000 / 0x91D5_0000 | RPMsg vrings, buffer pool, resource table | shared Linux/FreeRTOS; FreeRTOS copies its resource table to 0x91D5_0000 at start (B) |
| 0xC800_0000, 32 MiB non-cached | stream descriptors (offset 0, 0x40 apart in real-time mode) and burst/frame buffers | Fabric DMA writes, Linux reads through `/dev/udmabuf-ddr-nc0` (B) |
| 0x8800_0000, 32 MiB cached | shared-memory test region | Linux through `/dev/udmabuf-ddr-c0`, U54_4 through its cached window; coherent between harts without cache maintenance (B) |
| Stream source registers 0x4000_0F00 | Linux configures and starts; FreeRTOS only issues SNAPSHOT and reads | UIO device `pfdaq_stream_src` for Linux; direct access from hart 4 works (B) |
| DMA controller 0x6001_0000 | Linux | UIO device `dma-controller@60010000`, kernel driver `uio-microchip-dma` (B) |
| Latency channel 0 (0x4000_0F80), F2M[1] | Linux | configured, acknowledged and read by `pfdaq_lat` through the same UIO device (B) |
| Latency channel 1 (0x4000_0FC0), F2M[5] | U54_4 acknowledges in its interrupt handler or (FreeRTOS) in a task; Linux configures and reads statistics | PLIC source 123 enabled only for hart 4 (B) |
| Kernel driver `pfdaq_uio` (optional) | Linux | Same UIO device as `uio_pdrv_genirq`, but can acknowledge latency channel 0 inside the kernel interrupt handler; bound only for that measurement (B) |
| Mi-V IHC 0x5000_0000 | HSS + both contexts | mailbox for RPMsg (B) |
| MMUART1 / MMUART4 / MMUART0 | HSS / Linux / FreeRTOS | (B) |
| SD card, Ethernet | Linux | |

Shared-register caveat: SNAPSHOT is the only source control FreeRTOS writes. It is an idempotent pulse, so a Linux snapshot and a FreeRTOS snapshot cannot corrupt each other, but each side must take its own snapshot before reading.

## 4. Boot flow (B)

1. System Controller configures the fabric and releases the MSS.
2. E51: HSS inflates from eNVM, trains DDR, opens its console on MMUART1, initialises Mi-V IHC.
3. HSS reads the payload from SD partition 2 and loads two images: U-Boot (0x8020_0000, harts 1-3, S-mode through OpenSBI) and the FreeRTOS ELF (0x91C0_0000, hart 4, M-mode, no OpenSBI).
4. FreeRTOS starts immediately, prints its banner on MMUART0 and waits for the RPMsg link.
5. U-Boot loads `fitImage` from SD partition 1; Linux boots with 3 CPUs, console on MMUART4.
6. Linux remoteproc attaches to the running context; FreeRTOS announces the RPMsg channel `pfdaq-supervisor`.

Warm reboot to SSH reachable: 46-61 s (30-cycle test). What needs what: fabric = FlashPro programming (FABRIC + SNVM components only; eNVM/HSS never touched). Payload, kernel, device tree, rootfs, FreeRTOS = SD card only, and all of it can also be updated over SSH on a running board.

Recovery: the SD images and programming jobs of each stage are kept outside the repository (vendor Linux image, first AMP image, v0.3 image, current image with fabric job, kernel module and bare-metal payload). The fabric that was on the board as delivered cannot be restored (no copy exists).

## 5. Fabric design

```
                 FIC3 APB, 50 MHz                       FIC0 AXI4, 125 MHz
MSS ---------------------------------> pfdaq_stream_src ---- AXI4-Stream 64 bit ----> CoreAXI4DMAController ----> MSS FIC0 ----> DDR
       0x4000_0F00 registers             (project RTL)        TVALID continuous         0x6001_0000, F2M[2]
                                          + pfdaq_fir_src      TDEST 0-3 (real-time)
       0x4000_0F80 / 0x4000_0FC0 ------> pfdaq_latency_ch x2 ---- F2M[1] (Linux), F2M[5] (FreeRTOS)
```

`pfdaq_core.v` is the top level: it decodes the APB slot into the stream source (0x00-0x7F) and the two latency channels.

- Base: Discovery Kit reference design v2026.04, unmodified except for the integration script `hw/libero/overlay/PFDAQ.tcl` (derived from the vendor AXI4-Stream demo script).
- `hw/rtl/pfdaq_stream_src.v` v0.5.1: APB register block in the 50 MHz domain, generator in the 125 MHz domain, pulses crossing as toggles through 2-FF synchronisers, status crossing as a snapshot copied on a synchronised toggle. Run states: idle, FIR prefill, run, gap, real-time wait, zero fill.
- `pfdaq_fir_src` (same file): two triangle-wave samples per clock into a 4-stage pipeline (window, 16 multiplies, two adder levels) and a 16-word FIFO; a credit rule stops it overrunning the FIFO, and the generator waits for 12 words before a packet starts so the FIFO never runs dry mid-packet.
- Two frame words are generated per clock so that TVALID never drops inside a packet (required by the DMA core in practice, see section 7).
- `hw/rtl/pfdaq_latency.v`: entirely in the 50 MHz domain (20 ns resolution), no clock-domain crossing. Histograms in two RAM copies (one read-modified-written per sample, one for APB reads); CLEAR is a 64-cycle sweep.
- Constraints: `hw/libero/overlay/pfdaq_cdc.sdc` declares the quasi-static and synchronised crossings as false paths. Without it, every violating path the tool lists (the 20 worst in each report, 40 in all) is one of those crossings; the tool does not list the rest, so this is a check of the worst paths, not of all of them. The build script must list it together with every I/O and floorplan PDC, because `organize_tool_files` replaces a tool's file list: the first v0.5.0 build lost its pin constraints that way and the board did not boot past HSS.
- Timing: max and min delay met in all three corners (BUILD). v0.5.1 = design version 6, checksum d78d. History: v0.3.0 version 2 (5751), v0.4.0 version 3 (f5f6).

## 6. Hardware/software contract (as built, v0.5.1)

### 6.1 Stream source registers (APB, base 0x4000_0F00, 32-bit little-endian)

| Off | Name | Access | Meaning |
|---|---|---|---|
| 0x00 | ID | RO | 0x50464451 |
| 0x04 | VERSION | RO | 0x00050000 (v0.4.0: 0x00040000) |
| 0x08 | CTRL | WO pulses / RW | bit0 START, bit1 SNAPSHOT, bit4 ABORT, bit5 FORCE_ABORT; bit8 TLAST_PER_FRAME |
| 0x0C | PAYLOAD_WORDS | RW | payload words per frame, 1..65535 |
| 0x10 | FRAME_COUNT | RW | frames (burst) or frame slots (real-time) per START |
| 0x14 | GAP_CYCLES | RW | idle cycles between packets (TLAST_PER_FRAME mode only) |
| 0x18 | FAULT | RW | bit0 corrupt CRC, bit1 skip one sequence number, bit2 flip a payload bit; [31:16] index of the sent frame |
| 0x1C | STATUS | RO | bit0 busy, bit1 done (cleared by START), bit2 last run aborted, bit3 FIR underrun seen |
| 0x20 | FRAMES_SENT | RO | snapshot: frames completed since reset |
| 0x24 | WORDS_SENT | RO | snapshot: 32-bit words produced |
| 0x28 | STALL_CYCLES | RO | snapshot: cycles held off by backpressure |
| 0x2C | TIMESTAMP | RO | snapshot: 125 MHz cycle counter |
| 0x30 | NEXT_SEQ | RO | snapshot: sequence number of the next frame |
| 0x34 | MODE | RW | [1:0] payload 0 counter, 1 PRBS, 2 FIR; bit4 REALTIME; [9:8] destinations-1 |
| 0x38 | RATE | RW | real-time slot interval, 125 MHz cycles |
| 0x3C | CREDIT | W / R | write: [15:0] credits to add (bit31: replace); read: snapshot of credits held |
| 0x40 | DROPS | RO | snapshot: real-time frames dropped |
| 0x44 | FIR_STEP | RW | triangle phase increment per sample |
| 0x48-0x64 | COEF0-7 | RW | signed 16-bit FIR coefficients, tap 0 = newest sample |

Rules: configuration must not be written while busy; snapshot registers change only on SNAPSHOT and at the end of a burst; the sequence counter is never reset by START; 32-bit accesses only (byte-wise writes corrupt registers, which is why Python uses `libmmio.so`).

Differences from the M0 draft contract: no IRQ_STAT/IRQ_MASK (completion comes from the DMA), latency registers in their own block, 32-bit instead of 64-bit timestamp, RATE and DROPS apply to real-time mode only.

### 6.1b Latency unit registers (channel 0 at 0x4000_0F80, channel 1 at 0x4000_0FC0)

| Off | Name | Access | Meaning |
|---|---|---|---|
| 0x00 | CTRL | RW / WO pulses | bit0 ENABLE, bit1 ONESHOT, bit2 CLEAR statistics, [31:16] JITTER_MASK |
| 0x04 | PERIOD | RW | 50 MHz cycles between events |
| 0x08 | COUNT_LIMIT | RW | events per run, 0 = unlimited |
| 0x0C | ACK | WO | any write acknowledges: drops the interrupt and records the latency |
| 0x10 | STATUS | RO | bit0 interrupt pending, bit1 enabled, bit2 CLEAR in progress |
| 0x14 / 0x18 / 0x1C | EVENTS / ACKS / OVERRUN | RO | raised, acknowledged, due while the previous one was still pending |
| 0x20 / 0x24 / 0x28 | LAT_LAST / LAT_MIN / LAT_MAX | RO | cycles |
| 0x2C / 0x30 | LAT_SUM_LO / HI | RO | sum of all latencies |
| 0x34 / 0x38 | HIST_SEL / HIST_DATA | RW / RO | 64 bins, two per octave |
| 0x3C | SPURIOUS | RO | ACK writes with nothing pending |

Latency is counted from the first cycle the interrupt line is high to the APB write of ACK. The interrupt is level and stays high until acknowledged. Under Linux each event causes a second, empty UIO wake-up; software must acknowledge only when STATUS bit0 is set.

### 6.2 Frame format (32-bit words, little-endian in memory)

| Word | Content |
|---|---|
| 0 | 0xA55A0000 \| payload_words |
| 1 | sequence number (+1 per frame, and +1 per dropped slot in real-time mode) |
| 2 | timestamp: 125 MHz cycle counter when the frame started (burst) or became due (real-time) |
| 3..N+2 | payload: counter {seq[15:0], i[15:0]}; PRBS xorshift32 seeded with seq ^ 0x9E3779B9; FIR y[n] = sum c[k]·x[n-k], x[i] = tri((i+1)·STEP), continuous across the frames of a run |
| N+3 | CRC-32 (IEEE 802.3) over words 0..N+2, computed over the intended payload so a payload fault is detectable; equals `zlib.crc32` of the raw bytes |

### 6.3 DMA usage rules (all B, found by test)

1. One stream descriptor at the start of the non-cached window: +0 config (0xD = valid, destination ready, incrementing), +4 byte count, +8 destination. Its address goes to STREAM_0_ADDR_REG (0x460).
2. **Arm first, then START.** A descriptor re-armed while a packet is already waiting is not picked up, and a changed destination is ignored.
3. One burst = one packet = one DMA transaction, up to 8 MB. Verified up to 8,192,000 bytes.
4. Completion = DMA interrupt (blocking read on the UIO device) plus the core clearing the descriptor's ready bit. The kernel driver clears the status flag itself, so userspace sees INTR_0_STAT = 0x210 (stream descriptor) with no flag bits.
5. **An early TLAST does not complete a transfer**; only the full byte count does. So ABORT in burst mode finishes the current frame and then sends zero words up to the run's full length (FRAMES_SENT says how many frames are real). In real-time and per-frame modes every frame is its own transfer and ABORT simply stops after it. FORCE_ABORT can still leave the DMA waiting (reboot to recover).
6. **Real-time mode with four destinations works**: descriptors for TDEST 0-3 are armed up front, the fabric sends a frame only while it holds a credit, and software returns one credit per re-armed descriptor. Completed transfers are counted with the UIO event counter (`/sys/class/uio/uioN/event`).
7. **Known defect: odd frame lengths in multi-frame bursts.** With an odd number of 32-bit words per frame and several frames per transfer, 2 KB chunks are lost or misplaced at 2/4 KB destination boundaries, although the stream is correct in simulation, timing is met and the DMA reports completion. Repeated occurrences can disturb later transfers until a reboot. Even frame lengths are clean in every test; `pfdaq_cap` refuses the odd case unless `-O` is given. Root cause not found (DMA or interconnect; would need the vendor DMA RTL in simulation).

### 6.4 RPMsg supervision protocol (channel `pfdaq-supervisor`, 32-bit little-endian words)

- Linux to U54_4: `{magic 'PFDS', type, burst, next_seq, errors, deadline_ms, max_lag}`; types ARM=1, STATUS=2, DISARM=3, QUERY=4, LATMODE=5 (deadline_ms: 0 interrupt handler, 1 task acknowledges latency channel 1), MEMCHK=6 (burst = physical address, next_seq = words, errors = pattern seed).
- U54_4 to Linux, one per message: `{magic 'PFDA', type, state, status_rx, deadline_miss, seq_mismatch, fabric_seq, fabric_frames, uptime_ms}`; states IDLE=0, ARMED=1, ALARM=2; uptime_ms bit31 set = bare-metal build; for MEMCHK fabric_seq = mismatching words, fabric_frames = words checked.
- While ARMED, no STATUS within `deadline_ms` raises ALARM and counts a miss; the next STATUS returns to ARMED.
- On every STATUS the supervisor snapshots the fabric and requires `0 <= fabric_seq - next_seq <= max_lag` (the fabric may be one burst ahead of what Linux has verified, never behind).

## 7. Risks: status of the M0 list, and what replaced it

| M0 risk | Outcome |
|---|---|
| No vendor AMP support for this board | Resolved: machine, payload, device tree and FreeRTOS board ported in `yocto/meta-pfdaq` and `firmware/u54_4-supervisor` (B) |
| Fabric interrupt to FreeRTOS while Linux owns the PLIC | Resolved: F2M[5] is taken by hart 4 in M-mode, 0.52 us typical, no interference seen with Linux (B) |
| Stream-to-DDR DMA under Linux | Resolved with the rules in 6.3 (B) |
| HSS in eNVM may not match | HSS was already v2026.04.1 (B). The **fabric** did not match v2026.04 and was replaced |
| Recovery only by SD swap or reflash | Unchanged; mitigated by SSH updates and kept images |
| Yocto build resources | Resolved: full image builds on this PC; about 20 GB disk left |

Current risks and known issues:

1. **Odd frame lengths in multi-frame bursts corrupt data** (6.3 rule 7). Avoided in software; root cause open.
2. **Remoteproc cannot restart U54_4**: stop works, start fails because HSS v2026.04.1 rejects the boot message address ("rproc buffer not in DDR"). Fixing it means rebuilding HSS and reprogramming eNVM, which this project does not touch. Recovery is a reboot.
3. **One unexplained FreeRTOS no-start in about 60 boots** (hart reported Running, no console output, no channel). Not seen since, including 30/30 and 6/6 reboot tests.
4. DMA behaviour in 6.3 is established by test on one core version, not by vendor documentation.
5. Cold power cycles: two tested (two fresh-card boots).
6. End-to-end verified throughput (about 29 MB/s) is limited by one Linux core copying and checking; the fabric side runs at about 930-950 MB/s. In real-time mode this shows up as drops from about 50 us/KiB slots upward.
8. **Latency unit: an interrupt that is pending across CLEAR and acknowledged during the 64-cycle clear sweep is counted in ACKS and LAT_LAST but not in the histogram** (the sweep has priority on the single RAM write port). Found by reading the RTL and a scratch simulation (module H08); not fixed. `pfdaq_lat` avoids it by stopping, acknowledging any stale interrupt, then clearing.
7. Linux shows occasional 1-5 ms stalls in interrupt response (kernel handler included), so Linux is not a hard real-time responder here; the U54_4 context never missed an event in any test.
