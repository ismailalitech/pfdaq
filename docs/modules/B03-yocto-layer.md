---
id: B03
title: "A Yocto layer for a board the vendor does not support in AMP"
track: boot
level: 3
summary: Machine definition, payload, device tree and firmware recipe for a configuration the BSP does not ship.
needs: [B01]
repro: base
status: validated
hardware: [sd-card, u54-linux, u54-4]
software: [linux-kernel, u-boot, hss, supervisor, uio, pfdaq-uio, pfdaq-cap, pfdaq-lat]
sources:
  - yocto/meta-pfdaq/conf/layer.conf:LAYERDEPENDS_meta-pfdaq,LAYERSERIES_COMPAT_meta-pfdaq
  - yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf:HSS_PAYLOAD,KERNEL_DEVICETREE,UBOOT_MACHINE,MACHINE_FEATURES
  - yocto/meta-pfdaq/recipes-bsp/u-boot/u-boot-mchp_%.bbappend
  - yocto/meta-pfdaq/recipes-kernel/linux/linux-mchp_6.%.bbappend:do_configure
  - yocto/meta-pfdaq/recipes-mchp/polarfire-soc-amp-examples/polarfire-soc-freertos-amp-demo.bbappend:do_pfdaq_overlay
  - yocto/meta-pfdaq/recipes-kernel/pfdaq-uio/pfdaq-uio_1.0.bb
evidence: [suite-fresh-card, boot-uboot-linux]
related: [B01, B02, B04, I03]
---

# A Yocto layer for a board the vendor does not support in AMP

After this page you can read a small Yocto layer that adds a machine the vendor's BSP does not provide, name the five things it had to supply, and rebuild the project's SD image from it.

## Before you start

- **Level 3.** You should know what a Yocto recipe, a `bbappend` and a machine configuration file are, and what the boot stages are (module B01).
- **Board and host:** the shared baseline. The build commands below are the ones the project ran. The Yocto build itself was not rerun while this page was written.

## The idea

Microchip's Yocto layer, `meta-mchp`, ships a machine for this board (`mpfs-disco-kit`) and an AMP machine for a different board (`mpfs-icicle-kit-amp`, Linux on three cores and a second context on the fourth). It does not ship the combination this project needs: the Discovery Kit in AMP. You could edit the vendor layer, but then every update of it conflicts with your changes. The Yocto way is a **separate layer** that sits on top and adds or overrides only what differs.

## On this board

The layer, `yocto/meta-pfdaq`, is registered in the build's layer list and declares what it depends on:

```text
BBFILE_PATTERN_meta-pfdaq = "^${LAYERDIR}/"
BBFILE_PRIORITY_meta-pfdaq = "10"
LAYERDEPENDS_meta-pfdaq = "meta-mchp-polarfire-soc-bsp"
LAYERSERIES_COMPAT_meta-pfdaq = "scarthgap"
```

![How the project layer is used: five parts of meta-pfdaq (machine, U-Boot files, kernel device tree, supervisor firmware, extras) and the vendor layers all go into bitbake mchp-base-image, built with MACHINE=mpfs-disco-kit-amp, which leaves the SD image, kernel image, device tree, payload and firmware in the deploy directory.](../diagrams/lab-b03-layer.svg)

1. The machine file names the machine and its device tree, U-Boot configuration and payload.
2. The U-Boot part supplies the payload template, the environment and a configuration fragment, all keyed on the machine name.
3. The kernel part copies the project's device tree into the kernel source.
4. The firmware part adds a step that builds the hart 4 supervisor from this repository's source.
5. The extras add the project's tools, the optional kernel module and the package list.
6. The vendor layers (OpenEmbedded-Core, meta-openembedded, meta-mchp) are not edited. They join the five parts in one build, `MACHINE=mpfs-disco-kit-amp bitbake mchp-base-image`, which leaves the SD image, the kernel image, the device tree, `payload.bin` and the firmware ELF in the deploy directory.

Its content, and what each part replaces:

| Path | What it supplies | Why the vendor layer cannot |
|---|---|---|
| `conf/machine/mpfs-disco-kit-amp.conf` | the machine | no AMP machine exists for this board |
| `recipes-bsp/u-boot/` | the payload description, the U-Boot environment, a configuration fragment | both are per machine, and keyed on the machine name |
| `recipes-kernel/linux/` | the device tree (module B04) | the reserved-memory map is board-specific |
| `recipes-mchp/polarfire-soc-amp-examples/` | the supervisor firmware, built from this repository's source | the vendor example has no Discovery Kit board |
| `recipes-pfdaq/`, `recipes-kernel/pfdaq-uio/`, `recipes-mchp/packagegroups/` | the project's tools, the optional kernel module and the list of packages for the image | project content |

### 1. The machine

```text
MACHINE_TYPE = "amp"
MACHINE_FEATURES:append = " amp"
DISTRO_FEATURES_BACKFILL:append = "${@bb.utils.contains('MCHP_FEATURES', 'mchpcore', ' hss-early-boot-freertos', '', d)}"
KERNEL_DEVICETREE ?= "microchip/mpfs-disco-kit-amp.dtb"
UBOOT_MACHINE = "microchip_mpfs_discokit_defconfig"
HSS_PAYLOAD = "amp"
MACHINEOVERRIDES .= ":mpfs:mpfs-disco-kit"
```

The file begins with a comment that says what it is: "Project port of mpfs-icicle-kit-amp... Not a vendor machine." Each line has a job:

- `MACHINE_FEATURES ... amp` makes the vendor's U-Boot recipe pick up the AMP payload logic.
- `hss-early-boot-freertos` tells that logic to start the FreeRTOS context at boot (module B02).
- `KERNEL_DEVICETREE` selects the device tree the layer adds.
- `UBOOT_MACHINE` selects the Discovery Kit's U-Boot configuration.
- `HSS_PAYLOAD = "amp"` names the payload description, `amp.yaml.in`.
- `MACHINEOVERRIDES ... :mpfs-disco-kit` pulls in the vendor's Discovery Kit files (boot script, package lists) that are keyed on that name.

### 2. The payload, the U-Boot environment and its configuration

`recipes-bsp/u-boot/u-boot-mchp_%.bbappend` is one line: it adds the layer's `files/` directory to the vendor recipe's search path. The files then supply three things:

- `amp.yaml.in`, the payload template (module B02);
- `envs/mpfs-disco-kit-amp.txt`, U-Boot's environment: the vendor Discovery Kit's, plus `clk_ignore_unused` on the kernel command line;
- `mpfs-disco-kit-amp.cfg`, a fragment that keeps the environment on the SD card's first partition:

```text
CONFIG_ENV_IS_IN_FAT=y
CONFIG_ENV_FAT_INTERFACE="mmc"
CONFIG_ENV_FAT_DEVICE_AND_PART="0:1"
CONFIG_ENV_FAT_FILE="uboot.env"
```

The first build of this machine failed at the last step, the image assembly, because it expected an environment file for the machine and there was none. Adding `envs/mpfs-disco-kit-amp.txt` fixed it. The notes record this; the build log is not published.

### 3. The device tree

The kernel recipe's `do_configure` step copies the project's tree into the kernel source before configuring and adds it to the kernel's list of device trees:

```text
do_configure:prepend:mpfs-disco-kit-amp() {
    dtsdir="${S}/arch/riscv/boot/dts/microchip"
    install -m 0644 ${WORKDIR}/mpfs-disco-kit-amp.dts ${dtsdir}/
    grep -q 'mpfs-disco-kit-amp.dtb' ${dtsdir}/Makefile || \
        echo 'dtb-$(CONFIG_ARCH_MICROCHIP_POLARFIRE) += mpfs-disco-kit-amp.dtb' >> ${dtsdir}/Makefile
}
```

The `grep -q ... ||` guard makes the step safe to run twice.

### 4. The supervisor firmware

The vendor's example recipe, `polarfire-soc-freertos-amp-demo`, builds from the vendor's source repository, which has no Discovery Kit board and no supervisor. The project's `bbappend` leaves that recipe in place and adds a step after the patch step:

```text
do_pfdaq_overlay:mpfs-disco-kit-amp() {
    bash ${WORKDIR}/apply.sh ${S} --no-patch
}
addtask pfdaq_overlay after do_patch before do_configure
```

`apply.sh` is the same script a build outside Yocto uses. It adds the board files and the supervisor source from `firmware/u54_4-supervisor/` to the fetched vendor tree. So the firmware that runs on hart 4 is built from source in this repository, not from patches kept inside the layer. An earlier layout kept six large patches there; moving to source reduced the layer to files a reader can read.

