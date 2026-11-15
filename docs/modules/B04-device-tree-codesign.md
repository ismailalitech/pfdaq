---
id: B04
title: "Device tree for co-design: reserved memory, UIO nodes, a disabled CPU"
track: boot
level: 2
summary: The device-tree changes that make room for a second operating system and expose fabric logic to user space.
needs: [F02]
repro: base
status: validated
hardware: [u54-4, ddr-u54-4-image, ddr-rpmsg, ddr-noncached, ddr-cached-shared, pfdaq-stream-src, ihc]
software: [linux-kernel, uio, udmabuf, remoteproc-rpmsg]
sources:
  - yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts:contextb_reserved,vdev0vring0,vdev0buffer,rsctable,rproc_contextb,pfdaq_src,cpu4
  - yocto/meta-pfdaq/recipes-kernel/linux/linux-mchp_6.%.bbappend:do_configure
  - yocto/meta-pfdaq/recipes-bsp/u-boot/files/envs/mpfs-disco-kit-amp.txt:mpfs_set_bootargs
evidence: [boot-uboot-linux, suite-freertos, supervisor-console]
related: [B01, B02, B03, I01, I05, M01, M05]
---

# Device tree for co-design: reserved memory, UIO nodes, a disabled CPU

After this page you can read the 74-line device tree that lets Linux share a chip with a second program, say what each node does, and predict what Linux would do wrong without it.

## Before you start

- **Level 2.** You should know what a device tree is: a description of the hardware that the kernel reads at boot, instead of having it built in. Module F02 explains the memory map this page refers to.
- **Board:** the shared baseline.

## The idea

A normal Linux owns the whole machine: every core, all the memory, every device. Running a second program beside it means taking some of that away **in the description Linux is given**. Linux does not look at the hardware to find out what is free; it believes the device tree. So three things have to be written there:

1. **The core that Linux must not use** is marked disabled.
2. **The memory that Linux must not touch**, and the memory the two sides will share, are *reserved*.
3. **The hardware that user space will use** is described as a device Linux can hand over without a driver of its own.

## On this board

The file is `mpfs-disco-kit-amp.dts`. It starts by including the vendor's Discovery Kit tree and then overrides and adds:

```text
#include "mpfs-disco-kit.dts"

/ {
	model = "Microchip PolarFire-SoC Discovery Kit (AMP)";
```

Its own comment says where the AMP parts come from: they follow the vendor's overlay for the other board's AMP configuration, and "MMUART0 (context B console) and wdt4 are already disabled in mpfs-disco-kit.dts". The result is the model string the kernel prints at boot.

### 1. The disabled core

```text
&cpu4 {
	status = "disabled"; /* owned by context B */
};
```

Core 4 (a U54) is the supervisor's. Linux therefore boots with three CPUs. Note that disabling it here does not stop anyone from starting it: the boot monitor starts the supervisor before Linux exists (module B01). The device tree only makes sure that Linux does not also try to schedule work there.

### 2. Reserved memory

```text
reserved-memory {
	contextb_reserved: contextb_reserved@91c00000 { reg = <0x0 0x91c00000 0x0 0x100000>; no-map; };
	vdev0vring0:  vdev0vring0@91d00000  { reg = <0x0 0x91d00000 0x0 0x8000>;  no-map; };
	vdev0vring1:  vdev0vring1@91d08000  { reg = <0x0 0x91d08000 0x0 0x8000>;  no-map; };
	vdev0buffer:  vdev0buff@91d10000    { compatible = "shared-dma-pool"; reg = <0x0 0x91d10000 0x0 0x40000>; no-map; };
	rsctable:     rsc-table@91d50000    { reg = <0x0 0x91d50000 0x0 0x1000>;  no-map; };
};
```

(Shortened to one line per region; the file has one block each.) `no-map` means Linux does not even create a mapping for the region: it cannot read or write it by accident. The vendor tree adds three more regions that this project uses, shown here together with the project's:

| Address | Size | Name | Who uses it | Linux sees it as |
|---|---|---|---|---|
| `0x91C0_0000` | 1 MiB | `contextb_reserved` | the supervisor's code and data | nothing (`no-map`) |
| `0x91D0_0000` | 32 KiB | `vdev0vring0` | RPMsg virtqueue, one direction | the remoteproc "vring0" |
| `0x91D0_8000` | 32 KiB | `vdev0vring1` | RPMsg virtqueue, the other direction | "vring1" |
| `0x91D1_0000` | 256 KiB | `vdev0buffer` | the message buffers | "buffer" |
| `0x91D5_0000` | 4 KiB | `rsctable` | the resource table the supervisor copies there at start | "rsc-table" |
| `0x10_3FC0_0000` | 2 MiB | `hss-buffer` (vendor) | the boot monitor's copy of the payload | nothing |
| `0x8800_0000` | 32 MiB | `fabricbuf0ddrc` (vendor) | cached shared memory | `udmabuf-ddr-c0` |
| `0xC800_0000` | 32 MiB | `fabricbuf1ddrnc` (vendor) | non-cached buffers and DMA descriptors | `udmabuf-ddr-nc0` |
| `0xD800_0000` | 32 MiB | `fabricbuf2ddrncwcb` (vendor) | non-cached write-combining window | `udmabuf-ddr-nc-wcb0` (not used by the project) |

The three `udmabuf` nodes in the vendor tree turn each fabric buffer region into a device node user space can open and map (`/dev/udmabuf-ddr-c0` and so on). That is how `pfdaq_cap` reaches the DMA buffers (module I05) and why the stream source's data can be read by an ordinary program.

The first five rows are what makes the second context possible; they must agree with three other places, or the system fails in confusing ways:

- the **payload**, which loads the supervisor at `0x91C0_0000` (module B02);
- the **firmware's linker script**, which links the supervisor for that address;
- the **firmware's resource table**, which names the ring and buffer addresses (the supervisor copies it to `0x91D5_0000` at start).

### 3. The remoteproc node

```text
rproc_contextb: remote-context {
	compatible = "microchip,ipc-sbi-remoteproc";
	memory-region = <&contextb_reserved>, <&rsctable>, <&vdev0vring0>, <&vdev0buffer>, <&vdev0vring1>;
	memory-region-names = "firmware", "rsc-table", "vring0", "buffer", "vring1";
	mboxes = <&ihc 8>;
	status = "okay";
};
```

This describes the supervisor to Linux's remoteproc framework: where its firmware is, where its resource table and message rings are, and which mailbox entry of the inter-hart communication block (`&ihc`, enabled lower in the file) carries its notifications. It is why Linux can say "attaching to remote-context" instead of searching for firmware to load (module B01).

### 4. The fabric register window as a UIO device

```text
pfdaq_src: uio@40000f00 {
	compatible = "generic-uio";
	linux,uio-name = "pfdaq_stream_src";
	reg = <0x0 0x40000f00 0x0 0x100>;
	interrupt-parent = <&plic>;
	interrupts = <119>;
};
```

The project's fabric logic has no kernel driver. This node describes its 256-byte register window and its interrupt (PLIC source 119, which is fabric line 1 plus 118, module F03) as a `generic-uio` device. The kernel's generic UIO driver binds to it because U-Boot puts `uio_pdrv_genirq.of_id=generic-uio` on the kernel command line (module B01). The driver then offers a character device that user space can map and wait on; `linux,uio-name` is the name the tools search for in `/sys/class/uio/uio*/name`.

Two details matter. UIO maps whole pages, so the window at `0x40000F00` is reached by mapping the page at `0x40000000` and adding `0xF00`; and an earlier direct `/dev/mem` approach had needed three fabric drivers unbound after every boot, which this node replaced (decision D20). The DMA controller's UIO device comes from the vendor tree (`dma-controller@60010000`, enabled there) and a vendor kernel driver, not from this file.

### 5. How the file gets into the kernel

The kernel recipe (module B03) copies the file into the kernel source tree and appends one line to the tree's device-tree Makefile. The machine configuration then selects the result, and U-Boot loads it from the `fitImage` on the card together with the kernel.

## Evidence

- **Board, cold boot:** the lines above (`boot-uboot-linux`), and the supervisor's side of the same handshake (`supervisor-console`).
- **Board, memory:** the suite's shared-memory test has Linux write a pattern into a reserved region, the supervisor verify it and write the inverse, and Linux verify that: no mismatch in 50 passes of 65,536 words, in the non-cached region (`udmabuf-ddr-nc0`) and the cached one (`udmabuf-ddr-c0`) (`suite-freertos`, rows T7-noncached and T7-cached). It shows that both sides see the same memory at the reserved addresses.
- **Board, UIO and DMA buffers:** every burst test of the suite reads the fabric's data through these devices.

