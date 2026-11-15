Context: u54_4_context=freertos kernel=6.18.17-linux4microchip-2026.04.1-g7fbe4f69684d-dirty uptime_s=34

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
| ABORT-2ms | PASS | DMA completed True after 8.899 ms, 119 of 500 frames real, then zero fill, aborted flag True, nothing past the burst True |
| ABORT-50us | PASS | DMA completed True after 8.904 ms, 128 of 8000 frames real, then zero fill, aborted flag True, nothing past the burst True |
| ABORT-after | PASS | 8192 frames, 8.4 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (8 status, 0 misses) |
| T6-rt-125000 | measured | slot 1000.0 us (1.0 MB/s offered), sent 4000, dropped 0 (0.0 %), verified 4000, delivered 1.0 MB/s, errors 0/0/0 |
| T6-rt-12500 | measured | slot 100.0 us (10.2 MB/s offered), sent 3999, dropped 1 (0.0 %), verified 3998, delivered 1.7 MB/s, errors 0/0/0 |
| T6-rt-6250 | measured | slot 50.0 us (20.5 MB/s offered), sent 3208, dropped 792 (19.8 %), verified 3205, delivered 1.5 MB/s, errors 0/0/0 |
| T6-rt-2500 | measured | slot 20.0 us (51.2 MB/s offered), sent 1318, dropped 2682 (67.0 %), verified 1316, delivered 0.7 MB/s, errors 0/0/0 |
| T6-rt-1250 | measured | slot 10.0 us (102.4 MB/s offered), sent 586, dropped 3414 (85.3 %), verified 583, delivered 0.3 MB/s, errors 0/0/0 |
| T6-rt-625 | measured | slot 5.0 us (204.8 MB/s offered), sent 463, dropped 3537 (88.4 %), verified 460, delivered 0.2 MB/s, errors 0/0/0 |
| T6-rt-fir | measured | slot 50.0 us (20.5 MB/s offered), sent 1745, dropped 2255 (56.4 %), verified 1744, delivered 0.8 MB/s, errors 0/0/0 |
| T6-rt-1dest | PASS | slot 50.0 us (20.5 MB/s offered), sent 1993, dropped 2007 (50.2 %), verified 1993, delivered 10.2 MB/s, errors 0/0/0 |
| T6-rt-fault | measured | slot 100.0 us (10.2 MB/s offered), sent 1000, dropped 0 (0.0 %), verified 1000, delivered 10.2 MB/s, errors 0/1/1 |
| T7-noncached | PASS | udmabuf-ddr-nc0, 65536 words x 50, mismatches U54_4 0 / Linux 0, 31.15 ms per check (freertos) |
| T7-cached | PASS | udmabuf-ddr-c0, 65536 words x 50, mismatches U54_4 0 / Linux 0, 20.10 ms per check (freertos) |
| T8-u54_4-isr-idle | PASS | events 100000, overrun 0, min 0.52 / mean 0.52 / p99<= 0.62 / max 2.10 us |
| T8-linux-uio-idle | measured | events 100000, overrun 89, min 4.18 / mean 78.31 / p99<= 122.86 / max 5130.60 us |
| T8-linux-uio-fifo-idle | measured | events 100000, overrun 7, min 2.58 / mean 76.10 / p99<= 122.86 / max 1673.76 us |
| T8-u54_4-isr-load | PASS | events 100000, overrun 0, min 0.52 / mean 0.53 / p99<= 0.94 / max 1.94 us |
| T8-linux-uio-fifo-load | measured | events 100000, overrun 18, min 2.24 / mean 73.58 / p99<= 122.86 / max 1815.34 us |
| T8-u54_4-task-idle | PASS | events 100000, overrun 0, min 2.00 / mean 2.04 / p99<= 2.54 / max 11.66 us |
| T8-u54_4-task-load | PASS | events 100000, overrun 0, min 2.00 / mean 2.09 / p99<= 3.82 / max 9.36 us |
| T8-linux-kernel-isr-idle | measured | events 100000, overrun 6, min 1.36 / mean 4.41 / p99<= 10.22 / max 1298.18 us |
| T8-linux-kernel-isr-load | measured | events 100000, overrun 11, min 1.50 / mean 3.95 / p99<= 10.22 / max 1279.66 us |
| T9-after-kill | PASS | 16384 frames, 16.8 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (16 status, 0 misses) |
| T9-remoteproc | known issue | state_before=attached, after_stop=offline, firmware=polarfire-soc-freertos-amp-demo.elf, after_start=offline, supervisor_channel=0, capture_after= |
| KNOWN-odd-frames | known issue | 148 frames, 0.6 MB, errors seq/pay/crc 3/4/4, faults 0 |

27/27 functional tests passed, 13 measurements, 2 known issues