### 5. The extras

`pfdaq-uio_1.0.bb` builds the optional kernel module with the standard `inherit module`, and the packagegroup `bbappend` adds the project's packages to the image: the RPMsg kernel modules, the FreeRTOS demo package, `pfdaq-tools` and the module.

## Run it

The steps the project ran:

1. Point the build at the layer: its path is listed in the build's `bblayers.conf`.
2. Build the image: `MACHINE=mpfs-disco-kit-amp bitbake mchp-base-image`.
3. Collect the results from the deploy directory: the compressed SD image and its block map, the kernel image `fitImage`, the device tree, `payload.bin` and the firmware ELF. The project keeps them with a checksum file.
4. Write the image to an SD card, or replace single files over SSH (decision D23), and boot.

Versions that go with these commands: meta-mchp `linux4microchip-2026.04`, OpenEmbedded-Core `yocto-5.0.15` (Scarthgap), Linux 6.18.17 (VERSIONS.md in the repository).

## What you should see

- The console of a boot from the resulting card shows the machine model the device tree gives (`Machine model: Microchip PolarFire-SoC Discovery Kit (AMP)`) and Linux attaching to the supervisor (module B01).
- An image written to a blank card and booted cold with no manual steps passed all 27 functional tests (`suite-fresh-card`).

## Do the same in your project

Use this when the vendor's BSP lacks the machine you need. Rename `meta-pfdaq`, `mpfs-disco-kit-amp` and `pfdaq` to your own names; keep the shape.

