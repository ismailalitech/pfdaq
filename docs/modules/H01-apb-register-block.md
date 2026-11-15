---
id: H01
title: "An APB register block that software can trust"
track: fpga
level: 2
summary: Read-only identity, pulse bits, snapshot registers and the rules that keep software and hardware in agreement.
repro: sim
status: validated
hardware: [pfdaq-stream-src, fic3]
sources:
  - hw/rtl/pfdaq_stream_src.v:pfdaq_stream_src,apb_write,prdata_mux,credit_val_p,snapv_pulse_p
  - hw/rtl/pfdaq_core.v:sel_src
  - hw/tb/tb_pfdaq_stream_src.v:apb_wr,apb_rd,snapshot
  - hw/tb/check_trace.py
evidence: [sim-stream]
related: [H02, H03, I01, K02]
---

# An APB register block that software can trust

After this module you can read the register block of the stream source as a contract: which registers tell software what it is talking to, which ones only take effect at a defined moment, which ones can be read consistently, and which accesses are forbidden. You will also have read real register traffic from a simulation and seen the contract catch a mistake.

## Before you start

- **Level 2.** You should read simple Verilog (registers, `case`, `assign`).
- **Tools:** Icarus Verilog 12 and Python 3. No board.
- **Time:** the simulation runs in under a minute.

## The idea

Software on a processor and logic in an FPGA run on different clocks and at different speeds. They agree on how to talk through **registers**: addresses that the processor reads and writes and that the logic decodes. A register block is only useful if both sides mean the same thing by every access. Five habits make that agreement hold up:

1. **Say what you are.** A read-only identity register lets software check that the address it is about to use really holds this block, and a version register says which contract applies.
2. **Take effect at one defined moment.** If a run reads its configuration while software is half way through changing it, the result is a mixture of two configurations. Configuration is therefore sampled once, at START.
3. **Make actions pulses, not levels.** "Start" is an event. A bit that stays high would have to be cleared again, and a missed clear would start a second run.
4. **Read related values at one instant.** If five counters are read one after another while the hardware keeps counting, the five numbers do not belong together. A *snapshot* freezes them all first.
5. **Write down the forbidden accesses.** Whatever hardware cannot defend against, the contract has to say.

The bus that carries the accesses here is **APB**, the simplest of the AMBA buses. A transfer has two phases. In the *setup* phase the master drives the address and `PSEL`; in the *access* phase it also raises `PENABLE`, and the transfer completes on the clock edge where the slave signals `PREADY`.

## On this board

The register block is the top half of `pfdaq_core`'s APB slot: offsets `0x00` to `0x7F` of `0x4000_0F00`, reached through FIC3 at 50 MHz. The upper half belongs to the two latency channels (module H07). `pfdaq_core` selects the stream source with `PSEL & ~PADDR[7]` and passes the low eight address bits on.

### How this slave implements APB

| Aspect | What the RTL does |
|---|---|
| Write | Taken on the clock edge where `PSEL & PENABLE & PWRITE` holds. The write data is stored in one step. |
| Read | `PRDATA` is a combinational multiplexer on `PADDR`. It is valid from the setup phase on, so the master can sample it in the access phase. |
| Wait states | None: `PREADY` is tied high. |
| Errors | None: `PSLVERR` is tied low. |
| Address decode | `PADDR[7:2]` only. Offsets `0x68` to `0x7C` read as 0 and ignore writes. The two low bits are not looked at. |
| Byte strobes | None. APB3 has no `PSTRB`, so every write replaces a whole 32-bit register. |

The last row has a practical consequence. A byte-sized store from the processor still reaches this slave as a write of the whole word. What `PWDATA` carries for the other three bytes depends on the bus bridge in front of the slave, which was not investigated; the early Python capture tool hit exactly this, and the register contract below says "32-bit accesses only" because of it (case study K02).

### The register table

