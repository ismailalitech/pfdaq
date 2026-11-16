# amp-m08: three ways to signal another hart

A Linux program measures the round-trip time to hart 4 four ways: the production RPMsg ping (`rpmsg-query`), an RPMsg ping that is answered at once (`rpmsg-ping`), a word in the non-cached DDR window that both sides poll (`flag`), and an interrupt raised by one register write, latency channel 1 `ONESHOT` raising F2M 5 (`irq`). The page that explains it, including why an SBI inter-processor interrupt cannot reach hart 4 from Linux, is [M08](../../docs/modules/M08-three-ways-to-signal.md).

**Status: experimental. Built and tested on the PC only; not run on the board.**

## What is in this folder

| File | What it is |
|---|---|
| `m08_rtt.c` | the Linux program: pinned to a cpu, `CLOCK_MONOTONIC`, N trials per method, min / median / p95 / max, late and lost counts; `--sim` runs it against stand-ins in the same process |
| `../../firmware/u54_4-supervisor/pfdaq_m08.h` | the shared block and the two answering routines of hart 4 (polling loop, interrupt hook). Plain C with no hardware access, so the PC tests run this same file |
| `../../firmware/u54_4-supervisor/pfdaq_supervisor.c` | three small blocks and one hook under `#ifdef PFDAQ_AMP_M08`; without the switch the file builds to the proven image |
| `build_firmware.sh` | builds the variant ELF and `payload.bin` into `build/amp-m08/`, after proving that the production build is byte-identical to the proven ELF in its loadable content |
| `check_variant.py` | offline check that the variant ELF has the M08 code and none of the other variants' |
| `deploy_payload.sh` | backup, install and restore of the payload partition (backup first, read-back after each write); the only thing it writes on the board is `/dev/mmcblk0p2` |
| `run_m08.sh` | the board-day script: preflight, probe, idle and loaded passes, twenty pings afterwards, records under `logs/m5/m08/` |
| `tests/` | unit tests, tests of the program against the simulated hart 4, tests of the scripts |

## Build and test (no board)

```sh
experiments/amp-m08/build_firmware.sh        # firmware and payload, into build/amp-m08/
make -C experiments/amp-m08 board            # static RISC-V Linux binary for the board
experiments/amp-m08/tests/run_tests.sh       # unit tests, simulated hart 4, qemu-riscv64 (about 40 s)
experiments/amp-m08/tests/test_run_m08.sh    # the board-day script against stand-ins
experiments/amp-m08/tests/test_deploy.sh     # deploy_payload.sh against a plain file
```

## Board day

1. `deploy_payload.sh backup`, then `install`, then reboot the board and check the hart 4 console for the `[m08]` banner.
2. `run_m08.sh` (it copies the binary to `/root` itself).
3. `deploy_payload.sh restore`, reboot.

Read the header of each script first. Nothing here sets a lock, touches the eNVM or the boot monitor, or writes anything on the board but the payload partition (and files in `/root` and `/dev/shm`). The program writes one page of the non-cached window (`0xC9E0_0000`) and the `CTRL` register of latency channel 1, and refuses to touch the channel if it is enabled or has an interrupt pending.