## When it goes wrong

Except for the `mmap` row, these are reasoned from what each node does; none was triggered on purpose.

| Symptom | Cause |
|---|---|
| Linux crashes or corrupts the supervisor at random | the supervisor's region is not `no-map`, or overlaps memory Linux was given |
| `remoteproc` finds no resource table | the `rsctable` region disagrees with where the supervisor copies its table |
| No `uio` device appears for the fabric window | the `generic-uio` word is missing from the kernel command line, or the node's `compatible` was changed |
| `mmap` of the register window fails | the window is not page aligned: map the page and add the offset (an earlier kernel-driver version failed this way; see the failures table in the validation plan) |
| Linux brings up four CPUs | `&cpu4` is not disabled; Linux would schedule onto the supervisor's core |

## Trade-offs and going further

- **A reservation is a promise.** Nothing stops a mistake in the payload, the linker script and this file from disagreeing. The memory map is documented in one table (above and in the architecture reference) for that reason.
- **`no-map` costs nothing and prevents a class of bugs**, but it also hides the region from Linux entirely. That is why the shared-memory test uses separate regions that Linux does map.
- **Exercise:** the cached region at `0x8800_0000` passes the shared-memory test with no cache maintenance, on this chip. Why is that not true in general, and what would you add to the test to find out where it stops being true?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [F02 The memory map and the three faces of DDR](F02-memory-map-ddr.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [U54 hart 4](../system/README.md#u54-4) · [Hart 4 image](../system/README.md#ddr-u54-4-image) · [RPMsg shared memory](../system/README.md#ddr-rpmsg) · [Non-cached window](../system/README.md#ddr-noncached) · [Cached shared region](../system/README.md#ddr-cached-shared) · [Stream source](../system/README.md#pfdaq-stream-src) · [Mi-V inter-hart communication](../system/README.md#ihc) · [Linux 6.18.17](../system/README.md#linux-kernel) · [UIO devices](../system/README.md#uio) · [u-dma-buf](../system/README.md#udmabuf) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) |
| **Source** | [`yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts`](../../yocto/meta-pfdaq/recipes-kernel/linux/files/mpfs-disco-kit-amp.dts): `contextb_reserved`, `vdev0vring0`, `vdev0buffer`, `rsctable`, `rproc_contextb`, `pfdaq_src`, `cpu4`<br>[`yocto/meta-pfdaq/recipes-kernel/linux/linux-mchp_6.%.bbappend`](../../yocto/meta-pfdaq/recipes-kernel/linux/linux-mchp_6.%.bbappend): `do_configure`<br>[`yocto/meta-pfdaq/recipes-bsp/u-boot/files/envs/mpfs-disco-kit-amp.txt`](../../yocto/meta-pfdaq/recipes-bsp/u-boot/files/envs/mpfs-disco-kit-amp.txt): `mpfs_set_bootargs` |
| **Evidence** | [Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with FreeRTOS on hart 4, 27 of 27 functional tests](../../evidence/v0.5.1/board/suite-freertos.md) (board, fabric v0.5.1, 2026-11-15)<br>[Supervisor console during the fresh-card suite, with the alarm and recovery messages](../../evidence/v0.5.1/board/supervisor-console.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [B01 From power-on to login, stage by stage](B01-power-on-to-login.md)<br>[B02 The HSS payload: starting two operating systems](B02-hss-payload.md)<br>[B03 A Yocto layer for a board the vendor does not support in AMP](B03-yocto-layer.md)<br>[I01 Reaching fabric registers from Linux](I01-fabric-registers-from-linux.md)<br>[I05 Buffers the DMA can write: non-cached windows and u-dma-buf](I05-dma-buffers.md)<br>[M01 Asymmetric multiprocessing on one chip](M01-asymmetric-multiprocessing.md)<br>[M05 Shared memory between Linux and a bare-metal hart](M05-shared-memory-linux-bare-metal.md) |
| **Status** | validated |
<!-- nav:end -->