Checked against `hw/rtl/pfdaq_stream_src.v` (the write `case` and the `prdata_mux` read multiplexer); it is the same table as section 6.1 of the [architecture document](../architecture.md).

| Offset | Name | Access | Kind | Meaning |
|---|---|---|---|---|
| `0x00` | ID | RO | identity | `0x50464451` |
| `0x04` | VERSION | RO | identity | `0x00050000` here, set by a parameter |
| `0x08` | CTRL | WO pulses, RW bit 8 | action | bit 0 START, bit 1 SNAPSHOT, bit 4 ABORT, bit 5 FORCE_ABORT; bit 8 TLAST_PER_FRAME |
| `0x0C` | PAYLOAD_WORDS | RW | configuration | payload words per frame; a write of 0 is stored as 1 |
| `0x10` | FRAME_COUNT | RW | configuration | frames (burst) or slots (real-time) per START; 0 is stored as 1 |
| `0x14` | GAP_CYCLES | RW | configuration | idle cycles between packets (per-frame TLAST mode) |
| `0x18` | FAULT | RW | configuration | bit 0 corrupt CRC, bit 1 skip one sequence number, bit 2 flip a payload bit; `[31:16]` frame index |
| `0x1C` | STATUS | RO | status | bit 0 busy, bit 1 done, bit 2 last run aborted, bit 3 FIR underrun seen |
| `0x20` | FRAMES_SENT | RO | snapshot | frames completed since reset |
| `0x24` | WORDS_SENT | RO | snapshot | 32-bit words produced |
| `0x28` | STALL_CYCLES | RO | snapshot | cycles held off by backpressure |
| `0x2C` | TIMESTAMP | RO | snapshot | 125 MHz cycle counter |
| `0x30` | NEXT_SEQ | RO | snapshot | sequence number the next frame will carry |
| `0x34` | MODE | RW | configuration | `[1:0]` payload kind, bit 4 real-time, `[9:8]` destinations minus one |
| `0x38` | RATE | RW | configuration | real-time slot interval in 125 MHz cycles; values below 2 are stored as 2 |
| `0x3C` | CREDIT | W adds / R snapshot | action and snapshot | write: `[15:0]` credits to add, bit 31 replaces instead; read: credits held |
| `0x40` | DROPS | RO | snapshot | real-time frames dropped |
| `0x44` | FIR_STEP | RW | configuration | triangle phase increment per sample |
| `0x48`–`0x64` | COEF0–COEF7 | RW | configuration | signed 16-bit FIR coefficients |

Reading a "write-only" register is not an error; it returns whatever the RTL puts there, which for CTRL is only bit 8.

### Why each kind exists

**Identity.** `ID` is a constant and `VERSION` a parameter. Linux checks `ID` before it touches anything else. The fabric build, the Linux tools and the supervisor on the real-time core all read it, so a stale bitstream or a wrong address shows up as a failed check at start-up, not as strange data later. When the contract changes, the version changes with it: the checker in this module's simulation refuses a version it was not written for (see the exercise below).

**Configuration, sampled at START.** The registers in the table marked *configuration* live in the 50 MHz domain. The stream logic in the 125 MHz domain copies them into its own `cfg_*` registers on the START pulse and uses only those copies for the rest of the run. Writing a configuration register during a run therefore changes nothing for that run, and changes it for the next one. The rule "do not write configuration while busy" keeps that simple: software that writes during a run is describing a run that has already started. How the copy crosses between the clocks is module H03.

**Pulses.** START, SNAPSHOT, ABORT and FORCE_ABORT are bits of CTRL, but the RTL does not store them. A write of 1 to such a bit flips a toggle flip-flop, and the stream side detects the flip as one event. Writing 0 does nothing. Only bit 8, TLAST_PER_FRAME, is stored. A START write also clears the `done` flag at once, so a loop that waits for `done` cannot be satisfied by the previous run.

