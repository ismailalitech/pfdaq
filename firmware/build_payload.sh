#!/bin/bash
# Build the U54_4 firmware outside Yocto and wrap it, with U-Boot, in an HSS payload.
#
#   firmware/build_payload.sh [outdir] [bm|freertos]        (default: build/payload-bm, bm)
#
# The SD image always carries the FreeRTOS firmware built by Yocto. This script exists for the
# bare-metal comparison (same supervisor, no RTOS) and for quick firmware test builds: write the
# resulting payload.bin to SD partition 2 and reboot; write the image's own payload.bin back
# afterwards.
#
# Environment:
#   PFDAQ_AMP_EXAMPLES  pristine polarfire-soc-amp-examples v2025.07 checkout to copy from
#                       (default: cloned from GitHub into build/)
#   RISCV_TOOLCHAIN     bin directory of a riscv64-unknown-elf GCC (default: SoftConsole v2022.2)
#   PFDAQ_YOCTO_BUILD   Yocto build directory, for u-boot.bin and hss-payload-generator
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${1:-$ROOT/build/payload-bm}")
VAR=${2:-bm}
TC=${RISCV_TOOLCHAIN:-$HOME/Microchip/SoftConsole-v2022.2-RISC-V-747/riscv-unknown-elf-gcc/bin}
YB=${PFDAQ_YOCTO_BUILD:-$ROOT/yocto-dev/build-mpfs}
GEN=$YB/tmp-glibc/sysroots-components/x86_64/hss-payload-generator-native/usr/bin/hss-payload-generator
UBOOT=$YB/tmp-glibc/deploy/images/mpfs-disco-kit-amp/u-boot.bin
TREE=$ROOT/build/amp-examples
TAG=v2025.07

if [ ! -f "$TREE/.pfdaq-applied" ]; then
    rm -rf "$TREE"; mkdir -p "$ROOT/build"
    git clone -q --depth 1 --branch $TAG \
        "${PFDAQ_AMP_EXAMPLES:-https://github.com/polarfire-soc/polarfire-soc-amp-examples.git}" "$TREE"
    "$ROOT/firmware/u54_4-supervisor/apply.sh" "$TREE"
    touch "$TREE/.pfdaq-applied"
else
    # pick up edits to the supervisor without re-cloning
    for v in freertos bm; do
        cp "$ROOT/firmware/u54_4-supervisor/pfdaq_supervisor.c" "$TREE/mpfs-rpmsg-$v/src/application/inc/"
    done
fi
SRC=$TREE/mpfs-rpmsg-$VAR
mkdir -p "$OUT"
( cd "$SRC" && PATH=$TC:$PATH make clean BOARD=disco-kit >/dev/null 2>&1 || true
  PATH=$TC:$PATH make BOARD=disco-kit REMOTE=1 REMOTEPROC=1 CROSS_COMPILE=riscv64-unknown-elf- \
      EXT_CFLAGS='-DMPFS_HAL_FIRST_HART=4 -DMPFS_HAL_LAST_HART=4' >"$OUT/build.log" 2>&1 )
cp "$SRC/Remote-Default/mpfs-rpmsg-remote.elf" "$OUT/polarfire-soc-$VAR-amp-demo.elf"
cp -L "$UBOOT" "$OUT/u-boot.bin"
sed -e "s/@@AMP_DEMO@@/$VAR/g" -e "s/@@AMP_PAYLOAD@@/polarfire-soc-$VAR-amp-demo.elf/g" -e 's/@@AMP_SKIP-AUTOBOOT@@/false/g' \
    "$ROOT/yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in" > "$OUT/amp-$VAR.yaml"
( cd "$OUT" && "$GEN" -c "amp-$VAR.yaml" -v payload.bin >gen.log 2>&1 )
( cd "$OUT" && sha256sum "polarfire-soc-$VAR-amp-demo.elf" u-boot.bin "amp-$VAR.yaml" payload.bin > SHA256SUMS )
echo "$OUT/payload.bin $(stat -c %s "$OUT/payload.bin") bytes"
