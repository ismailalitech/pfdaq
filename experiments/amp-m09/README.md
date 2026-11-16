# amp-m09: fencing the real-time hart with PMP

Hart 4 reads and prints the physical memory protection the boot monitor left, and, only when asked, installs four user-mode entries (never locked), runs a few instructions in user mode that try three forbidden accesses, catches the access faults in a machine-mode trap handler, restores the entries and goes on supervising. The page that explains it, with the risk list and the prediction that on this board entry 0 is locked and open to everything (in which case the test is refused and nothing is changed), is [M09](../../docs/modules/M09-fencing-the-real-time-hart-with-pmp.md).

**Status: experimental. Built and tested on the PC only; not run on the board.**

## What is in this folder

| File | What it is |
|---|---|
| `../../firmware/u54_4-supervisor/pfdaq_pmp.h` | the PMP logic and the sequence of the experiment: decoding, the access check, the plan, the refusal rules. Plain C with no hardware access, so the PC tests run this same file |
| `../../firmware/u54_4-supervisor/pfdaq_pmp_hw.h` | CSR access and the user-mode excursion (`m09_user_run`, `m09_user_entry`, `m09_trap`), RISC-V only |
| `../../firmware/u54_4-supervisor/pfdaq_supervisor.c` | blocks under `#ifdef PFDAQ_AMP_M09`; without the switch the file builds to the proven image |
| `m09_ctl.c` | the Linux tool: `ping`, `report`, `run` over RPMsg |
| `build_firmware.sh` | builds the variant ELF and `payload.bin` into `build/amp-m09/`, after proving that the production build is byte-identical to the proven ELF in its loadable content |
| `check_variant.py` | offline check that the variant ELF has the M09 code (one `mret`, the `ecall`, no `wfi`) and none of the other variants' |
| `deploy_payload.sh` | backup, install and restore of the payload partition (backup first, read-back after each write) |
| `run_m09.sh` | the board-day script: ping, report, typed confirmation, run, twenty pings, report again; exit status 3 means the test was refused |
| `fake_hart4_m09.c` | stand-in for hart 4 on the PC: the real sequence behind a socket, with a model of the PMP registers |
| `tests/` | `test_pmp.c` (logic and sequence), `sim_rv64.py` and `test_m09_asm.py` (the firmware's machine code in a small RISC-V interpreter), tests of the scripts |

## Build and test (no board)

```sh
experiments/amp-m09/build_firmware.sh        # firmware and payload, into build/amp-m09/
make -C experiments/amp-m09 board            # static RISC-V Linux binary of m09_ctl for the board
experiments/amp-m09/tests/run_tests.sh       # logic, qemu-riscv64, the assembler in the interpreter, the scripts (about 30 s)
```

`qemu-system-riscv64` is not installed on the PC, so the machine-mode and user-mode code was run in `sim_rv64.py`, a small interpreter written for this lab; it is not a model of the PolarFire SoC.

## Board day

1. `deploy_payload.sh backup`, then `install`, then reboot, and read the `[m09]` PMP report on the hart 4 console.
2. `run_m09.sh` (it copies the tool to `/root` and asks you to type `RUN-M09` before the test).
3. `deploy_payload.sh restore`, reboot.

Read the header of each script first. Nothing here sets the PMP lock bit, touches the eNVM, the boot monitor or its flash, boot-mode settings, keys or fuses; the only thing written on the board is the payload partition.
