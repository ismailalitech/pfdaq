---
id: M09
title: "Fencing the real-time hart with physical memory protection"
track: multicore
level: 5
summary: Reading what the boot monitor left in the real-time hart's PMP, and a deliberate violation from user mode that proves whether a fence holds.
needs: [M01]
repro: full-system
status: validated
evidence: [m09-report, m09-run, m09-console, m09-ping-after]
hardware: [u54-4, u54-linux, e51, ddr-u54-4-image, ddr-rpmsg]
software: [hss, supervisor, remoteproc-rpmsg]
sources:
  - firmware/u54_4-supervisor/pfdaq_pmp.h:pmp_check,pmp_plan_build,pmp_can_install,pmp_first_locked,m09_run,m09_judge
  - firmware/u54_4-supervisor/pfdaq_pmp_hw.h:m09_user_run,m09_trap,m09_hw_write_state
  - firmware/u54_4-supervisor/pfdaq_supervisor.c:PFDAQ_AMP_M09,m09_handle
  - firmware/u54_4-supervisor/disco-kit/MPFS_DISCOVERY_KIT_MSS_mss_cfg.xml:PMP0CFG,CSR_PMPADDR0,MEM_CONFIGS_ENABLED
  - experiments/amp-m09/m09_ctl.c
  - experiments/amp-m09/tests/sim_rv64.py
  - experiments/amp-m09/build_firmware.sh
  - experiments/amp-m09/deploy_payload.sh
  - experiments/amp-m09/run_m09.sh
related: [M01, M02, M03, M04, M05, M10, B02]
---

# Fencing the real-time hart with physical memory protection

After this module you can read the physical memory protection (PMP) of the real-time hart, say what a PMP entry does and does not constrain in machine mode and in user mode, and follow a test that tries three forbidden accesses from user mode and catches each one as an access fault. You will also know the one result that decides whether the test can run at all on this board: whether the entries the boot monitor leaves are locked. The board answered it: they are locked, and open to everything the register can express, so on this board the test is refused and the PMP is not a fence.

**Status: validated; the result is a finding, not the test the lab was built for.** On the board, hart 4's PMP holds one locked entry (`pmp0`, NAPOT, read, write and execute, covering `0x0` to `0x7fffffffff`). The deliberate-violation test therefore refused to run, installed nothing and changed nothing, and hart 4 kept answering. The test itself (three forbidden accesses from user mode, caught as access faults) was **not** run on the board: it ran only on the PC, in a model of the registers and in a small RISC-V interpreter written for this lab. What the page claims about the board is limited to the register read and the refusal.

## Before you start

- **Level 5.** You should have read module M01 (which hart runs what, in which privilege mode, and that the boot monitor sets up PMP before it starts each hart) and know what a trap and `mret` are. M10 shows the same preparation for a different experiment (a payload variant, offline checks, backup and restore).
- **Board:** the shared baseline with one reversible change: a payload with a variant of the hart 4 firmware. The only thing written on the board is the raw payload partition of the SD card, with a backup first and a restore afterwards. **Never anything else:** no eNVM, boot-mode settings, keys, fuses or locks; no change to the boot monitor or its flash; and the demo never sets the PMP lock bit on any entry.
- **Tools:** the PC toolchain of M10, the SmartHLS RISC-V compiler for the Linux tool, and `qemu-riscv64` for the logic tests. A system-mode RISC-V emulator (`qemu-system-riscv64`) is not installed on the PC, which is why the trap code was tested in the small interpreter described below.

## The idea

PMP is a table in each hart of up to 16 entries, each a memory range with read, write and execute permissions. It is **per hart**: the E51 and each U54 have their own, and one hart's table says nothing about another's. For an access the hardware looks for the lowest-numbered entry whose range contains the access and applies that entry's permissions. Three rules matter here:

1. **Machine mode ignores an entry unless it is locked.** The lock bit (L, bit 7 of the entry's configuration byte) makes an entry apply to machine mode too, and freezes it: the entry cannot be changed again until the hart is reset. Without L, an entry constrains only user and supervisor mode.
2. **If no entry matches, machine mode succeeds and user mode fails.**
3. **The lowest-numbered matching entry decides.** A permissive entry at a low number makes every later, stricter entry irrelevant.

The first rule is why a fence test on a machine-mode hart needs a trick: hart 4 runs in machine mode, so an unlocked entry never stops it, and a deliberate violation proves nothing from there. The trick used here is to run a few instructions in user mode, where the entries do apply, and to catch the access fault in a machine-mode trap handler. The fence is real for user-mode code, and it is the demo's own code that plays the part of untrusted software.

The test would be a fence for a real supervisor only if the supervisor's untrusted parts ran in user mode all the time. This lab does not go that far. It shows the mechanism, and it shows what the boot monitor already did.

## On this board

### What the boot monitor and the firmware leave

The boot record shows a `SetupPMP` step for each of the four application harts (module M01). Reading the vendor sources and the project's configuration gave a specific prediction, which the board then confirmed (next section). The list below is read from source files, not measured:

- The boot monitor asks each hart to program its own PMP (`HSS_Boot_PMPSetupHandler` calls `init_pmp` on the target hart, in machine mode), and its comment says the entries are locked so that they cannot be changed without a reboot.
- `pmp_configure`, the vendor function behind it, writes `pmpaddr0` to `pmpaddr15` from the values of the MSS configuration, and then writes `pmpcfg0` from the same table, unless the configurator's PMP switch is on.
- This board's MSS configuration has that switch off and, for hart 4, `PMP0CFG` = `0x9F` and `CSR_PMPADDR0` = all ones.
- The hart 4 firmware's own start-up code calls the same `init_pmp(4)` again before `main`. If entry 0 is already locked, that second write is ignored.

![Three lines of the project's MSS configuration for hart 4: PMP0CFG is 0x9F, binary 1001 1111, which is lock, NAPOT, execute, write, read; CSR_PMPADDR0 is all ones, a NAPOT range that covers the whole address space; and the configurator's own PMP switch, MEM_CONFIGS_ENABLED, is 0. Together they predict a locked entry 0 that is open to everything.](../diagrams/lab-m09-predicted-state.svg)

1. `0x9F` is `1001 1111`: L, address mode NAPOT, and X, W, R. A locked entry.
2. A NAPOT range whose address is all ones is the whole address space. Entry 0 is the lowest number, so it decides every access.
3. With the configurator's PMP off, the vendor code writes the table's `pmpcfg0` as it stands, which is the first line.

That is what the hart has. Three consequences follow; the first two come from the PMP rules and the register value, and only the locked entry itself was read from the board:

- **Every access of every mode is allowed.** Entry 0 matches everything and allows read, write and execute. User mode is as open as machine mode.
- **Entries 1 to 15 can never matter.** Whatever the demo writes there is behind entry 0.
- **Entry 0 cannot be changed.** It is locked, so the demo cannot tighten the PMP without a reset, and a reset with a different PMP is the boot monitor's job, not firmware's. Making the PMP real on this board would mean enabling PMP in the MSS configuration and rebuilding the boot monitor, which is a change of the boot monitor and its flash. This project does not do that.

So the experiment is built to start by looking. Step (a), reading and printing the registers, is read-only and runs at every boot of the variant. Step (b), installing entries, runs only when asked, and only if no entry that it would write is locked.

### What the variant does

`pfdaq_supervisor.c` gets three blocks under `#ifdef PFDAQ_AMP_M09`; the logic is in `pfdaq_pmp.h` (no hardware access, so the PC tests run the same file) and the hardware half in `pfdaq_pmp_hw.h`.

1. **At start (read only):** print `pmpcfg0`, `pmpcfg2` and each entry that is not empty, decoded (mode, R, W, X, L, the range it covers), then one line saying how many are active and locked, and whether the first locked entry is open to everything.
2. **On a `REPORT` message:** the same again, and an answer with the counts.
3. **On a `RUN` message from Linux, never at boot:**
   1. Build the plan (below). If a locked entry among the first four, or a locked top-of-range entry above them, would ignore the write, print `REFUSED`, change nothing, run nothing.
   2. Print what the model of the PMP predicts for each forbidden access.
   3. Write the four entries (switch them off, write the addresses, then the configuration), read them back, and compare. If they differ, put everything back and stop.
   4. Run the user-mode function once (next section).
   5. Put the registers back as the boot monitor left them and read them back to confirm.
   6. Print the three faults, judge the run, send the result to Linux. The supervisor loop goes on.

### The plan: four entries, no lock

![The four PMP entries the demo installs for user mode and what each access of the test meets: pmp0 a 4 KiB canary page, read only; pmp1 the 1 MiB image at 0x91C00000, read, write and execute; pmp2 off, holding the bottom address of pmp3, the rings from 0x91D00000 to 0x91D51000, read and write. A load from Linux DDR or from the boot monitor's L2 scratchpad matches no entry and is denied; a store to the canary page is denied by pmp0, which comes before pmp1.](../diagrams/lab-m09-plan.svg)

1. User mode starts at the image's own code and meets the entries in order.
2. `pmp0` makes one 4 KiB page of the firmware's own data read only. Because it is a lower number than `pmp1`, it wins inside its page.
3. `pmp1` lets user mode run and use the firmware image: code, data and stack.
4. `pmp2` is off; its address is only the bottom of `pmp3`, which covers the ring region (rings, buffer pool, resource table, `0x91D0_0000` to `0x91D5_1000`) for read and write.
5. Everything else has no entry, so user mode is denied. Machine mode is never limited, because no entry has L.

The three forbidden accesses and why each is harmless even if the fence does not hold:

| Probe | Address | Kind | Expected | Why it cannot hurt |
|---|---|---|---|---|
| Linux DDR | `0x8020_0000`, where U-Boot and the kernel live | load | `mcause` 5, `mtval` = that address | a load: nothing in memory changes |
| boot monitor | `0x0A00_0000`, the L2 scratchpad where the boot monitor runs (its linker script) | load | `mcause` 5 | a load: nothing changes |
| canary | a 4 KiB page of the firmware's own data | **store** | `mcause` 7 | the store goes to hart 4's own variable; if the fence is missing it simply succeeds there |

A store would be the natural probe for the first two, and the demo does not use one: a store that was meant to be caught, and was not, would corrupt Linux or the boot monitor. A load proves the same thing about the fence, and the store test aims only at memory the demo owns. The allowed accesses are a load and a store of one word of its own data, a load of the first word of the ring region (never a store there), and a load of the canary page.

### The excursion, and why FreeRTOS is not disturbed

The vendor start-up code owns the trap vector. For any exception the vendor trap handler (`trap_from_machine_mode` in the HAL) ends in a `while` loop that never returns ("wait for watchdog"). **A fault in user mode with the vendor vector in place would therefore freeze hart 4 for good.** The demo must own the vector while it is in user mode, and it does.

![One trip into user mode in the order it happens: m09_user_run saves the registers and the four CSRs it changes, clears mie, points mtvec at m09_trap, keeps the stack pointer in mscratch, sets mepc, clears MPP and MPRV and executes mret; the user code does three allowed accesses and then three violations, each violation traps to m09_trap, which logs, skips the instruction and returns by mret; the ecall that ends the code makes m09_trap restore everything and return to the caller as an ordinary function return.](../diagrams/lab-m09-excursion.svg)

1. `m09_user_run` pushes `ra`, `s0` to `s11` and the four CSRs it will change (`mtvec`, `mscratch`, `mstatus`, `mie`) on the supervisor task's own stack.
2. It clears `mie` (no interrupt can arrive in user mode: an interrupt for machine mode is always taken while the hart is in a lower mode, whatever `mstatus.MIE` says), points `mtvec` at `m09_trap`, keeps the stack pointer in `mscratch`, sets `mepc`, clears `mstatus.MPP` and `MPRV`, and executes `mret`.
3. The user code in `m09_user_entry` is six accesses and an `ecall`, written in assembler without compressed instructions, on a stack of its own.
4. Each access fault enters `m09_trap`: it swaps `sp` with `mscratch` to get a machine-mode stack, logs `mcause`, `mtval` and `mepc` in a table of eight, steps over the instruction, swaps back and returns by `mret` to user mode.
5. A load or store access fault is the only thing it skips. The `ecall`, a full log, or any other trap (an instruction fault if user mode may not even fetch, an illegal instruction) ends the excursion at once, so a fault that repeats cannot loop.
6. On the way out it restores `mtvec`, `mscratch`, `mstatus`, the registers and finally `mie`, and returns. To the caller it was an ordinary function.

Why this does not break the FreeRTOS port, from reading its sources:

- **The scheduler cannot run in between.** The tick and every other interrupt are masked for the few microseconds of the excursion, so nothing saves a context on the user stack or switches task. The excursion runs inside the supervisor task, which is a task of the running scheduler, as `st_print` in M10 does; running it before the scheduler starts would need an edit to the vendor `demo_main.c`, which the variant avoids.
- **The port's `mret` is untouched.** It restores `mstatus` from the task's saved context and sets `MPP` to machine mode itself each time (`MSTATUS_PRV1` in the port assembler), and it never uses `ecall`. Our `mret` and `ecall` are outside it.
- **`mscratch` is the port's per-task slot** (it saves and restores it with the context); the demo saves and restores it too.
- **A timer interrupt that became pending meanwhile** is taken the moment `mie` is restored, in the restored context, by the port's own vector, as if it had arrived a few microseconds later.

### What the PC tests cover

- `test_pmp` (about 9,000 checks, also under `qemu-riscv64` and with the address and undefined-behaviour sanitizers): the encoding of NAPOT, TOR and NA4 ranges; the access check against the specification's rules (lowest match, partial match fails, machine mode and the lock bit, no match); the plan (its register values, no lock bit in any entry); the refusal rules; the whole sequence against a model of the CSRs that ignores writes to locked entries: a clean PMP gives result `OK` and restores the registers exactly; the state predicted for this board gives `REFUSED` with nothing written and nothing run; a locked allow-all entry at a higher number lets the plan be installed and gives `NOT_EFFECTIVE`; a write that does not read back, and an aborted user run, give `INSTALL_FAILED` and `ABORTED`; 500 random starting states show that no lock bit is ever written and the registers always come back.
- `test_m09_asm.py`: the real machine code of `m09_user_run`, `m09_user_entry` and `m09_trap`, taken from the firmware ELF, runs in `sim_rv64.py`, a small RISC-V interpreter with machine and user mode, traps, `mret` and a PMP written from the specification separately from the C model. With the plan installed: exactly three faults with `mcause` 5, 5, 7, the three `mtval` addresses and the three `mepc` values, the allowed store happened and the denied one did not, no store went to Linux, boot monitor or ring memory, and `mtvec`, `mscratch`, `mstatus`, `mie`, `sp`, `s0` to `s11` and the privilege mode are as before. A timer interrupt that becomes pending during the excursion is taken once, after it, by the saved vector. With no PMP entry at all the user code cannot be fetched and the excursion ends at once and restores everything. With the predicted board state nothing is blocked. If every data access faults, the log stops at eight and the function still returns.
- The interpreter is **not a model of the PolarFire SoC**: no caches, devices or timing, and its PMP and the C model's were written by the same author. It tests the control flow and the register discipline of the assembler, which is where a hang would come from. It does not test the hardware's PMP.
- `run_m09.sh` and `m09_ctl` against a stand-in that runs the real sequence behind a socket: the clean state ends with result `OK`, the predicted state with `REFUSED` and exit status 3, a silent hart 4 stops the script before the test, a hart 4 that never answers `RUN` is reported with the way back.

## Run it

Steps 1 to 3 ran on the PC. Steps 4 to 8 are the board-day sequence; they have **not been run**.

1. Build the variant, its payload and the offline checks:

   ```sh
   experiments/amp-m09/build_firmware.sh
   ```

   It prints that the production build equals the proven ELF, builds `build/amp-m09/payload.bin`, runs `verify_payload.py` of M10 on it, and `check_variant.py` (the M09 strings are in the variant and in no other ELF; the machine code has the one `mret` and the `ecall` and no `wfi`).

2. Run the host tests:

   ```sh
   experiments/amp-m09/tests/run_tests.sh
   ```

3. Build the board tool:

   ```sh
   make -C experiments/amp-m09 board
   ```

4. On the board. Backup first, then install, then reboot:

   ```sh
   experiments/amp-m09/deploy_payload.sh backup
   experiments/amp-m09/deploy_payload.sh install
   ssh root@pfdaq-board reboot
   ```

5. Read the console of hart 4 (interface 3 of the FlashPro5 port). It must show the `[m09]` banner and the PMP report. **This is the answer to the first question** and needs nothing else.
6. Run the sequence:

   ```sh
   experiments/amp-m09/run_m09.sh
   ```

   It pings hart 4, asks for the report, waits for you to type `RUN-M09`, asks hart 4 to run the test, then pings it 20 times and asks for the report again. Exit status 0 means the test ran and caught all three violations; 3 means hart 4 refused because a locked entry blocks the plan (the predicted outcome; not an error); 1 is anything else.
7. Keep the record in `logs/m5/m09/<time>/`: `run.txt`, `report.txt`, the pings, and the console lines.
8. Go back to the proven payload and reboot:

   ```sh
   experiments/amp-m09/deploy_payload.sh restore
   ssh root@pfdaq-board reboot
   ```

## What you should see

The recorded session (`m09-console`, `m09-run`, `m09-report`, `m09-ping-after`):

```text
[m09] pmpcfg0 0x000000000000009f pmpcfg2 0x0000000000000000
[m09] pmp0  cfg 0x9f NAPOT RWX L pmpaddr 0x00000fffffffff covers 0x0..0x7fffffffff
[m09] 1 entries active, 1 locked
[m09] entry 0 is locked: a locked entry cannot be changed before the hart is reset
[m09] RUN requested by Linux
[m09] REFUSED: pmp0 is locked, so writes to it are ignored until the hart is reset. Nothing installed, no user-mode test run, nothing changed
[m09] pmpcfg0 0x000000000000009f pmpcfg2 0x0000000000000000
...
```

1. **The report is printed at every boot of the variant, read-only.** Entry 0 is `0x9f`: locked, NAPOT, read, write, execute, with an address that covers `0x0` to `0x7fffffffff`. No other entry is active.
2. **`RUN` is refused.** The tool prints `result REFUSED (1), 0 access faults caught`, and hart 4 wrote nothing: the register report after the refusal equals the one before.
3. **Hart 4 kept working.** 3 of 3 pings before and 20 of 20 pings after were answered (the first answers took 1.8 to 2.2 ms, as in module M02).
4. **The boot monitor set this entry, not the lab.** The boot record shows `SetupPMP` for hart 4 before the firmware runs (module M01); the firmware's own start-up code writes the same value again, which a locked entry ignores. The lab changed nothing: it read.
5. **What it means.** As configured, the PMP of the real-time hart allows every access of every mode that the entry can describe, and entry 0, the lowest, decides first, so entries 1 to 15 could never matter even if they could be written. Making the PMP a real fence on this board means turning the PMP on in the MSS configuration and rebuilding the boot monitor, which changes its flash. This project does not do that, so the answer to the lab's title question is: not here, and here is the register that says so.

What was not shown: the user-mode violations (the three faults, `mcause` 5, 5 and 7), because the test cannot run with a locked entry 0. Their code ran only in the PC interpreter. One report line, `locked_open_to_all` in the JSON of the tool, printed `false`. That flag is true only for a range that reaches `0x00ffffffffffffff` or higher, and this core's `pmpaddr` register holds fewer bits: the value read back, `0xfffffffff`, has every implemented bit set (the project's configuration writes all ones), and it describes the range `0x0` to `0x7fffffffff`. So the flag's test is too strict for this core; the register is full, and the entry reads read, write and execute. That reading is an inference from the value, not a separate measurement.

## Evidence

- `m09-console`: the hart 4 console lines of the session. `m09-report`, `m09-run`, `m09-ping-after`: the output of the tool for the report, the refused run and the 20 pings afterwards.
- The proven payload was written back after the session, read back and compared by SHA-256, and the board suite passed 27 of 27 (`m0809-suite-after`).
- Offline only, as described in "What the PC tests cover": the logic of the PMP and the whole sequence in a model, the machine code in an interpreter. None of it is a result for the board.

## Risk list

What is changed on the board, and what can go wrong. The first four are the same as for any payload variant in this project.

| Risk | What happens and what protects against it |
|---|---|
| **The payload partition is written.** | Only `/dev/mmcblk0p2`, by `deploy_payload.sh`: the payload on the board is read first and kept on the PC with a checksum computed on both sides; `install` refuses without a verified backup, refuses a partition that does not start with the boot monitor's magic, asks for a typed confirmation, and reads back after the write. The variant payload is checked offline against the proven one before it is written. |
| **The variant does not come up** (no `[m09]` banner, no channel). | Linux boots without hart 4 (the project has seen a FreeRTOS no-start once in about sixty boots, module M01), so `deploy_payload.sh restore` still works over ssh. If Linux is not reachable, power the board off, put the SD card in the PC, and write the saved or the proven payload to partition 2 by hand: `lsblk -o NAME,SIZE,LABEL,MODEL /dev/sdX` to find the card, then `sudo dd if=<file> of=/dev/sdX2 bs=1M conv=fsync` after reading the output. The script never does that itself. |
| **Hart 4 hangs in `RUN`.** | Hart 4 would stop answering; the tool says so after its timeout (default 10 s) and prints this recovery. Nothing persists: PMP registers live in the hart and reset with it, and the demo never sets the lock bit, so no entry it writes survives a reset. Linux and the other harts are not touched by a hang of hart 4. Reboot the board (power-cycle if Linux does not answer), then `deploy_payload.sh restore` if hart 4 still does not come up. A hang is believed unlikely: the handler is a few dozen instructions, a repeating fault ends after eight, and the interpreter test covers a user mode that cannot fetch. It cannot be ruled out until it has run on the hardware. |
| **A hang at boot.** | The only code that runs at boot is the read-only report. A hang there would be a CSR read or the console print; recovery as above. |
| **Locking.** | The demo never writes the lock bit: the plan has none (checked in the code and in a test of 500 random states), the register writer skips any entry whose new value has it, and a locked entry is never written. No change of the boot monitor, its flash, the eNVM, boot-mode settings, keys or fuses. |
| **Memory that Linux or the boot monitor uses is changed.** | Not by design. The accesses to Linux DDR and to the boot monitor's scratchpad are loads. The one store that is meant to be denied aims at the demo's own page. Hart 4's other stores in the test go to its own word and stack. The interpreter test checks that no store reaches those addresses. |
| **Interrupts are delayed.** | For a few microseconds, with `mie` cleared: the tick, the fabric latency interrupt and the mailbox interrupt are held pending and taken afterwards. A real-time deadline shorter than that is not protected during the test. |
| **The user-mode code finds the PMP behaving differently from the model.** | The run reports `NOT_EFFECTIVE`, `ABORTED` or `INSTALL_FAILED` and restores the registers; each is a result, not a failure of the lab. A fault in an unexpected place (an instruction fault) ends the excursion at once. |

## When it goes wrong

Only the first of these was met on the board.

| Symptom | What it would mean |
|---|---|
| `REFUSED: pmp0 is locked` (met) | The boot monitor locked entry 0 before the firmware started. Nothing was changed. This is the board's answer. |
| `INSTALL_FAILED` | The registers did not read back as written (a different number of entries, coarser granularity, or a write that did not take). They were put back; read the line that shows both values. |
| `NOT_EFFECTIVE` with `model: ... ALLOWED` | An entry at a higher number than the plan allows everything, and the plan cannot go below it. The model said so before the test. |
| `NOT_EFFECTIVE` with a different count of faults and no such line | The hardware does not match the model; this is the interesting case. Record the three trap lines. |
| `ABORTED`, `mcause` 1 | User mode could not fetch its own code: the plan's image entry did not take effect as written. |
| No answer to `RUN` | See the risk list. |

## Trade-offs and going further

- **A fence you can lock cannot be changed.** Locking an entry makes machine mode obey it and makes it permanent until reset, which is what a safety argument wants and what this lab deliberately does not do. The project's rule is that the board is never locked; a locked fence would be a decision about the boot flow, not an experiment.
- **PMP does not see other bus masters.** It constrains the harts' own accesses only. The fabric DMA and the MSS DMA are limited by other mechanisms (the memory protection unit of the MSS), which this lab does not touch.
- **A supervisor that runs in machine mode trusts itself.** The only protection it gets from PMP is for code it chooses to run in user mode. A design that wants a fence around the whole supervisor would run it in user mode and keep a small machine-mode kernel, which is a different firmware.
- **Exercise:** from the configuration lines in the first figure, work out which entry decides a user-mode store to `0x8020_0000`, and which decides a machine-mode load from `0x0A00_0000`. Then say what would change if bit 7 of `PMP0CFG` were 0.
- **Exercise:** the plan puts the canary entry before the image entry. Swap them on paper and predict which of the three probes changes its outcome.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [M01 Asymmetric multiprocessing on one chip](M01-asymmetric-multiprocessing.md) |
| **How to run** | Needs the complete integrated system |
| **In the system** | [U54 hart 4](../system/README.md#u54-4) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [E51 (hart 0)](../system/README.md#e51) · [Hart 4 image](../system/README.md#ddr-u54-4-image) · [RPMsg shared memory](../system/README.md#ddr-rpmsg) · [Hart Software Services](../system/README.md#hss) · [pfdaq supervisor](../system/README.md#supervisor) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) |
| **Source** | [`firmware/u54_4-supervisor/pfdaq_pmp.h`](../../firmware/u54_4-supervisor/pfdaq_pmp.h): `pmp_check`, `pmp_plan_build`, `pmp_can_install`, `pmp_first_locked`, `m09_run`, `m09_judge`<br>[`firmware/u54_4-supervisor/pfdaq_pmp_hw.h`](../../firmware/u54_4-supervisor/pfdaq_pmp_hw.h): `m09_user_run`, `m09_trap`, `m09_hw_write_state`<br>[`firmware/u54_4-supervisor/pfdaq_supervisor.c`](../../firmware/u54_4-supervisor/pfdaq_supervisor.c): `PFDAQ_AMP_M09`, `m09_handle`<br>[`firmware/u54_4-supervisor/disco-kit/MPFS_DISCOVERY_KIT_MSS_mss_cfg.xml`](../../firmware/u54_4-supervisor/disco-kit/MPFS_DISCOVERY_KIT_MSS_mss_cfg.xml): `PMP0CFG`, `CSR_PMPADDR0`, `MEM_CONFIGS_ENABLED`<br>[`experiments/amp-m09/m09_ctl.c`](../../experiments/amp-m09/m09_ctl.c)<br>[`experiments/amp-m09/tests/sim_rv64.py`](../../experiments/amp-m09/tests/sim_rv64.py)<br>[`experiments/amp-m09/build_firmware.sh`](../../experiments/amp-m09/build_firmware.sh)<br>[`experiments/amp-m09/deploy_payload.sh`](../../experiments/amp-m09/deploy_payload.sh)<br>[`experiments/amp-m09/run_m09.sh`](../../experiments/amp-m09/run_m09.sh) |
| **Evidence** | [The PMP report of hart 4 as m09_ctl prints it: 1 entry active, 1 locked (the register values are in m09-console)](../../evidence/v0.5.1/board/m09-report.txt) (board, fabric v0.5.1, 2026-11-16)<br>[The answer to RUN: REFUSED, 0 access faults, nothing installed](../../evidence/v0.5.1/board/m09-run.txt) (board, fabric v0.5.1, 2026-11-16)<br>[The hart 4 console during the M09 session: the report, RUN requested, REFUSED, the report again](../../evidence/v0.5.1/board/m09-console.txt) (board, fabric v0.5.1, 2026-11-16)<br>[20 RPMsg pings to hart 4 after the refused test: all answered](../../evidence/v0.5.1/board/m09-ping-after.txt) (board, fabric v0.5.1, 2026-11-16) |
| **Related** | [M01 Asymmetric multiprocessing on one chip](M01-asymmetric-multiprocessing.md)<br>[M02 RPMsg in practice: announce, bind, ping](M02-rpmsg-in-practice.md)<br>[M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md)<br>[M04 Same core, same code: FreeRTOS against bare metal](M04-freertos-vs-bare-metal.md)<br>[M05 Shared memory between Linux and a bare-metal hart](M05-shared-memory-linux-bare-metal.md)<br>[M10 A four-core monitoring station: acquire, analyse, report, protect](M10-four-core-monitoring-station.md)<br>[B02 The HSS payload: starting two operating systems](B02-hss-payload.md) |
| **Status** | validated |
<!-- nav:end -->
