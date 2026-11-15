---
id: B02
title: "The HSS payload: starting two operating systems"
track: boot
level: 2
summary: One payload file that tells the monitor which image runs on which hart, in which privilege mode.
needs: [B01]
repro: base
status: validated
hardware: [hss, e51, u54-linux, u54-4, sd-card, ddr-u54-4-image]
software: [hss, u-boot, supervisor]
sources:
  - yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in:hart-entry-points,owner-hart,skip-opensbi,priv-mode
  - yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf:HSS_PAYLOAD
  - firmware/build_payload.sh
evidence: [boot-hss, suite-fresh-card, suite-baremetal]
related: [B01, B03, B04, M01, M04, K11, S03]
---

# The HSS payload: starting two operating systems

After this page you can read the payload description that makes one chip run Linux on three cores and a real-time supervisor on the fourth, change which image runs where, and check the result against what the boot monitor prints.

## Before you start

- **Level 2.** Module B01 describes the stages; this one zooms into the second.
- **Board:** the shared baseline (final fabric and image). The commands below are the ones the project ran; the board was not available while this page was written, so the outputs shown come from the recorded boot and from a payload file on the PC.

## The idea

The boot monitor does not know what software you want. It is handed a single file, the **payload**, which carries the images and a table saying: this image, this address, this core, this privilege mode. The payload is produced on the PC by a small tool, the HSS payload generator, from a short YAML description.

Two properties make the setup used here possible:

- **Each core can have its own image.** One image can serve several cores (U-Boot runs on three), another can belong to one core alone.
- **A core can be started without OpenSBI.** Linux cores start in supervisor mode and need OpenSBI underneath. A bare-metal or RTOS core can instead be started straight in machine mode.

## On this board

### The description

The project's template, `amp.yaml.in`, in full:

```yaml
set-name: 'PolarFire-SoC-HSS::Disco Linux + @@AMP_DEMO@@ AMP'

hart-entry-points: {u54_1: '0x80200000', u54_2: '0x80200000', u54_3: '0x80200000', u54_4: '0x91C00000'}

payloads:
  u-boot.bin: {exec-addr: '0x80200000', owner-hart: u54_1, secondary-hart: u54_2, secondary-hart: u54_3, priv-mode: prv_s}
  @@AMP_PAYLOAD@@: {exec-addr: '0x91C00000', owner-hart: u54_4, priv-mode: prv_m, skip-opensbi: true, skip-autoboot: @@AMP_SKIP-AUTOBOOT@@}
```

Reading it line by line:

