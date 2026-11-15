---
id: H04
title: "CRC-32 at 64 bits per clock"
track: fpga
level: 3
summary: Computing a standard CRC over two words per cycle and matching it against zlib.
needs: [H02]
repro: sim
status: validated
hardware: [pfdaq-stream-src]
software: [pfdaq-cap]
sources:
  - hw/rtl/pfdaq_stream_src.v:pfdaq_word_step,crc32_word,step_a,step_b,crc_final,crc_word,pay_good
  - hw/tb/check_trace.py:zlib,struct
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:crc32_words,bad_crc
evidence: [sim-stream, suite-freertos]
related: [H02, H05, H11, P03]
---

# CRC-32 at 64 bits per clock

After this module you can explain what a CRC-32 protects and how a hardware version of it handles a whole word, and later two words, in a single clock. You can show that the hardware result equals Python's `zlib.crc32` of the same bytes, and you can see why this design computes the CRC over the payload it *meant* to send rather than the one it sent.

## Before you start

- **Level 3.** You should have read module H02 (the stream is two words per beat) and be comfortable with XOR and shifts.
- **Tools:** Icarus Verilog 12 and Python 3. No board.
- **Time:** about five minutes, plus the experiments.

## The idea

A **cyclic redundancy check** is a short checksum, here 32 bits, computed over a block of data. The sender appends it; the receiver recomputes it over the data it received and compares. A single flipped bit, or any burst of up to 32 flipped bits, always changes the result. It is not a security feature, since anyone can recompute it, but it is the standard cheap way to detect accidental corruption.

CRC-32 as used in Ethernet, zip and PNG ("IEEE 802.3") is defined by four choices:

| Choice | Value |
|---|---|
| Polynomial, in reflected (bit-reversed) form | `0xEDB88320` |
| Starting value of the register | `0xFFFFFFFF` |
| Bit order | least significant bit of each byte first |
| Final step | invert every bit of the register (XOR with `0xFFFFFFFF`) |

The definition works one bit at a time. For each data bit `d`, with `r` the register:

```
if (r[0] XOR d) r = (r >> 1) XOR 0xEDB88320
else            r =  r >> 1
```

Software usually speeds this up with a lookup table. Hardware has a different option: the bit-serial rule is only shifts and XORs, so any number of steps in a row collapse into a fixed network of XOR gates from the old register and the new data bits to the new register. Eight steps give a byte-at-a-time CRC, thirty-two steps a word-at-a-time CRC. No table and no loop exists at run time.

## On this board

### The per-word update

The frame generator is a combinational module, `pfdaq_word_step` in `hw/rtl/pfdaq_stream_src.v`. Its CRC part is this function, which processes 32 data bits, least significant first:

```verilog
function [31:0] crc32_word;
    input [31:0] c;
    input [31:0] d;
    integer i;
    reg [31:0] r;
    begin
        r = c;
        for (i = 0; i < 32; i = i + 1) begin
            if (r[0] ^ d[i])
                r = (r >> 1) ^ 32'hEDB88320;
            else
                r = r >> 1;
        end
        crc32_word = r;
    end
endfunction
```

This is the bit-serial rule from the previous section written as a loop. The loop has fixed bounds, so the synthesis tool unrolls it into one XOR network of 32 stages' worth. Nothing in it is clocked; the module is "register in, register out, with this network in between".

Thirty-two bits in little-endian order, least significant bit first, is the same as feeding the four bytes of the word to the byte-wise definition, lowest address first. That is why the result equals `zlib.crc32` of the frame as it sits in memory (a plain array of little-endian 32-bit words): the hardware and Python process the same bit sequence.

### What is covered, and what the register does

A frame has `N + 4` words: header, sequence number, timestamp, `N` payload words, then the CRC. The CRC covers the first `N + 3`. In `pfdaq_word_step` each phase of the frame feeds the register through `crc_in`:

```verilog
P_MAGIC: begin word = {MAGIC_HI, cfg_payload}; crc_in = word; ...
P_SEQ:   begin word = seq;                     crc_in = word; ...
P_TIME:  begin word = frame_ts; crc_in = word; ...
P_PAY:   begin word = pay_word; crc_in = pay_good; ...
default: begin // P_CRC
    word = crc_word; ...
n_crc = (ph == P_CRC) ? 32'hFFFFFFFF : crc32_word(crc, crc_in);
```

- The register starts at `0xFFFFFFFF` at reset and at every START. After the CRC word it is set to `0xFFFFFFFF` again, so every frame is checked independently.
- The CRC word sent is `crc_final = ~crc`, the final inversion.

### Two words per clock

The stream is 64 bits wide, so one beat carries two words (module H02). The source therefore instantiates the word step twice and chains them in one clock:

