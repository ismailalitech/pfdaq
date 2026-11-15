---
id: K02
title: "Python wrote my registers one byte at a time"
track: case-study
level: 2
summary: Why a 32-bit register write from Python reached the hardware as four writes.
repro: doc
status: validated
hardware: [pfdaq-stream-src, fic3]
sources:
  - tests/legacy/mmio.c:mmio_wr32,mmio_rd32
  - hw/rtl/pfdaq_stream_src.v:apb_write,PWDATA
evidence: [fabric-throughput-v030]
related: [H01, I01, K05]
---

# Python wrote my registers one byte at a time

The first board tool was written in Python because it is the quickest way to poke at memory-mapped registers. Its writes corrupted the fabric's registers. The fix was five lines of C. This page records what is known about the failure, which is less than one would like, and the rule that came out of it.

## Symptom

The first tool, `pfdaq_capture.py`, opened `/dev/mem` and mapped the stream source's registers with Python's `mmap`. Writes to the fabric registers did not behave: the notes record only the conclusion, that "Python mmap/ctypes stores are bytewise on this target and corrupt APB writes".

The detail of the first failing runs was not kept. This page therefore cannot show what a corrupted register looked like, and the numbers in older pages that mention the Python tool were all measured *after* the fix.

## Isolation

The fabric logic had already passed its simulation, so the suspicion fell on how software reached the bus.

Python writes into a mapped buffer by slice assignment or through a `ctypes` object. In both cases the interpreter copies bytes with a library routine (`memcpy`). The project's explanation, written into the helper's own source comment, is that on this RISC-V target that routine stores **one byte at a time**.

That explanation was not checked by disassembling the C library, and the failing bus traffic was not observed. It rests on one fact that was checked: replacing the write path with a single 32-bit store removed the problem.

## Cause

**Established:** a 32-bit register write made through Python's own mapped-memory stores did not work, and the same write made through a C function containing `*p = v` did.

**Why the slave cannot cope, from the RTL.** The register block is an APB3 slave. APB3 has no byte strobes: the slave's port list has `PWDATA[31:0]` but no `PSTRB`, and every write that reaches it is decoded as a write of the whole register.

```verilog
wire        apb_write = PSEL & PENABLE & PWRITE;
...
    8'h0C: payload_words <= (PWDATA[15:0] == 16'd0) ? 16'd1 : PWDATA[15:0];
    8'h10: frame_count   <= (PWDATA == 32'd0) ? 32'd1 : PWDATA;
```

Four byte stores to one register therefore become up to four whole-register writes. Which of the other 24 bits each of them carries across the interconnect was **not investigated**, so what the register held afterwards is not known. What is certain is that no byte store can change one byte of a register and leave the rest alone.

**Not established:** whether the bridge between the processor and the APB bus splits, merges or reorders such stores. This page does not claim a mechanism for it.

## Fix

Two steps, in order.

**1. A real 32-bit store.** The entire helper is two functions, built into a shared library the Python tool loads with `ctypes`:

```c
uint32_t mmio_rd32(const volatile uint32_t *p) { return *p; }
void mmio_wr32(volatile uint32_t *p, uint32_t v) { *p = v; }
```

The `volatile uint32_t` pointer makes the compiler emit one word-sized load or store. The tool's register class calls these for every access.

**2. C tools.** The tools that replaced the Python one (`pfdaq_cap`, `pfdaq_lat`) map registers as `volatile uint32_t *` and never use a library copy for them. The Python tool stayed as a register-level test tool.

With the helper in place the Python tool passed its 13-test suite in all five repeated runs recorded for fabric v0.3.0 (the [record](../../evidence/v0.3.0/board/suite-python-x3.txt) shows each run as "13 pass, 0 fail"). Those runs show that the tool works with the helper. They do not show the failure.

The rule went into the register contract in the architecture document: **32-bit accesses only**. Module H01 explains why a register block that is fed full words needs such a rule.

## How to recognise it

| What you see | What it tells you |
|---|---|
| A register reads back a value you did not write, but only when the write came from a scripting language | Check the width of the store before you check the logic |
| The same sequence works in simulation and from a C program | The hardware is right; the access path is not |
| A slave on a bus without byte strobes (APB3) | It can only do whole-register writes. Any partial write is a bug in the master |
| `memcpy`, slice assignment or `struct.pack_into` on mapped I/O | None of these promise a store width. Use a `volatile` word access |

Habits that followed:

- **Never reach device registers through a general-purpose copy routine.** It is allowed to use any store width.
- **State the access width in the register contract**, so that the slave's author and every tool's author read the same rule.
- **Keep a one-line reproduction.** The cheapest check for a suspect access path is to write a known value from C and from the suspect tool and read both back; nothing in the project did this at the time, and the lack of it is why the first symptom is not recorded.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [FIC3 (APB, 50 MHz)](../system/README.md#fic3) |
| **Source** | [`tests/legacy/mmio.c`](../../tests/legacy/mmio.c): `mmio_wr32`, `mmio_rd32`<br>[`hw/rtl/pfdaq_stream_src.v`](../../hw/rtl/pfdaq_stream_src.v): `apb_write`, `PWDATA` |
| **Evidence** | [Fabric-side throughput of single bursts, 931 to 947 MB/s](../../evidence/v0.3.0/board/suite-python-x3.txt) (board, fabric v0.3.0, 2026-11-15) |
| **Related** | [H01 An APB register block that software can trust](../modules/H01-apb-register-block.md)<br>I01 Reaching fabric registers from Linux *(planned)*<br>[K05 Two interrupts for every event](K05-two-interrupts-per-event.md) |
| **Status** | validated |
<!-- nav:end -->