| Key | Value here | Meaning |
|---|---|---|
| `set-name` | `Disco Linux + freertos AMP` | A label printed by the monitor at boot ("Boot image set name"). `@@AMP_DEMO@@` becomes `freertos` or `bm` |
| `hart-entry-points` | cores 1 to 3 at `0x80200000`, core 4 at `0x91C00000` | The address each core starts executing. The U-Boot address is where Linux-side software lives in DDR; the other is the reserved region for the second context (module B04) |
| `u-boot.bin` | `owner-hart: u54_1`, `secondary-hart` 2 and 3, `priv-mode: prv_s` | One image for three cores. Core 1 owns it; cores 2 and 3 join it. All start in **supervisor** mode, so OpenSBI sits underneath |
| the firmware ELF | `owner-hart: u54_4`, `priv-mode: prv_m` | The supervisor, alone on core 4, in **machine** mode |
| `skip-opensbi: true` | | Core 4 is started by a plain return into the image (the generator's README: "a simple mret rather than" through OpenSBI). The supervisor owns the whole machine-mode world on its core |
| `skip-autoboot: false` | | The monitor starts this image at boot. `true` would leave it for Linux to start through remoteproc |

The `@@...@@` words are placeholders. In the Yocto build, a small shell step from the vendor layer replaces them with `freertos`, the FreeRTOS ELF name and `false` when the machine has the feature `hss-early-boot-freertos`, and with the bare-metal equivalents when it has `hss-early-boot-bm`. The project's machine file turns the FreeRTOS one on (when the vendor feature `mchpcore` is set), and chooses this payload by name:

```text
DISTRO_FEATURES_BACKFILL:append = "${@bb.utils.contains('MCHP_FEATURES', 'mchpcore', ' hss-early-boot-freertos', '', d)}"
HSS_PAYLOAD = "amp"
```

The vendor layer then runs the generator on the filled-in file:

```text
hss-payload-generator -c amp.yaml -v payload.bin
```

### What the generator makes

`payload.bin` is a header, a table of chunks and the chunks themselves: every loadable piece of every image, with its destination, its owner core and its privilege mode. The generator's analyser (`-d`) prints it back. For the proven payload:

```text
magic:	            0xb007c0de
chunkTableOffset:   0x660
name[0]:            >>u-boot.bin<<
entryPoint[0]:      0x80200000
privMode[0]:        1 (PRV_S)
name[3]:            >>polarfire-soc-freertos-amp-demo.elf<<
entryPoint[3]:      0x91c00000
privMode[3]:        3 (PRV_M)
flags[3]:           40
	 SKIP_OPENSBI
set_name            >>PolarFire-SoC-HSS::Disco Linux + freertos AMP<<
bootImageLength:    1376656
 - 1 chunk found for owner 1
 - 5 chunks found for owner 4
Boot Chunks: total of 7 chunks found
 - 4 ZI chunks found for owner 4
```

Core 1's entry carries U-Boot (the cores that join it have entry points but no chunks of their own: they share it). Core 4's entry carries the supervisor in five chunks plus four zero-initialised ones (a "ZI chunk" is memory the monitor clears for the program's `.bss`). The numbers match the monitor's own log:

| In `payload.bin` | In the boot monitor's console (`boot-hss`) |
|---|---|
| `bootImageLength: 1376656` | `Copying 1376656 bytes to 0x103fc00000` |
| `chunkTableOffset: 0x660` (1632) | `Attempting to read image header (1632 bytes)` |
| `set_name` | `Boot image set name: "PolarFire-SoC-HSS::Disco Linux + freertos AMP"` |
| `SKIP_OPENSBI` on core 4 | `boot_service(u54_4):: BOOT_FLAG_SKIP_OPENSBI found` |
| `u-boot.bin` shared by cores 1, 2, 3 | `Registering domain "u-boot.bin" (hart mask 0xe)` |

## Run it

These are the steps the project ran; they are not rerun here.

1. **The normal way:** the payload is built as part of the image: `MACHINE=mpfs-disco-kit-amp bitbake mchp-base-image` (module B03). It is also deployed on its own as `payload.bin`, next to the kernel image.
2. **Without Yocto**, for the bare-metal alternative of the same supervisor: `firmware/build_payload.sh` builds the firmware, fills in the same template with `bm` and runs the generator.
3. **Putting a new payload on a running board** needs no card swap: the file is written to the card's payload partition over SSH (decision D23), keeping the previous one as a copy. The board is rebooted to use it.
4. **Looking inside any payload**, on the PC: `hss-payload-generator -d payload.bin`.

## What you should see

On a good boot, the monitor's console shows the five rows of the table above, then one state line per core and per step, and `[Running]` for core 4 at 4.86 s, before Linux starts (module B01).

Swapping the supervisor for its bare-metal build is only a different payload on partition 2: the same suite then runs against the bare-metal firmware (8 of 8 tests, `suite-baremetal`), and the final image from a freshly written card passes 27 of 27 (`suite-fresh-card`).

## Evidence

- **Board:** the boot monitor's console from a cold boot of the final image (`boot-hss`); the two suites above.
- **Host:** the `-d` dump above was produced on the PC from the final payload file. It is not stored as a separate record.

## When it goes wrong

| Symptom | Cause |
|---|---|
| The monitor does not print "BOOT_FLAG_SKIP_OPENSBI found" | the core 4 entry lost `skip-opensbi`, so the core would be started through OpenSBI, which the firmware does not expect (reasoned from the flag, not tried) |
| U-Boot starts on a core you did not intend | `owner-hart` or `secondary-hart` lines changed |
| The supervisor does not start and the monitor reports nothing | check that the ELF is linked for the address in `exec-addr` and that Linux's device tree reserves that region (module B04) |
| `remoteproc` cannot stop and start the supervisor again | the monitor rejects a restart request from Linux (case study K11); started at boot, it cannot be restarted from Linux |

## Trade-offs and going further

- **Starting the second context at boot** (`skip-autoboot: false`) gives supervision before Linux and a simple Linux driver (it attaches). Starting it from Linux (`skip-autoboot: true`) would let Linux load firmware at will but needs the restart path that this monitor rejects.
- **Two images, one file:** the update unit is the whole payload. A change to the supervisor means a new payload even though U-Boot did not change.
- **No signature is required.** The unsigned payloads used throughout this project boot, so the monitor in this chip does not insist on one. Whether it would check a signature if one were present is not known. Module S03 signs this very payload offline and shows how a monitor built with a key would check it.
- **Exercise:** make the template start the supervisor on core 3 instead of core 4, and name every other place that must change (device tree, firmware load address, console).

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [B01 From power-on to login, stage by stage](B01-power-on-to-login.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [Hart Software Services](../system/README.md#hss) · [E51 (hart 0)](../system/README.md#e51) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [U54 hart 4](../system/README.md#u54-4) · [microSD card](../system/README.md#sd-card) · [Hart 4 image](../system/README.md#ddr-u54-4-image) · [Hart Software Services](../system/README.md#hss) · [U-Boot](../system/README.md#u-boot) · [pfdaq supervisor](../system/README.md#supervisor) |
| **Source** | [`yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in`](../../yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in): `hart-entry-points`, `owner-hart`, `skip-opensbi`, `priv-mode`<br>[`yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf`](../../yocto/meta-pfdaq/conf/machine/mpfs-disco-kit-amp.conf): `HSS_PAYLOAD`<br>[`firmware/build_payload.sh`](../../firmware/build_payload.sh) |
| **Evidence** | [Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15)<br>[The same suite on a freshly written SD card, 27 of 27](../../evidence/v0.5.1/board/suite-fresh-card.md) (board, fabric v0.5.1, 2026-11-15)<br>[Board suite with the bare-metal supervisor on hart 4, 8 of 8](../../evidence/v0.5.1/board/suite-baremetal.md) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [B01 From power-on to login, stage by stage](B01-power-on-to-login.md)<br>[B03 A Yocto layer for a board the vendor does not support in AMP](B03-yocto-layer.md)<br>[B04 Device tree for co-design: reserved memory, UIO nodes, a disabled CPU](B04-device-tree-codesign.md)<br>[M01 Asymmetric multiprocessing on one chip](M01-asymmetric-multiprocessing.md)<br>[M04 Same core, same code: FreeRTOS against bare metal](M04-freertos-vs-bare-metal.md)<br>[K11 remoteproc cannot restart the hart](../case-studies/K11-remoteproc-restart.md)<br>[S03 A signed boot chain you can run](S03-signed-boot-chain.md) |
| **Status** | validated |
<!-- nav:end -->