```verilog
pfdaq_word_step step_a (.ph(ph), .crc(crc), ... .n_crc(a_crc), ...);   // even word
pfdaq_word_step step_b (.ph(a_ph), .crc(a_crc), ... .n_crc(b_crc), ...); // odd word
```

Instance B starts from the state that A leaves: `a_ph`, `a_crc`, `a_seq` and so on. Between two clock edges a signal therefore travels through A's `crc32_word`, then B's. If A has just consumed the last payload word and B's phase is the CRC word, B's output is simply `~a_crc`, so the whole CRC network of A sits in front of the output register that drives the stream.

The cost is logic depth: two XOR networks in series, plus for the PRBS mode the xorshift step in front of A's CRC input, all inside the 8 ns of one 125 MHz period. The number of logic levels was not measured separately. What the published records show is the total: the full design meets timing in all corners, with a worst slack of 1.342 (Libero reports it in nanoseconds) for the multi-pass run, and uses 26.02 % of the fabric's logic elements. That slack belongs to the whole design, not to the CRC path in particular.

### Where fault injection goes

The CRC register is fed with `pay_good`, the payload the frame is *supposed* to have, while the word actually sent is `pay_word`, which has bit 8 flipped when a payload fault is injected into this frame:

```verilog
wire [31:0] pay_word  = (fault_here && cfg_fault_en[2] && (pay_idx == 16'd0)) ? (pay_good ^ 32'h00000100) : pay_good;
```

Because the CRC covers `pay_good`, a payload fault produces a frame whose payload does *not* match its CRC. The receiver sees both a payload mismatch and a CRC error, from two independent checks. The other CRC fault is simpler: `crc_word` flips bit 0 of the CRC word itself.

### The checks in software and in the Python model

Software has the same job and does it with a table. `pfdaq_cap.c` has `crc32_words`, a slicing-by-4 version (four 256-entry tables, four bytes per step), and counts a bad frame when the result differs from the last word.

The simulation checker, `hw/tb/check_trace.py`, uses the most independent route available: it packs the first `N + 3` words into little-endian bytes and asks Python's `zlib`:

```python
if zlib.crc32(struct.pack('<%dI' % (flen - 1), *w[:-1])) != w[-1]:
    det['crc'].append(i)
```

It also knows what *should* be detected. Each scenario states which frame carries an injected fault; the checker computes the set of frames expected to fail the CRC (any fault that corrupts the CRC word or the payload) and fails the run if the detected set differs, in either direction.

## Run it

1. From the repository root, run the stream simulation into a directory of your own:

   ```sh
   make -C hw sim-stream OUT=/tmp/pfdaq-h04
   ```

2. Recompute one CRC yourself. The trace file lines `B <scenario> <word> <tlast> <time> <tdest>` list every word that crossed the stream. Take the first twelve words of scenario 1, which are one frame of eight counter-payload words:

   ```sh
   python3 - <<'EOF'
   import zlib, struct
   w = []
   for line in open('/tmp/pfdaq-h04/sim/trace.txt'):
       p = line.split()
       if p and p[0] == 'B' and p[1] == '1':
           w.append(int(p[2], 16))
           if len(w) == 12:
               break
   print([hex(x) for x in w])
   print('crc in stream  : %08x' % w[-1])
   print('zlib.crc32     : %08x' % zlib.crc32(struct.pack('<11I', *w[:-1])))
   EOF
   ```

3. **Break it: restart value.** In a scratch copy of `hw/`, change the value the register is reloaded with after each frame, in the last line of the `always @(*)` block:

   ```verilog
   n_crc = (ph == P_CRC) ? 32'h00000000 : crc32_word(crc, crc_in);
   ```

   and run `make sim-stream OUT=...` in the copy, with an output directory of your own. The repository itself is never touched.

4. **Break it: what the CRC covers.** In another scratch copy, change `crc_in = pay_good;` in the payload phase to `crc_in = pay_word;`, so the CRC covers what is sent, faults included, and run the simulation again.

## What you should see

Step 1 ends with `RESULT: PASS` after 20 scenario lines.

Step 2, in the run made for this page:

```
['0xa55a0008', '0x0', '0x116', '0x0', '0x1', '0x2', '0x3', '0x4', '0x5', '0x6', '0x7', '0xb023c43c']
crc in stream  : b023c43c
zlib.crc32     : b023c43c
```

The words are the header `0xA55A0008` (magic and payload length 8), sequence number 0, the timestamp counter value `0x116`, eight counter-payload words, and the CRC. The CRC computed in hardware and the CRC computed by Python's `zlib` are the same number.

