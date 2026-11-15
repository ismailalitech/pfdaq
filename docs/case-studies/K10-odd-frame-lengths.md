---
id: K10
title: "Odd frame lengths lose 2 KB chunks"
track: case-study
level: 4
summary: "An open defect: what is known, what was ruled out, and how it is contained."
needs: [I04]
repro: doc
status: validated
hardware: [pfdaq-stream-src, dma, ddr-noncached]
software: [pfdaq-cap]
sources:
  - yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c:allow_odd
  - tests/board/pfdaq_final_suite.sh
evidence: [suite-freertos, suite-freertos-raw, sim-stream, timing, timing-without-cdc]
related: [I04, K03, K09]
---

# Odd frame lengths lose 2 KB chunks

This defect is open. A burst of several frames whose length is an odd number of 32-bit words arrives in memory with pieces missing or misplaced, although the DMA reports a normal completion. The stream is correct in simulation and the design meets timing. The cause has not been found. This page records what is known, what has been ruled out, and what keeps the defect from touching the rest of the project.

## Symptom

The capture tool sends one burst of 37 frames of 1,001 payload words. A frame is the payload plus four words (header, sequence number, timestamp, CRC), so each frame is 1,005 words, an odd number. The burst is 148,740 bytes. The board suite runs it four times as its last test, `KNOWN-odd-frames`:

| Result | Value |
|---|---|
| Frames received | 148 (4 × 37) |
| Bytes | 594,960 |
| Errors seq / payload / CRC | 3 / 4 / 4 |
| Frames whose header was not found where expected | 121 |
| DMA failures reported | 0 |

The figures are in the board suite record (`suite-freertos`) and, with the extra counters, in the raw record (`suite-freertos-raw`). The test is marked as a known issue, not as a pass.

The damage has a recognisable shape. In dumps of the buffer after a failed counter burst, the payload ran normally and then jumped: in two of three repeated runs the counter jumped 512 words ahead (at word 92 and at word 111 of a frame), which is exactly 2 KB of data missing. In the third, earlier data appeared again where new data was due (at word 604). Everything after the jump was shifted, so later frames started at the wrong address and their headers did not match. The position of the first jump differed from run to run (frame 5 in two runs, frame 6 in another).

These dumps were taken during the investigation. The raw output is not published; the figures above are recorded in the project's notes.

## Isolation

One variable was changed at a time, on the board, with fabric v0.5.1 and the capture tool in its serial mode. Pass or fail is by the tool's per-frame check, which recomputes every word.

| Burst | Result |
|---|---|
| 37 frames, 1,001 payload words (odd frame length), counter payload | fails |
| same, PRBS payload | fails |
| same, FIR payload, two different filter steps | fails |
| 37 frames, 1,000 payload words (even frame length), FIR | passes |
| 1 frame, 1,001 payload words (odd), FIR | passes |
| 38 frames, 1,001 payload words (odd), FIR | fails |
| 37 frames of 999, 1,003, 251 and 253 payload words (all odd lengths) | all fail |

So the failure depends on the frame length being odd and on there being more than one frame in the transfer. It does not depend on the payload type, which also rules out the generator's payload logic. The suite's even-length tests pass in every mode: 27 of 27 functional tests, including four transfers of 8,192,000 bytes and 64 single-frame bursts with a one-word payload.

A reboot was needed between some of these experiments. In one series, after several failing odd bursts, an even-length capture failed too (thousands of payload errors). After a reboot the even-length captures passed again; a single odd burst then failed, and an even-length run directly after it passed. What exactly makes one failure disturb later transfers and another not was not worked out.

## What is known and what is not

**Established by measurement**

- The failure needs an odd number of words per frame and several frames in one DMA transfer.
- Damage comes in pieces of 2 KB, at 2 KB or 4 KB boundaries of the destination buffer. The tool does not say which 2 KB boundary each time.
- The DMA reports completion. The descriptor's ready bit clears and the interrupt arrives as it does for a good transfer.

**Ruled out, with the evidence**

- *The stream itself.* The simulation, judged by an independent model, passes all 20 scenarios (`sim-stream`), among them scenario 8 (7 frames of 13 words) and scenario 20 (40 frames of 37 words with an abort), both odd. A further case of 6 frames of 1,005 words passed in a scratch testbench during the investigation; it was not kept.
- *The final 4-byte beat.* An odd total number of words ends the packet with a half-filled beat. But 38 frames of 1,005 words is an even total, and it fails as well. This is an inference from the recorded runs, not a separate test.
- *Timing.* The build meets timing in all corners (`timing`). Re-running the analysis without the clock-crossing constraints (`timing-without-cdc`) lists violating paths, and the project's notes record that they are exactly the intended crossings. The list itself is not part of the published extract.
- *Payload type, and the capture tool's checker.* Counter, PRBS and FIR all fail alike, and a separate script reading the buffer directly sees the same jumps.