1. **Make the layer.** Copy `conf/layer.conf` and rename the collection. Keep `BBFILE_PRIORITY` (10 here) and set `LAYERDEPENDS` to the vendor BSP layer you build on and `LAYERSERIES_COMPAT` to your Yocto release (`scarthgap` here). Add the layer's path to `bblayers.conf`.
2. **Write the machine.** Start from the vendor machine closest to yours (this project's file says in its first lines which one it ports and that it is not a vendor machine). Set `KERNEL_DEVICETREE`, `UBOOT_MACHINE`, `HSS_PAYLOAD`, the `amp` machine feature, the `hss-early-boot-freertos` distro feature and `MACHINEOVERRIDES` so the vendor's files for your board are pulled in.
3. **Add the U-Boot files.** Create `recipes-bsp/u-boot/u-boot-mchp_%.bbappend` with its one `FILESEXTRAPATHS` line, then under `files/` the payload template (module B02), `envs/<machine>.txt` and the `.cfg` fragment that keeps the environment on the card's first partition. Do not skip the environment file: the first build here failed at the image step without it.
4. **Add the device tree.** Put your `.dts` under `recipes-kernel/linux/files/` (module B04) and copy the `do_configure:prepend` step from the kernel `bbappend`, changing the file name.
5. **Build the second program's firmware from your own source.** Copy the `do_pfdaq_overlay` step and the `addtask` line from the firmware `bbappend`, and point it at a script of yours that adds your files to the fetched vendor tree. Keep the source in your repository, not as patches inside the layer.
6. **List what goes in the image.** Add your tools and module to the packagegroup with a `bbappend`.
7. **Build.** Run `MACHINE=<your machine> bitbake mchp-base-image` and collect the SD image with its block map, `fitImage`, the device tree, `payload.bin` and the firmware ELF.
8. **Check it.** Boot from a card written from that image. The console must show your device tree's `model` string and Linux attaching to the second program (module B01); the project's own check is the full test suite from a freshly written card.

What not to change:

- **Do not edit the vendor layers.** All changes live in your layer, so a vendor update does not conflict with them.
- **Keep the `grep -q ... ||` guard** in the device-tree step so running it twice is safe.
- **Keep the `hss-early-boot-freertos` feature** if you want the payload to contain firmware: without it the vendor step fills the template with `null`.
- **Rebuild the firmware recipe after a source change.** `apply.sh` copies the source over the fetched tree.

## Evidence

- **Board:** a cold boot of the final image (`boot-uboot-linux`) and the full suite from a freshly written card (`suite-fresh-card`).
- **Build equivalence:** after the layer was reorganised, the rebuilt FreeRTOS firmware had the same checksum as the validated one, and the rebuilt Linux tools and device tree matched too. The kernel image and the kernel module were rebuilt but are not bit-identical, so a board run of those was still owed. This is recorded in the project's notes; no file of it is published.

## When it goes wrong

| Symptom | Cause |
|---|---|
| `bitbake` ends at the image step, "no uboot.env" | the machine has no environment file under `envs/` |
| The layer is ignored | its path is not in `bblayers.conf`, or `LAYERSERIES_COMPAT` does not name your Yocto release |
| The payload has no firmware in it | the machine lacks the `hss-early-boot-freertos` distro feature, so the vendor step fills the template with `null` |
| The device tree is not built | check that the `.dts` is in `SRC_URI` and that `KERNEL_DEVICETREE` names the `.dtb` |
| A firmware change has no effect | `apply.sh` copies `pfdaq_supervisor.c` over the fetched tree; the recipe must be rebuilt |

## Trade-offs and going further

- **A machine of your own is a promise to maintain it.** Each vendor layer update may change a file you copied (the environment, the package lists). The layer keeps its copies small and says in each header what they derive from.
- **Source in the repository, not patches.** A patch against a vendor tree breaks when the tree moves. A script that adds files and a small patch to a few vendor files fails less often and shows its input.
- **Exercise:** change `HSS_PAYLOAD` so the payload starts the bare-metal build instead (the vendor step supports `hss-early-boot-bm`). Which two conflicts does the vendor include guard against?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [B01 From power-on to login, stage by stage](B01-power-on-to-login.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [microSD card](../system/README.md#sd-card) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [Linux 6.18.17](../system/README.md#linux-kernel) · [U-Boot](../system/README.md#u-boot) · [Hart Software Services](../system/README.md#hss) · [pfdaq supervisor](../system/README.md#supervisor) · [UIO devices](../system/README.md#uio) · [pfdaq_uio kernel driver](../system/README.md#pfdaq-uio) · [pfdaq_cap](../system/README.md#pfdaq-cap) · [pfdaq_lat](../system/README.md#pfdaq-lat) |
| **Source** | [`yocto/meta-pfdaq/conf/layer.conf`](../../yocto/meta-pfdaq/conf/layer.conf): `LAYERDEPENDS_meta-pfdaq`, `LAYERSERIES_COMPAT_meta-pfdaq`<br>[`yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf`](../../yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf): `HSS_PAYLOAD`, `KERNEL_DEVICETREE`, `UBOOT_MACHINE`, `MACHINE_FEATURES`<br>[`yocto/meta-pfdaq/recipes-bsp/u-boot/u-boot-mchp_%.bbappend`](../../yocto/meta-pfdaq/recipes-bsp/u-boot/u-boot-mchp_%.bbappend)<br>[`yocto/meta-pfdaq/recipes-kernel/linux/linux-mchp_6.%.bbappend`](../../yocto/meta-pfdaq/recipes-kernel/linux/linux-mchp_6.%.bbappend): `do_configure`<br>[`yocto/meta-pfdaq/recipes-mchp/polarfire-soc-amp-examples/polarfire-soc-freertos-amp-demo.bbappend`](../../yocto/meta-pfdaq/recipes-mchp/polarfire-soc-amp-examples/polarfire-soc-freertos-amp-demo.bbappend): `do_pfdaq_overlay`<br>[`yocto/meta-pfdaq/recipes-kernel/pfdaq-uio/pfdaq-uio_1.0.bb`](../../yocto/meta-pfdaq/recipes-kernel/pfdaq-uio/pfdaq-uio_1.0.bb) |
| **Evidence** | [The same suite on a freshly written SD card, 27 of 27](../../evidence/v0.5.1/board/suite-fresh-card.md) (board, fabric v0.5.1, 2026-11-15)<br>[Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [B01 From power-on to login, stage by stage](B01-power-on-to-login.md)<br>[B02 The HSS payload: starting two operating systems](B02-hss-payload.md)<br>[B04 Device tree for co-design: reserved memory, UIO nodes, a disabled CPU](B04-device-tree-codesign.md)<br>[I03 A small kernel driver for a fabric interrupt](I03-kernel-driver-fabric-interrupt.md) |
| **Status** | validated |
<!-- nav:end -->