**Snapshot registers.** `FRAMES_SENT` to `NEXT_SEQ` and `DROPS` are counters that run in the stream domain, and the bus cannot read them directly: the read path would have to cross the clock boundary in a single cycle. Instead the stream side copies them into a bank of 50 MHz registers when software writes SNAPSHOT or when a run ends, and APB reads see only that bank. This gives two properties at once. Reads are consistent, because all values in the bank belong to one instant, and the read path contains nothing but 50 MHz flip-flops, which keeps timing easy. The cost is that a read shows the situation at the last snapshot, not the present. Software that wants current values writes SNAPSHOT first and waits a few cycles.

**CREDIT is two registers at one address.** A write sends an amount to the stream side (the amount is held while the toggle crosses); a read returns the snapshot of the credits currently held. Bit 31 turns "add" into "replace". Module H06 explains what the credits are for.

### Versioning the contract

Every change to the table above that software can see raises `VERSION`. The history in [CHANGELOG.md](../../CHANGELOG.md) pairs each version with what changed: v0.5 added `MODE`, `RATE`, `CREDIT`, `DROPS`, `FIR_STEP` and the coefficients; the latency channels have their own block. Tools that need a feature check `VERSION` and refuse to run on hardware that lacks it.

## Run it

1. From the repository root:

   ```sh
   make -C hw sim-stream
   ```

   The testbench drives APB exactly as the processor does (`apb_wr` and `apb_rd` in `hw/tb/tb_pfdaq_stream_src.v`: setup phase, then access phase) and writes every read it makes to `build/sim/sim/trace.txt`.

2. Open the trace. Lines beginning `R` are register reads: scenario, offset, value. Lines beginning `P` are the snapshot reads taken just before START.

   ```sh
   grep -E '^(R|P) (0|1|2) ' build/sim/sim/trace.txt
   ```

3. Read the first scenario against the table. After five frames of twelve words, the snapshot registers hold 5 frames, 60 words and next sequence number 5, and STATUS shows `done` with `busy` clear.

4. Read the next scenario's `P` lines. They are the values left by scenario 1: the snapshot registers count since reset, not since START.

5. **Break the contract on purpose.** Copy `hw/` to a scratch directory, change the `VERSION_VALUE` that the testbench gives the design from `32'h00050000` to `32'h00050001`, and run `make -C <scratch>/hw sim-stream`. The design still works, but its version no longer matches what the checker expects.

## What you should see

Step 1 ends with `RESULT: PASS` after twenty scenarios, as on the [AXI4-Stream module](H02-axi4-stream.md).

The first lines of the trace are the identity and coefficient reads, taken before any run:

```
R 0 00 50464451
R 0 04 00050000
R 0 48 00002ee0
R 0 4c 0000c568
R 0 50 00004e20
R 0 54 00007fff
R 0 58 00008000
```

`50464451` is the ASCII text `PFDQ`. The coefficients read back as the values the testbench wrote: `0x2ee0` is 12000, `0xc568` is 50536, which as a signed 16-bit number is −15000, and `0x8000` is −32768. The upper 16 bits read as zero, so a signed coefficient is stored in 16 bits and read back unextended.

Scenario 1 and the start of scenario 2:

```
P 1 20 00000000        snapshot before the run: nothing sent yet
R 1 1c 00000002        STATUS: done, not busy
R 1 20 00000005        FRAMES_SENT = 5
R 1 24 0000003c        WORDS_SENT = 60 = 5 x (12 words)
R 1 30 00000005        NEXT_SEQ = 5
P 2 20 00000005        scenario 2 starts from the counts left by scenario 1
R 2 20 00000019        FRAMES_SENT = 25 = 5 + 20
```

In scenario 13 (real-time mode), `R 13 40 00000012` reads the DROPS snapshot: 18 frames dropped, which is the same 18 that the result line reports: `sent 12 dropped 18`.