**Hypotheses, not tested**

- The DMA core or the AXI interconnect splits write bursts at 2 KB or 4 KB boundaries and mishandles a burst that starts at a four-byte offset inside an eight-byte beat. That fits the 2 KB pieces but does not explain why the length of a *frame* matters, since the stream is one continuous packet.
- The pattern of valid and ready cycles differs between odd and even lengths in a way simulation, with its random ready pattern, does not reproduce. Not investigated.

**Not known**

- Whether any other length or alignment fails, such as lengths that are odd multiples of some other number. Only the cases in the table were tried.
- Whether the defect is in the vendor DMA core, in the interconnect, or in how this project drives them.
- Whether it appears on a second board or core version.

Settling it needs either the vendor DMA core in a simulator, fed the same stream, or an on-chip logic analyser on the DMA's AXI write channel. Neither has been done.

## Containment

The capture tool refuses the case unless asked. This is the check in `pfdaq_cap.c`:

```c
if (((payload + 4) & 1) && frames > 1 && !rt_rate && !allow_odd) {
	fprintf(stderr, "odd frame length (%u words) in a multi-frame burst is a known data-corruption case; "
		"use an even payload length, or -O to run it anyway\n", payload + 4);
	return 2;
}
```

Real-time mode is exempt because each frame there is its own packet and its own transfer, the same shape as the single-frame case that passes. Everything else that uses multi-frame bursts uses even lengths.

The board suite runs the odd case last, with `-O`, because a failure can disturb later transfers until a reboot. It is reported as a known issue and does not count towards the 27 functional tests.

The project decided not to chase the root cause for now: even lengths are clean in every test, and the investigation needs material that is not at hand.

## How to recognise it

| What you see | What it tells you |
|---|---|
| Frame length (payload plus 4 words) is odd and the burst has more than one frame | You are in the known case; use an even payload length |
| The check reports bad headers for most frames after one point | A piece of 512 words (2 KB) went missing or moved; later frames are shifted |
| Counter payload jumps by exactly 512 | The same defect, seen in the simplest payload |
| The DMA reports success and the descriptor's ready bit is clear | Completion alone proves nothing; the data check found the damage |
| Even-length captures start failing after an odd one | Reboot before trusting any further result |

The lesson that carries over is the one from case study K03: with a vendor core, a clean simulation and met timing do not prove the whole path, and every capture needs a check that reads the data, not only the completion flag.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [I04 DMA stream descriptors: the rules this core really follows](../modules/I04-dma-stream-descriptors.md) |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [Stream source](../system/README.md#pfdaq-stream-src) · [CoreAXI4DMAController](../system/README.md#dma) · [Non-cached window](../system/README.md#ddr-noncached) · [pfdaq_cap](../system/README.md#pfdaq-cap) |
| **Source** | [`yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c`](../../yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c): `allow_odd`<br>[`tests/board/pfdaq_final_suite.sh`](../../tests/board/pfdaq_final_suite.sh) |
| **Evidence** | [Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Raw output of the FreeRTOS suite, one JSON object per test](../../evidence/v0.5.1/board/suite-freertos.jsonl) (board, fabric v0.5.1, 2026-11-15)<br>[Stream source simulation, 20 scenarios checked by the Python model](../../evidence/v0.5.1/sim/stream.txt) (sim, fabric v0.5.1, 2026-11-15)<br>[Place and route result, timing met in all corners, 64 of 64 pins placed, 26 % of the fabric](../../evidence/v0.5.1/build/timing-and-utilisation.txt) (build, fabric v0.5.1, 2026-11-15)<br>[Timing analysis without the clock-crossing constraints (the tool's console log; the 40 worst violating paths it lists are the intended crossings)](../../evidence/v0.5.1/build/timing-without-cdc-constraints.txt) (build, fabric v0.5.1, 2026-11-15) |
| **Related** | [I04 DMA stream descriptors: the rules this core really follows](../modules/I04-dma-stream-descriptors.md)<br>[K03 A DMA core that needs TVALID without gaps](K03-tvalid-gaps.md)<br>[K09 An early TLAST does not end a transfer](K09-early-tlast.md) |
| **Status** | validated |
<!-- nav:end -->
