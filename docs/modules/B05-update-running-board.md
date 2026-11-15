---
id: B05
title: "Updating a running board over SSH"
track: boot
level: 2
summary: Replacing the kernel, the payload and the tools without removing the SD card.
needs: [B02]
repro: base
status: validated
hardware: [sd-card, ethernet, host]
software: [hss, u-boot, linux-kernel, remoteproc-rpmsg]
sources:
  - tests/host/amp_reboot_test.sh:remoteproc0,pfdaq-supervisor
  - firmware/build_payload.sh:payload.bin
  - yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in
evidence: [reboot-30, suite-fresh-card, boot-uboot-linux, boot-hss]
related: [B02, B03, B06, K12, P06]
---

# Updating a running board over SSH

After this module you can put a new kernel image, a new boot payload (U-Boot and the real-time firmware) or new tools onto a running board without taking the SD card out, check that each file arrived intact, and know the one way this goes wrong that cannot be fixed from the board itself.

The commands on this page are the ones this project ran many times during bring-up, with the failure handling tightened (each step now stops if the one before it failed). The board was not available when the page was written, so no output below is new: it is quoted from the records listed under Evidence.

## Before you start

- **Level 2.** You should know what the boot monitor loads and from where (module B02).
- **Board:** the pfdaq baseline, reachable over SSH as `root`. In the commands, `<board>` is its address and `<tag>` the folder holding the files you are installing.
- **PC:** the files to install, built by Yocto or by `firmware/build_payload.sh`.
- **Rule of this page:** every command that writes to a block device is preceded by a command that shows which device it is. Read that output before you press Enter.

## The idea

A running Linux can rewrite the same SD card it booted from, as long as the pieces are separate and each one is checked afterwards. What an update needs depends on where the thing lives.

| What changed | Where it lives on the card | How it is updated | Needs a reboot |
|---|---|---|---|
| The Linux tools (`pfdaq_cap`, `pfdaq_lat`) | the root filesystem, `/usr/bin` | copy, then `install -m 755` | no |
| The optional kernel module (`pfdaq_uio.ko`) | root filesystem, `/lib/modules/<kernel>/updates` | copy, `install`, `depmod -a` | no |
| The kernel image and device tree (`fitImage`) | partition 1, a FAT filesystem | mount, copy, `sync`, unmount | yes |
| The boot payload (U-Boot and the hart 4 firmware) | partition 2, raw, no filesystem | `dd` the file onto the partition | yes |
| The fabric | not on the card | JTAG only, see module B06 | no |

The record of a cold boot shows why these are separate. The boot monitor finds its boot partition on the SD card ("Boot Partition found at index 1", the second partition) and reads the payload from it. U-Boot, started from that payload, then loads the kernel from the FAT partition ("Loading kernel (any) from FIT Image at 8e000000"), and Linux mounts its root from the third partition (`root=/dev/mmcblk0p3` on its command line).

## On this board

The whole card was written once from an image file with `bmaptool`, and every later change went in over SSH. Two copies matter:

- the **proven image**, the full SD image that was written to a freshly written card and passed the whole board suite (record `suite-fresh-card`), kept on the PC;
- on the board, before each overwrite, a copy of what was there (`fitImage` kept as a second file on the FAT partition, the old payload saved as a file with `dd`).

The second copy only helps while Linux still boots. The first is the one that always works.

## Run it

All commands run on the PC, from the folder holding the new files. The board address is a placeholder.

### 1. Tools and module (no reboot)

```sh
scp pfdaq_cap pfdaq_lat <board>:/root/
ssh root@<board> 'install -m 755 /root/pfdaq_cap /root/pfdaq_lat /usr/bin/ && rm /root/pfdaq_cap /root/pfdaq_lat && sync'
```

For the kernel module, the module goes into a folder named after the running kernel, and `depmod` makes it findable:

```sh
K=$(ssh root@<board> uname -r)
scp pfdaq_uio.ko <board>:/root/
ssh root@<board> "mkdir -p /lib/modules/$K/updates && install -m 644 /root/pfdaq_uio.ko /lib/modules/$K/updates/ && depmod -a && modinfo pfdaq_uio | head -3"
```

### 2. Kernel image and device tree (reboot)

```sh
scp fitImage <board>:/root/
ssh root@<board> 'mkdir -p /mnt/boot && mount /dev/mmcblk0p1 /mnt/boot \
  && { [ -f /mnt/boot/fitImage.old ] || cp /mnt/boot/fitImage /mnt/boot/fitImage.old; } \
  && cp /root/fitImage /mnt/boot/fitImage && sync && sha256sum /mnt/boot/fitImage | cut -c1-16 && umount /mnt/boot'
sha256sum fitImage | cut -c1-16        # on the PC: the two 16-character prefixes must be equal
```

The braces `{ [ -f … ] || cp …; }` save the previous image once and never overwrite that saved copy with a later, possibly bad one. If the mount fails, nothing after it runs: without the `&&` chain, the copy would land in an empty folder on the root filesystem and look as if it had worked. The name `fitImage.old` is a choice; the project used its own names for the saved copies.

### 3. Boot payload (reboot)

First look at what you are about to overwrite:

```sh
ssh root@<board> 'lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT /dev/mmcblk0'
```

Partition 2 should be the raw payload partition: not mounted, and not the root filesystem. If the listing does not look like the three-partition layout above, stop here. Then:

```sh
scp payload.bin <board>:/root/
ssh root@<board> '{ [ -f /root/payload-old.img ] || dd if=/dev/mmcblk0p2 of=/root/payload-old.img bs=1M; } \
  && dd if=/root/payload.bin of=/dev/mmcblk0p2 bs=1M conv=fsync \
  && head -c $(stat -c %s /root/payload.bin) /dev/mmcblk0p2 | sha256sum | cut -c1-16 \
  && rm /root/payload.bin && sync'
sha256sum payload.bin | cut -c1-16     # equal to the line above, or do not reboot
```

The read-back reads exactly as many bytes as the file has, because the partition can be larger than the payload and the bytes after it are not part of what was written. Only reboot when the two prefixes match.

### 4. Reboot and check

```sh
ssh root@<board> reboot
```

Then wait for SSH to return, and check that both systems came up. The project's reboot test does this per cycle (`tests/host/amp_reboot_test.sh`): it waits for SSH, reads the remoteproc state, looks for the supervisor's message channel and runs a supervised capture.

## What you should see