Step 5 fails. In the recorded run the checker's last lines were:

```
RESULT: FAIL
   ID/VERSION readback {0: 1346782289, 4: 327681, 72: 12000, ...
```

`327681` is `0x00050001`. Nothing is wrong with the data; the contract check is what stopped. That is its job: it forces whoever changes the version to change the checker, and with it the documentation, in the same commit.

## Evidence

- **Simulation:** 20 scenarios, every register read in them recorded and compared by the Python model (`hw/tb/check_trace.py`), among them identity, coefficient read-back, snapshot values against the model's own count of frames, words and drops, and the status flags after normal, aborted and force-aborted runs. Record: [Stream source simulation](../../evidence/v0.5.1/sim/stream.txt).
- **Version-mismatch exercise:** run for this page in a scratch copy of the testbench (not recorded as evidence); it ended with `RESULT: FAIL` and the ID/VERSION message above, and the unmodified design passes.

Simulation shows that the register logic behaves as the table says. It does not show how the processor's bus bridge forms the accesses; the byte-store behaviour above was seen on the board.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| Registers read back garbage after a write from a script | The access was split into bytes, and the slave takes each as a whole-word write (K02). Use 32-bit stores. |
| A run uses the old configuration | The write came after START, or while busy; configuration is sampled at START only |
| `done` is already set when you start waiting | Not possible after START in this design, since START clears it; check that your loop reads after the START write |
| Counters appear not to move | You read before writing SNAPSHOT; the bank updates only on SNAPSHOT and at the end of a run |
| `FRAMES_SENT` is larger than the frames you asked for | The counters count since reset, over all runs |
| A write of 0 does not read back as 0 | `PAYLOAD_WORDS` and `FRAME_COUNT` store 0 as 1, and `RATE` stores anything below 2 as 2 |

## Trade-offs and going further

- **No wait states and no errors** make the slave trivial and make the bus fast, but they also mean a wrong address is silently accepted. A slave that raised `PSLVERR` for undefined offsets would catch software bugs earlier, at the cost of a decode term and an error path through the bus bridge.
- **Snapshots trade freshness for consistency.** Reading the counters live would need a multi-cycle read or a handshake with the stream domain; the snapshot keeps the APB side simple and puts the cost in software's two-step read.
- **Clamping on write** (0 becomes 1) means the configuration registers never hold a value the logic cannot use, but it also hides the mistake from the caller. Whether to clamp, ignore or flag an error is a design decision, and this design chose to keep the logic simple.
- **Exercise:** the table says reads of CTRL return only bit 8. Predict what a read of `0x68` returns, then check it by adding one `apb_rd(32'h68)` to a scratch copy of the testbench.
- **Exercise:** which registers would you have to freeze at START if you wanted a run's configuration to be readable back unchanged while it is running? Compare with what the RTL does now.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Simulation only: Icarus Verilog and Python, no board |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [FIC3 (APB, 50 MHz)](../system/README.md#fic3) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `pfdaq_stream_src`, `apb_write`, `prdata_mux`, `credit_val_p`, `snapv_pulse_p`<br>[`hw/rtl/pfdaq_core.v`](../../hw/rtl/pfdaq_core.v): `sel_src`<br>[`hw/tb/tb_pfdaq_stream_src.v`](../../hw/tb/tb_pfdaq_stream_src.v): `apb_wr`, `apb_rd`, `snapshot`<br>[`hw/tb/check_trace.py`](../../hw/tb/check_trace.py) |
| **Evidence** | [Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15) |
| **Related** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md)<br>[H03 Crossing clock domains: toggles, snapshots and constraints](H03-clock-domain-crossing.md)<br>[I01 Reaching fabric registers from Linux](I01-fabric-registers-from-linux.md)<br>[K02 Python wrote my registers one byte at a time](../case-studies/K02-bytewise-register-writes.md) |
| **Status** | validated |
<!-- nav:end -->
