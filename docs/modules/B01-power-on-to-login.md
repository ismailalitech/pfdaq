---
id: B01
title: "From power-on to login, stage by stage"
track: boot
level: 1
summary: "System controller, HSS, OpenSBI, U-Boot, Linux: what each stage does and what it hands to the next."
needs: [F01]
repro: doc
status: validated
hardware: [soc, e51, u54-linux, u54-4, envm, sd-card]
software: [hss, opensbi, u-boot, linux-kernel, supervisor, remoteproc-rpmsg]
sources:
  - yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in:hart-entry-points
  - yocto/meta-pfdaq/recipes-bsp/u-boot/files/envs/mpfs-disco-kit-amp.txt:bootcmd,mpfs_set_bootargs
  - yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/mpfs-disco-kit-amp.cfg
evidence: [boot-hss, boot-uboot-linux, reboot-30, supervisor-console]
related: [B02, B04, M01, K11, K12, S02]
---

# From power-on to login, stage by stage

After this page you can read a PolarFire SoC boot log and say which stage printed each line, what that stage was handed, what it hands on, and where a boot that stops has stopped. Every line quoted below comes from one cold boot of the final image, recorded on the board.

## Before you start

- **Level 1.** You should know that the chip has one small monitor core (E51) and four application cores (U54), and that the FPGA fabric is configured before software runs. Module F01 covers that.
- **Tools:** none. This is a reading page.

## The idea

A system with several processors and several operating systems cannot just "start the CPU". Something has to bring up memory, decide which image runs on which core in which privilege mode, load those images, and start the cores in the right order. Each stage here does one part of that and then hands over to a stage that is bigger, slower to start and less trusted.

```text
power on
  → system controller: configures the FPGA fabric, releases the processor subsystem
  → E51 runs the Hart Software Services (HSS) from the chip's boot flash (eNVM)
  → HSS trains DDR, reads the payload from the SD card, starts the four U54 cores
        U54 1-3: OpenSBI → U-Boot → Linux          (supervisor mode)
        U54 4:   the real-time supervisor directly   (machine mode, no OpenSBI)
  → Linux attaches to the running supervisor through remoteproc
```

## On this board

| Stage | Runs on | Stored in | Does | Hands over |
|---|---|---|---|---|
| System controller | its own small core | inside the chip | configures the FPGA fabric from the programmed design and releases the processor subsystem | a running E51 |
| HSS v0.99.53-v2026.04.1 | E51 | eNVM, inside the chip | trains DDR, reads the payload, starts the U54 cores, keeps monitoring | one payload, four started cores |
| OpenSBI v1.2 | U54 1 (then 2 and 3) | inside the HSS | gives supervisor-mode software the machine-mode services it needs | U-Boot in supervisor mode |
| U-Boot 2025.07 | U54 1 | the payload, SD partition 2 | loads the kernel image and device tree from the SD card | Linux |
| Linux 6.18.17 | U54 1-3 | SD card, partitions 1 and 3 | runs the system; attaches to hart 4 | a login prompt |
| pfdaq supervisor | U54 4 | the payload | supervises and answers interrupts | an RPMsg channel, announced once Linux is up |

This project never writes the eNVM. The HSS in it is the one that came on the board, and the fabric programming jobs contain the fabric and sNVM components only (decision D12). Everything after the HSS lives on the SD card and can be rewritten from a PC.

### Stage 1: the boot monitor starts

The first lines of the recorded boot monitor console:

```text
[0.52230] Initializing Mi-V IHC V2
[0.56909] u54 State Change:  [Idle] [Idle] [Idle] [Idle]
[0.781541] Initializing PMPs
[0.785647] PolarFire(R) SoC Hart Software Services (HSS) - version 0.99.53-v2026.04.1
incorporating OpenSBI - version 1.2
[0.921229] DESIGNID: MPFS_DISCOVERY_KIT
[0.926671] DESIGNVER: 00.00.6
```

The numbers in brackets are seconds since the monitor started. Four things worth reading here:

- **"Initializing Mi-V IHC V2"** is the monitor opening the mailbox it will later share with Linux and the supervisor. It is also the last line a bad fabric build once reached: with its pin constraints missing, the board sat in a reset loop at this point (case study K08).
- **"Initializing PMPs"**: physical memory protection, the hardware that decides which core may touch which addresses, is configured before any other software runs.
- **"incorporating OpenSBI - version 1.2"**: OpenSBI is not a separate file. It is part of the monitor.
- **DESIGNID and DESIGNVER** are read from the programmed fabric. `00.00.6` is design version 6, the project's v0.5.1 fabric: this is how you confirm from a console which design is running.

