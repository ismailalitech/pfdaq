---
id: M01
title: "Asymmetric multiprocessing on one chip"
track: multicore
level: 2
summary: Why one hart is taken away from Linux, and what that costs and buys.
needs: [B02]
repro: doc
status: validated
hardware: [e51, u54-linux, u54-4, ddr-u54-4-image, ddr-rpmsg]
software: [hss, opensbi, u-boot, linux-kernel, supervisor, remoteproc-rpmsg]
sources:
  - yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in:skip-opensbi,owner-hart
  - yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts:contextb_reserved,rproc_contextb
  - yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf:HSS_PAYLOAD
evidence: [boot-hss, boot-uboot-linux, suite-freertos]
related: [M02, M03, M04, B01, K11, K12, F03, P02]
---

# Asymmetric multiprocessing on one chip

After this module you can say why this project runs two software systems on one chip, which core runs what and in which privilege mode, what the boot monitor and the device tree each have to do to make that work, and what the arrangement costs. It is a reading module: every claim is checked against the project's recorded boot output, and the commands below only search those records.

## Before you start

- **Level 2.** You should know that the PolarFire SoC has one monitor core (E51) and four application cores (U54), and roughly what a privilege mode is.
- **Read first:** B02, the boot payload (planned). Without it, take the YAML in the next section on trust.
- **Tools:** none. The "Run it" steps only read the evidence files in this repository.

## The idea

Most multicore chips run one operating system on all their cores. That is **symmetric multiprocessing** (SMP): Linux decides which core runs what, and every core looks the same to it. **Asymmetric multiprocessing** (AMP) does the opposite on purpose: each core, or group of cores, runs its own software, and the groups are not scheduled by one another.

The reason to do it is timing. A general-purpose kernel is built to share many cores fairly among many jobs. That makes it fast on average and poor at promising an upper bound on how long any one reaction takes. A small program on a core of its own, with its own interrupt line and nothing else to run, can promise much more. Module P02 shows the size of the difference on this board.

What AMP needs, whatever the chip:

1. **A way to start each software system** on its own core, in its own privilege mode.
2. **A way to keep them out of each other's memory**, so that one cannot overwrite the other.
3. **A way for them to talk**: shared memory plus a notification (module M02).
4. **An agreement about every shared device**, such as the interrupt controller or a UART.

## On this board

| Core | Software | Privilege mode | Started by |
|---|---|---|---|
| E51 (hart 0) | Hart Software Services (HSS), the boot monitor | machine | the chip, from its boot flash |
| U54 harts 1 to 3 | U-Boot, then Linux 6.18, one SMP system | supervisor, under OpenSBI | the HSS |
| U54 hart 4 | the pfdaq supervisor, on FreeRTOS or on bare metal | machine, no OpenSBI | the HSS, at the same moment |

There is no hypervisor. The separation is made of four plain mechanisms, one per need above.

### 1. Starting the two systems: the payload

The HSS reads a **payload** from partition 2 of the SD card. A short YAML file says what runs on which core. This is the project's file, `amp.yaml.in`:

```yaml
hart-entry-points: {u54_1: '0x80200000', u54_2: '0x80200000', u54_3: '0x80200000', u54_4: '0x91C00000'}

payloads:
  u-boot.bin: {exec-addr: '0x80200000', owner-hart: u54_1, secondary-hart: u54_2, secondary-hart: u54_3, priv-mode: prv_s}
  @@AMP_PAYLOAD@@: {exec-addr: '0x91C00000', owner-hart: u54_4, priv-mode: prv_m, skip-opensbi: true, skip-autoboot: @@AMP_SKIP-AUTOBOOT@@}
```

Two lines, two systems. U-Boot goes to hart 1, with harts 2 and 3 as secondaries, in supervisor mode. The firmware for hart 4 goes to its own address in machine mode, and `skip-opensbi: true` tells the HSS not to put the OpenSBI layer under it. The placeholders are filled in by the build: `freertos` or `bm` for the firmware (module M04).

The boot record shows the HSS doing exactly that, at the same time for both:

```text
[4.606722] boot_service(u54_4):: BOOT_FLAG_SKIP_OPENSBI found
[4.621426] boot_service(u54_1)::Registering domain "u-boot.bin" (hart mask 0xe)
```

`0xe` is binary 1110: harts 1, 2 and 3. Hart 4 is not in the Linux domain, and it reaches the running state at 4.86 s on the HSS clock, while the three Linux harts are still booting:

```text
[4.862061] u54 State Change:  [Booting] [Booting] [Booting] [Running]
```

### 2. Keeping them apart: memory and the device tree

Linux has to be told that hart 4 is not its to schedule, and that some memory is not its to use. Both are in the device tree, `mpfs-disco-kit-amp.dts`:

```dts
&cpu4 {
	status = "disabled"; /* owned by context B */
};
```

and, in `reserved-memory`:

```dts
contextb_reserved: contextb_reserved@91c00000 {
	reg = <0x0 0x91c00000 0x0 0x100000>;
	no-map;
};
```

`no-map` means the kernel does not even map the region, so a stray kernel pointer cannot reach it. The table in the architecture reference lists the whole layout; the pieces that matter here:

| Address | Size | Owner |
|---|---|---|
| `0x91C0_0000` | 1 MiB | the hart 4 firmware image |
| `0x91D0_0000`, `0x91D0_8000` | 2 × 32 KiB | the two RPMsg rings |
| `0x91D1_0000` | 256 KiB | the RPMsg buffer pool |
| `0x91D5_0000` | 4 KiB | the resource table |

The HSS also sets up memory protection for each core before it starts it. The boot record shows a "SetupPMP" step for every hart. What those protection settings contain was not examined for this module, and the isolation described here is therefore as strong as these mechanisms are, not something this project tested by attacking it.

### 3. Talking: a mailbox and shared rings

Linux is made aware of the second system through a **remoteproc** node in the same device tree: it names the reserved regions and a mailbox channel on the Mi-V inter-hart communication block (`mboxes = <&ihc 8>`). Linux attaches to hart 4 instead of starting it, because the HSS already did. The Linux log shows the attach:

```text
[    1.709490] remoteproc remoteproc0: remote-context is available
[    1.715628] remoteproc remoteproc0: attaching to remote-context
[    1.751180] remoteproc remoteproc0: remote processor remote-context is now attached
```

From there the two systems exchange messages (module M02).

### 4. Shared devices: who gets which interrupt

The PLIC is shared. Each core has its own enable bits, so Linux enables the sources it handles and hart 4 enables its own, with no software agreement needed beyond which source goes where. That split is module F03. The hart 4 console is a UART that Linux does not use (MMUART0, routed through the fabric); Linux's console is another (MMUART4).

## Run it

All steps search the recorded evidence. Nothing here needs a board.

1. See that the monitor starts both systems and leaves hart 4 out of the Linux domain:

   ```sh
   grep -n 'SKIP_OPENSBI\|Registering domain\|State Change' evidence/v0.5.1/board/boot-hss.txt
   ```

2. See Linux attach to hart 4 and the channel appear:

   ```sh
   grep -n 'Booting Linux\|remoteproc\|rpmsg' evidence/v0.5.1/board/boot-uboot-linux.txt
   ```

3. Read the two device-tree lines that make room:

   ```sh
   grep -n -B1 -A2 'cpu4\|contextb_reserved@' yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts
   ```

## What you should see

Step 1 prints nine lines. The important ones are the three quoted above and the last, which says when every core is running:

```text
[22.485161] u54 State Change:  [Running] [Running] [Running] [Running]
```

Hart 4 reported running 17.6 s earlier, at 4.86 s. Both times are on the HSS's own clock; the Linux boot itself is timed on Linux's clock in step 2, where the same boot attaches to hart 4 at 1.75 s after the kernel starts.

Step 2 shows Linux starting on **hart 1** (`Booting Linux on hartid 1`), then the five lines of remoteproc and RPMsg. The channel line is the last one:

```text
[    1.938361] virtio_rpmsg_bus virtio0: creating channel pfdaq-supervisor addr 0x0
```

## Evidence

- **Boot records (board, fabric v0.5.1):** `boot-hss` and `boot-uboot-linux`, cold boot of the final image, give every line quoted above. The HSS was v0.99.53-v2026.04.1, as found in the chip's boot flash; the payload it copied was 1,376,656 bytes.
- **Memory visibility across the two systems (board):** in the board suite, Linux writes a pattern into a shared region, hart 4 checks it and writes the inverse, and Linux checks that. Result: no mismatch in either direction, in the non-cached region (31.15 ms per check) and in the cached region (20.10 ms per check), with no cache maintenance. See the T7 rows of `suite-freertos`.
- **Order of starting:** hart 4 reported running at 4.86 s on the HSS clock; the Linux harts reported running together with it only at 22.49 s. Starting the supervisor from the HSS, and not from Linux, is what makes it available that early.

## When it goes wrong

| Symptom met in this project | What it was |
|---|---|
| Linux stops hart 4 through remoteproc, but cannot start it again | the HSS rejects the start request; recovery is a reboot (K11) |
| Once in about sixty boots the supervisor did not start: no console output and no channel | not reproduced and not explained (K12) |

Mismatches between the device tree and the firmware (wrong ring addresses, `cpu4` left enabled, a region not `no-map`) were not met during this project, so no symptom is recorded for them.

## Trade-offs and going further

- **Two contexts is the vendor's maximum.** The architecture reference records "two contexts is the vendor maximum" for this boot flow, so a third software system is not available on this chip with this boot monitor.
- **No restart from Linux.** Linux can stop hart 4 but the HSS in this chip's flash rejects the request to start it again; recovery is a reboot. K11 explains.
- **The cores still share the L2 cache, the memory controller and the FIC3 bus.** The latency record shows hart 4's worst case grows a little when Linux is loaded (P02); the cause was not investigated, so the isolation is good but not total.
- **What AMP buys here:** a handler that answers in 0.5 microseconds typically and was never late in 600,000 events, regardless of what Linux does (P02). The price is one core less for Linux and the effort of the mechanisms above.
- **Exercise:** in the output of step 1, find the order in which the HSS moves hart 4 and the Linux harts through the states Init, SetupPMP, ZeroInit and Download. Explain from the lines why hart 4 reaches Running first.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [B02 The HSS payload: starting two operating systems](B02-hss-payload.md) |
| **How to run** | Explanation with recorded evidence; nothing to run |
| **In the system** | [E51 (hart 0)](../system/README.md#e51) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [Hart 4 image](../system/README.md#ddr-u54-4-image) · [RPMsg shared memory](../system/README.md#ddr-rpmsg) · [Hart Software Services](../system/README.md#hss) · [OpenSBI](../system/README.md#opensbi) · [U-Boot](../system/README.md#u-boot) · [Linux 6.18.17](../system/README.md#linux-kernel) · [pfdaq supervisor](../system/README.md#supervisor) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) |
| **Source** | [`yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in`](../../yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in): `skip-opensbi`, `owner-hart`<br>[`yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts`](../../yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts): `contextb_reserved`, `rproc_contextb`<br>[`yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf`](../../yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf): `HSS_PAYLOAD` |
| **Evidence** | [Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [M02 RPMsg in practice: announce, bind, ping](M02-rpmsg-in-practice.md)<br>[M03 A real-time supervisor: deadlines and independent checks](M03-real-time-supervisor.md)<br>[M04 Same core, same code: FreeRTOS against bare metal](M04-freertos-vs-bare-metal.md)<br>[B01 From power-on to login, stage by stage](B01-power-on-to-login.md)<br>[K11 remoteproc cannot restart the hart](../case-studies/K11-remoteproc-restart.md)<br>[K12 One boot in sixty, FreeRTOS never started](../case-studies/K12-freertos-no-start.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md)<br>[P02 Interrupt latency: bare metal, RTOS task, kernel, user space](P02-interrupt-latency.md) |
| **Status** | validated |
<!-- nav:end -->