After a reboot with a good payload and kernel, the Linux console shows the real-time context being found and attached, and the message channel appearing (these lines are from the record of the final image's cold boot):

```text
[    1.709490] remoteproc remoteproc0: remote-context is available
[    1.715628] remoteproc remoteproc0: attaching to remote-context
[    1.738642] virtio_rpmsg_bus virtio0: rpmsg host is online
[    1.751180] remoteproc remoteproc0: remote processor remote-context is now attached
[    1.938361] virtio_rpmsg_bus virtio0: creating channel pfdaq-supervisor addr 0x0
```

In the project's 30-cycle warm-reboot test, run on the final payload, every cycle ended with remoteproc attached, the supervisor channel present and the supervised capture passing, and SSH was reachable between 45.9 and 61.4 seconds after the `reboot` command.

## Evidence

- **Reboot test, fabric v0.5.1:** 30 of 30 cycles, supervisor channel present and full pass in each (record `reboot-30`).
- **From a clean card:** the full image, written with `bmaptool`, passed the entire 27-test suite with no manual step beyond the root password and SSH key (record `suite-fresh-card`). That is why the SSH route is trusted: it was always followed by a comparison against this image.
- **Boot records:** the boot monitor and Linux console lines quoted above are in `boot-hss` and `boot-uboot-linux`.
- **What is not shown:** the payload-writing commands themselves are not in a published record. The command lines above are the ones the project's notes and shell history show; the board's state afterwards was checked by checksum, not by a separate log.

## When it goes wrong

| Symptom | Likely cause and what to do |
|---|---|
| The two checksum prefixes differ | The copy or the write was incomplete. Do not reboot. Repeat the copy; if it differs again, suspect the card. |
| After a reboot the board does not answer | A bad kernel image or payload. If you saved the old files and the board still boots into U-Boot, restore from them; otherwise take the card out and write the proven image from the PC. |
| The hart 4 firmware printed nothing and no channel appeared, although the boot monitor said the hart was running | Seen once, on the first boot after a payload was written from the running system, and never again in the boots since. The cause is unknown and the notes do not say whether the data had been flushed. See case study K12. The page's steps include `conv=fsync`, `sync` and a read-back for that reason; whether they matter here has not been shown. |
| `dd` reports an error or the listing in step 3 is not what you expect | Stop. A wrong target device is the one mistake that this method cannot undo. |

## Trade-offs and going further

- **Speed against safety.** An SSH update takes seconds and avoids a card swap. It also trusts the running system to write its own boot media correctly. For that reason the project re-proved every milestone from a freshly written card: the SSH route is for iterating, the card image is for deciding that something works.
- **Where the rollback lives.** Copies kept on the same card help only while the card still boots. A copy of the proven image on the PC is the only rollback that does not depend on the thing being updated.
- **What this method cannot do.** It cannot change the fabric (JTAG only) and must never touch the chip's boot flash (module B06).
- **Exercise:** add a step that records `sha256sum` of the three installed files into one line on the board, so a later session can ask "what is on this card?" without a PC.
- **Exercise:** which of the four steps would you move to a script, and which would you keep by hand because the device check needs a person?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [B02 The HSS payload: starting two operating systems](B02-hss-payload.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [microSD card](../system/README.md#sd-card) · [Gigabit Ethernet](../system/README.md#ethernet) · [Development PC](../system/README.md#host) · [Hart Software Services](../system/README.md#hss) · [U-Boot](../system/README.md#u-boot) · [Linux 6.18.17](../system/README.md#linux-kernel) · [remoteproc and RPMsg](../system/README.md#remoteproc-rpmsg) |
| **Source** | [`tests/host/amp_reboot_test.sh`](../../tests/host/amp_reboot_test.sh): `remoteproc0`, `pfdaq-supervisor`<br>[`firmware/build_payload.sh`](../../firmware/build_payload.sh): `payload.bin`<br>[`yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in`](../../yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in) |
| **Evidence** | [Thirty warm reboots, both systems up and a supervised capture passing each time](../../evidence/v0.5.1/board/reboot-30.csv) (board, fabric v0.5.1, 2026-11-15)<br>[The same suite on a freshly written SD card, 27 of 27](../../evidence/v0.5.1/board/suite-fresh-card.md) (board, fabric v0.5.1, 2026-11-15)<br>[Key lines of the U-Boot and Linux console from OpenSBI to login, including remoteproc attaching to hart 4](../../evidence/v0.5.1/board/boot-uboot-linux.txt) (board, fabric v0.5.1, 2026-11-15)<br>[Boot monitor console from power-up to all four application cores running](../../evidence/v0.5.1/board/boot-hss.txt) (board, fabric v0.5.1, 2026-11-15) |
| **Related** | [B02 The HSS payload: starting two operating systems](B02-hss-payload.md)<br>[B03 A Yocto layer for a board the vendor does not support in AMP](B03-yocto-layer.md)<br>[B06 Programming safely: components, digests and what never to touch](B06-programming-safely.md)<br>[K12 One boot in sixty, FreeRTOS never started](../case-studies/K12-freertos-no-start.md)<br>[P06 Soak and reboot testing](P06-soak-and-reboot.md) |
| **Status** | validated |
<!-- nav:end -->