Step 3 fails, and the checker says where. The first frame still passes, because START loads the register with `0xFFFFFFFF` by a different path; every following frame is wrong. From the run made for this page:

```
RESULT: FAIL
   scenario 1: detected {'crc': [1, 2, 3, 4], 'seq': [], 'pay': []}, expected {'crc': [], 'seq': [], 'pay': []}
   scenario 2: detected {'crc': [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19], ...
   scenario 3: detected {'crc': [1, 2, 3, 4, 5], 'seq': [], 'pay': []}, expected {'crc': [2], ...
```

Scenario 3 is the useful one. It injects one CRC fault into frame 2, and the checker expects frame 2 to fail. With the wrong restart value it sees frames 1 to 5 fail, so the fault in frame 2 is buried in noise. A CRC that is wrong everywhere detects nothing.

Step 4 also fails, and for the opposite reason: the payload fault is now *included* in the CRC, so the frame is internally consistent and the CRC check passes:

```
RESULT: FAIL
   scenario 5: detected {'crc': [], 'seq': [], 'pay': [0]}, expected {'crc': [0], 'seq': [], 'pay': [0]}
   scenario 15: detected {'crc': [], 'seq': [], 'pay': [1]}, expected {'crc': [1], 'seq': [], 'pay': [1]}
```

The payload mismatch is still caught, but only by the model's own recomputation of the payload. The CRC no longer catches a fault in the payload, which is the case it exists for. The original choice protects against a source that corrupts its own data after computing it.

## Evidence

- **Simulation:** all 20 numbered scenarios pass against the checker's independent recomputation, including the CRC fault in scenario 3 (`detected crc=[2]`) and the payload fault in scenario 5 (`detected crc=[0] ... pay=[0]`), where one injected fault is seen by both the CRC check and the payload check. Record `sim-stream`, command `make -C hw sim-stream`, fabric v0.5.1. The two experiments above were run for this page on scratch copies and are not part of that record.
- **Board, fabric v0.5.1:** the Linux tool's table-driven CRC agrees with the hardware's on every frame of the board suite. Test `T9-crc-counter` injects 4 CRC faults and the tool counts exactly 4 CRC errors; `T9-pay-counter` injects 2 payload faults and counts 2 payload errors and 2 CRC errors, and the same holds for the PRBS and FIR payloads. Record `suite-freertos`.

What these results do not show: the simulation checks the logic, not the delay of the XOR networks. Timing was checked by the place-and-route tool in the build record, not here.

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| The CRC of the first frame is right and every later one is wrong | The register is not restored to `0xFFFFFFFF` after a frame (step 3) |
| Every CRC is wrong by the same inversion | The final `~` is missing, or applied twice |
| CRC passes but the payload check fails | The CRC covers the sent word, not the intended one (step 4) |
| Python disagrees with a correct frame | Words packed big-endian (`'>%dI'`) instead of little-endian (`'<%dI'`) |
| A design that worked at 1 word per clock fails timing at 2 | The chained networks are too deep for the period; see the trade-offs below |

## Trade-offs and going further

- **Parallelism costs logic and time, never correctness.** One word per clock would halve the network depth and the stream rate. Two words per clock keeps the stream continuous (module H02) at the price of two networks in series. In general a wider step is built either by chaining, as here, or by deriving one larger XOR network for the whole step, which is shallower but bigger; this design uses chaining.
- **The CRC in the middle of a chain cannot be pipelined freely.** The register depends on the previous word, so the dependency runs through every word of the frame. The design accepts one long path instead.
- **Detection is not correction, and not security.** CRC-32 catches accidental damage with very high probability; it does not stop deliberate changes.
- **Exercise:** work out what the 32-bit result would be for a frame of one payload word with the checker's model, then change the testbench to send that frame and confirm.
- **Exercise:** replace `crc32_word` with a byte-wise version used four times and run the simulation. Does the checker see any difference? What does your answer say about the structure of the checker?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md) |
| **How to run** | Simulation only: Icarus Verilog and Python, no board |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `pfdaq_word_step`, `crc32_word`, `step_a`, `step_b`, `crc_final`, `crc_word`, `pay_good`<br>[`hw/tb/check_trace.py`](../../hw/tb/check_trace.py): `zlib`, `struct`<br>[`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `crc32_words`, `bad_crc` |
| **Evidence** | [Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [H02 AXI4-Stream: the handshake, backpressure and TLAST](H02-axi4-stream.md)<br>[H05 A FIR filter on math blocks, decoupled by a FIFO](H05-fir-on-math-blocks.md)<br>[H11 Verification with an independent model](H11-independent-model.md)<br>[P03 Fault injection from the fabric up](P03-fault-injection.md) |
| **Status** | validated |
<!-- nav:end -->
