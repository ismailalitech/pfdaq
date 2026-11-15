# Changelog

Fabric versions follow the `VERSION` register of the stream source. Each entry lists what changed and what was validated on the board.

## v0.5.1

- Payload modes: counter, PRBS (xorshift32) and an 8-tap FIR filter on the fabric's math blocks.
- Real-time mode: fixed-rate frame slots, credits, counted drops, four rotating DMA destinations.
- Abort that completes the DMA transfer: finishes the current frame, then zero-fills to the armed length.
- Latency unit histograms moved into block RAM (26.0 % of the fabric, down from 29.2 %).
- Supervisor: task-level latency mode, shared-memory check, bare-metal build of the same source.
- Kernel driver `pfdaq_uio` for in-kernel interrupt acknowledge.
- Board: 27/27 functional tests (also from a freshly written SD card), 8/8 with the bare-metal supervisor, 30/30 reboots.
- Known issues: odd frame lengths in multi-frame bursts corrupt data; remoteproc cannot restart hart 4.

## v0.4.0

- Fabric latency unit: two channels, 20 ns resolution, minimum, maximum, sum and histogram.
- Board: latency measured for the FreeRTOS handler and Linux user space.

## v0.3.0

- Two frame words per clock, so `TVALID` is continuous inside a packet.
- Linux capture tool in C; FreeRTOS supervisor over RPMsg with deadline and sequence cross-check.
- Board: ten-minute supervised soak, 17.8 GB, no errors.

## v0.2.x

- First stream source with APB registers, sequence numbers, timestamps, CRC-32 and fault injection.
- v0.2.0 failed timing on the status readback; v0.2.1 corrupted packets above about 360 bytes on the board. Both are written up as case studies.