Then the monitor prints its memory setup (cached and non-cached DDR windows, L2 cache split) and waits one second for a keypress ("Press a key to enter CLI, ESC to skip", then "CLI boot interrupt timeout") before it carries on.

### Stage 2: the payload is read from the SD card

```text
[4.399843] Initializing Boot Image ...
[4.410823] Attempting to select SDCARD ... Passed
[4.448555] Validated GPT Header ...
[4.486903] Boot Partition found at index 1
[4.492424] Attempting to read image header (1632 bytes) ...
[4.501503] Copying 1376656 bytes to 0x103fc00000
[4.570249] Boot image set name: "PolarFire-SoC-HSS::Disco Linux + freertos AMP"
```

The monitor reads the SD card's partition table (GPT), finds its payload partition (index 1 counting from zero, which Linux calls partition 2) and reads a 1632-byte header. It then copies the whole payload, 1,376,656 bytes, into DDR at `0x10_3FC0_0000`, a region that the Linux device tree reserves so Linux never touches it (module B04). The `set name` comes from the payload's own configuration (module B02).

### Stage 3: each application core is set up and started

```text
[4.606722] boot_service(u54_4):: BOOT_FLAG_SKIP_OPENSBI found
[4.621426] boot_service(u54_1)::Registering domain "u-boot.bin" (hart mask 0xe)
[4.755671] boot_service(u54_4)::Processing boot image: "polarfire-soc-freertos-amp-demo.elf"
[4.862061] u54 State Change:  [Booting] [Booting] [Booting] [Running]
```

Every core goes through the same states (`SetupPMP`, `ZeroInit`, `Download`, then `OpenSBIInit`, `Wait`, `Complete`, `Idle`), one line per transition. Three things differ between cores:

- **Hart 4 skips OpenSBI.** The flag `BOOT_FLAG_SKIP_OPENSBI` means the monitor starts it directly in machine mode. Its state line `[Running]` appears at 4.86 s, **before Linux exists**. The supervisor is already running when U-Boot starts.
- **Cores 1 to 3 form one domain.** "hart mask 0xe" is binary 1110: harts 1, 2 and 3 share the image `u-boot.bin`.
- **Each core copies only its own chunks.** On hart 4, `Skipping chunk 103fc00660 due to ownership 1` is the monitor noting that this part of the image belongs to hart 1, not to it.

### Stage 4: OpenSBI, then U-Boot

The Linux-side console picks up from here (key lines of the recorded capture):

```text
OpenSBI v1.2
U-Boot 2025.07-linux4microchip-2026.04 (Apr 29 2026 - 08:52:30 +0000)
DRAM:  1 GiB (total 1.8 GiB)
Hit any key to stop autoboot:  2  1  0
## Loading kernel (any) from FIT Image at 8e000000 ...
Starting kernel ...
```

U-Boot runs a short script, `bootcmd`, from its environment file `uboot.env` on SD partition 1:

```text
bootcmd=run mpfs_set_bootargs; run distro_bootcmd
mpfs_set_bootargs=... setenv bootargs earlycon root=/dev/mmcblk0p${rootpart} rootwait uio_pdrv_genirq.of_id=generic-uio clk_ignore_unused
```

It sets the kernel command line (root filesystem on partition 3; the `uio_pdrv_genirq.of_id=generic-uio` word is what lets Linux hand fabric registers to user space, see B04), then scans the SD card for something to boot and finds the kernel image, `fitImage`, at `0x8e00_0000`.

### Stage 5: Linux, and the handshake with hart 4

```text
[    0.000000] Booting Linux on hartid 1
[    0.000000] Machine model: Microchip PolarFire-SoC Discovery Kit (AMP)
[    1.709490] remoteproc remoteproc0: remote-context is available
[    1.715628] remoteproc remoteproc0: attaching to remote-context
[    1.751180] remoteproc remoteproc0: remote processor remote-context is now attached
[    1.938361] virtio_rpmsg_bus virtio0: creating channel pfdaq-supervisor addr 0x0
[    2.063078] Run /sbin/init as init process
```

Linux says "attaching", not "starting". The supervisor is already running, so remoteproc only connects to it, and the message channel `pfdaq-supervisor` appears. On the supervisor's own console (the third serial port) the other end of the handshake is recorded:

```text
[pfdaq-sup] supervisor starting (FreeRTOS), stream source ID 50464451 (ok)
[pfdaq-sup] channel "pfdaq-supervisor" announced, state IDLE
```

## Evidence

- **One cold boot of the final image** is recorded on the boot monitor's console, the Linux console and the supervisor's console (`boot-hss`, `boot-uboot-linux`, `supervisor-console`). Only the lines that mark stages are kept from the Linux console; the full captures are not published.
- **Boot reliability:** 30 warm reboots over SSH, each checked for remoteproc attached, the RPMsg channel present and a supervised capture passing: 30 of 30. From the reboot command to SSH answering took 45.9 to 61.4 s (`reboot-30`). In the monitor's own timeline, the application cores are all `Running` at 22.5 s; how the remaining time to an answering SSH server divides up was not measured.
- **Not recorded:** the system controller's part (fabric configuration) prints nothing on any console used here, and the DDR training messages come before the first line of the record.

## When it goes wrong

| What you see | Where the boot stopped | Look at |
|---|---|---|
| Nothing on any console | before the monitor: power, or the fabric programming | the fabric job |
| Monitor restarts again and again after "Initializing Mi-V IHC" | the fabric | pin constraints missing from the build (case study K08) |
| Monitor prints, then "Trying to get boot image via MMC" fails | the SD card or its partition table | the card; the monitor needs a GPT with the payload partition |
| Monitor reaches `Running` for all cores but U-Boot never prints | the payload | the payload's U-Boot entry and the `hart-entry-points` (module B02) |
| U-Boot prints, kernel does not start | partition 1 | `fitImage` and `uboot.env` on the card |
| Linux boots but no `pfdaq-supervisor` channel | hart 4 | the supervisor's console; once in about sixty boots the supervisor did not start (case study K12) |

## Trade-offs and going further

- **Early boot of the second context** (hart 4 before Linux) means supervision exists while Linux is still loading, and it does not depend on Linux. The price is that Linux cannot restart that context: the monitor rejects the request (case study K11).
- **The monitor is in the chip's boot flash**, which makes the SD card freely rewritable and the monitor effectively a fixed part of the board. Making it verify a signed payload would change that flash; module S02 explains why this project did not.
- **Exercise:** from the monitor record, compute how long the monitor took from "Initializing Boot Image" to hart 4 `Running`. Which of the three steps in between (reading the card, copying the payload, setting up the cores) takes the most?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [F01 The chip and the board: MSS, fabric and the interfaces between them](F01-chip-and-board.md) |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [MPFS095T system-on-chip](../system/README.md#soc) · [E51 (hart 0)](../system/README.md#e51) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [eNVM boot flash](../system/README.md#envm) · [microSD card](../system/README.md#sd-card) · [Hart Software Services](../system/README.md#hss) · [OpenSBI](../system/README.md#opensbi) · [U-Boot](../system/README.md#u-boot) · [Linux 6.18.17](../system/README.md#linux-kernel) · [pfdaq supervisor](../system/README.md#supervisor) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) |
| **Source** | [`yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in`](../../yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in): `hart-entry-points`<br>[`yocto/meta-pfdaq/recipes-bsp/u-boot/files/envs/mpfs-disco-kit-amp.txt`](../../yocto/meta-pfdaq/recipes-bsp/u-boot/files/envs/mpfs-disco-kit-amp.txt): `bootcmd`, `mpfs_set_bootargs`<br>[`yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/mpfs-disco-kit-amp.cfg`](../../yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/mpfs-disco-kit-amp.cfg) |
| **Evidence** | [Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Thirty warm reboots, both systems up and a supervised capture passing each time](../../evidence/v0.5.1/board/reboot-30.csv) (board, fabric v0.5.1, 2026-11-15)<br>[Supervisor console during the fresh-card suite, with the alarm and recovery messages](../../evidence/v0.5.1/board/supervisor-console.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [B02 The HSS payload: starting two operating systems](B02-hss-payload.md)<br>[B04 Device tree for co-design: reserved memory, UIO nodes, a disabled CPU](B04-device-tree-codesign.md)<br>[M01 Asymmetric multiprocessing on one chip](M01-asymmetric-multiprocessing.md)<br>[K11 remoteproc cannot restart the hart](../case-studies/K11-remoteproc-restart.md)<br>[K12 One boot in sixty, FreeRTOS never started](../case-studies/K12-freertos-no-start.md)<br>[S02 Secure boot on PolarFire SoC: the chain of trust, and what we left alone](S02-secure-boot.md) |
| **Status** | validated |
<!-- nav:end -->
