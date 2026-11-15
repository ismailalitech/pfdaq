Context: u54_4_context=freertos kernel=6.18.17-linux4microchip-2026.04.1-g7fbe4f69684d-dirty uptime_s=66

| Test | Result | Detail |
|---|---|---|
| T4-counter | PASS | 32768 frames, 33.6 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (32 status, 0 misses) |
| T4-prbs | PASS | 32768 frames, 33.6 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (32 status, 0 misses) |
| T4-fir | PASS | 32768 frames, 33.6 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (32 status, 0 misses) |
| T4-fir-37x1004 | PASS | 592 frames, 2.4 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (16 status, 0 misses) |
| T5-max-burst | PASS | 2000 frames, 32.8 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (4 status, 0 misses) |
| T5-min-frame | PASS | 64 frames, 0.0 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (64 status, 0 misses) |
| T9-crc-counter | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 0/0/4, faults 4, supervisor ok (20 status, 0 misses) |
| T9-seq-counter | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 5/0/0, faults 5, supervisor ok (20 status, 0 misses) |
| T9-pay-counter | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 0/2/2, faults 2, supervisor ok (20 status, 0 misses) |
| T9-crc-prbs | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 0/0/4, faults 4, supervisor ok (20 status, 0 misses) |
| T9-seq-prbs | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 5/0/0, faults 5, supervisor ok (20 status, 0 misses) |
| T9-pay-prbs | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 0/2/2, faults 2, supervisor ok (20 status, 0 misses) |
| T9-crc-fir | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 0/0/4, faults 4, supervisor ok (20 status, 0 misses) |
| T9-seq-fir | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 5/0/0, faults 5, supervisor ok (20 status, 0 misses) |
| T9-pay-fir | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 0/2/2, faults 2, supervisor ok (20 status, 0 misses) |
| T9-deadline | PASS | 12288 frames, 12.6 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (12 status, 1 misses) |
| ABORT-2ms | PASS | DMA completed True after 8.895 ms, 119 of 500 frames real, then zero fill, aborted flag True, nothing past the burst True |
| ABORT-50us | PASS | DMA completed True after 8.899 ms, 130 of 8000 frames real, then zero fill, aborted flag True, nothing past the burst True |
| ABORT-after | PASS | 8192 frames, 8.4 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (8 status, 0 misses) |
| T6-rt-125000 | measured | slot 1000.0 us (1.0 MB/s offered), sent 4000, dropped 0 (0.0 %), verified 4000, delivered 1.0 MB/s, errors 0/0/0 |
| T6-rt-12500 | measured | slot 100.0 us (10.2 MB/s offered), sent 4000, dropped 0 (0.0 %), verified 4000, delivered 10.2 MB/s, errors 0/0/0 |
| T6-rt-6250 | measured | slot 50.0 us (20.5 MB/s offered), sent 2032, dropped 1968 (49.2 %), verified 2029, delivered 0.9 MB/s, errors 0/0/0 |
| T6-rt-2500 | measured | slot 20.0 us (51.2 MB/s offered), sent 1161, dropped 2839 (71.0 %), verified 1158, delivered 0.6 MB/s, errors 0/0/0 |
| T6-rt-1250 | measured | slot 10.0 us (102.4 MB/s offered), sent 796, dropped 3204 (80.1 %), verified 793, delivered 0.4 MB/s, errors 0/0/0 |
| T6-rt-625 | measured | slot 5.0 us (204.8 MB/s offered), sent 460, dropped 3540 (88.5 %), verified 457, delivered 0.2 MB/s, errors 0/0/0 |
| T6-rt-fir | measured | slot 50.0 us (20.5 MB/s offered), sent 1473, dropped 2527 (63.2 %), verified 1472, delivered 0.7 MB/s, errors 0/0/0 |
| T6-rt-1dest | PASS | slot 50.0 us (20.5 MB/s offered), sent 2008, dropped 1992 (49.8 %), verified 2008, delivered 10.3 MB/s, errors 0/0/0 |
| T6-rt-fault | measured | slot 100.0 us (10.2 MB/s offered), sent 1000, dropped 0 (0.0 %), verified 1000, delivered 10.2 MB/s, errors 0/1/1 |
| T7-noncached | PASS | udmabuf-ddr-nc0, 65536 words x 50, mismatches U54_4 0 / Linux 0, 31.14 ms per check (freertos) |
| T7-cached | PASS | udmabuf-ddr-c0, 65536 words x 50, mismatches U54_4 0 / Linux 0, 20.11 ms per check (freertos) |
| T8-u54_4-isr-idle | PASS | events 100000, overrun 0, min 0.52 / mean 0.52 / p99<= 0.62 / max 2.36 us |
| T8-linux-uio-idle | measured | events 100000, overrun 42, min 8.00 / mean 78.91 / p99<= 122.86 / max 4952.96 us |
| T8-linux-uio-fifo-idle | measured | events 100000, overrun 10, min 0.80 / mean 76.12 / p99<= 122.86 / max 1707.86 us |
| T8-u54_4-isr-load | PASS | events 100000, overrun 0, min 0.52 / mean 0.53 / p99<= 0.94 / max 1.96 us |
| T8-linux-uio-fifo-load | measured | events 100000, overrun 29, min 7.44 / mean 73.23 / p99<= 122.86 / max 2025.20 us |
| T8-u54_4-task-idle | PASS | events 100000, overrun 0, min 2.00 / mean 2.03 / p99<= 2.54 / max 14.42 us |
| T8-u54_4-task-load | PASS | events 100000, overrun 0, min 2.00 / mean 2.09 / p99<= 3.82 / max 9.58 us |
| T8-linux-kernel-isr-idle | measured | events 100000, overrun 0, min 1.54 / mean 4.37 / p99<= 10.22 / max 44.64 us |
| T8-linux-kernel-isr-load | measured | events 100000, overrun 10, min 1.62 / mean 3.93 / p99<= 7.66 / max 1213.50 us |
| T9-after-kill | PASS | 16384 frames, 16.8 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (16 status, 0 misses) |
| T9-remoteproc | known issue | state_before=attached, after_stop=offline, firmware=polarfire-soc-freertos-amp-demo.elf, after_start=offline, supervisor_channel=0, capture_after= |
| KNOWN-odd-frames | known issue | 148 frames, 0.6 MB, errors seq/pay/crc 3/4/4, faults 0 |

27/27 functional tests passed, 13 measurements, 2 known issues
