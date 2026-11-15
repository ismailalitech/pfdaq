Context: u54_4_context=bare-metal remoteproc=attached

| Test | Result | Detail |
|---|---|---|
| T4-counter | PASS | 32768 frames, 33.6 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (32 status, 0 misses) |
| T4-fir | PASS | 32768 frames, 33.6 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (32 status, 0 misses) |
| T9-crc-prbs | PASS | 20480 frames, 21.0 MB, errors seq/pay/crc 0/0/4, faults 4, supervisor ok (20 status, 0 misses) |
| T9-deadline | PASS | 12288 frames, 12.6 MB, errors seq/pay/crc 0/0/0, faults 0, supervisor ok (12 status, 1 misses) |
| T7-noncached | PASS | udmabuf-ddr-nc0, 65536 words x 50, mismatches U54_4 0 / Linux 0, 29.88 ms per check (bare-metal) |
| T7-cached | PASS | udmabuf-ddr-c0, 65536 words x 50, mismatches U54_4 0 / Linux 0, 18.77 ms per check (bare-metal) |
| T8-u54_4-isr-idle | PASS | events 100000, overrun 0, min 0.50 / mean 0.50 / p99<= 0.62 / max 3.10 us |
| T8-u54_4-isr-load | PASS | events 100000, overrun 0, min 0.50 / mean 0.51 / p99<= 0.94 / max 2.44 us |

8/8 functional tests passed, 0 measurements, 0 known issues
