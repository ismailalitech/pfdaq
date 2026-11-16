#!/bin/bash
# Build the M09 demo variant of the hart 4 firmware (PFDAQ_AMP_M09) and wrap it in an HSS payload,
# without touching the proven images or the Yocto deploy directory. Same method as
# experiments/amp-station/build_firmware.sh.
#
#   experiments/amp-m09/build_firmware.sh [outdir]          (default: build/amp-m09)
#
#   1. Sets up the compiler the proven ELF was built with (the Yocto cross GCC 13.4, read-only from
#      yocto-dev, no bitbake) and a private copy of the vendor example tree under <outdir>/tree.
#   2. Builds the production supervisor (no demo switch) and checks that its loadable image is
#      byte-identical to images/pfdaq-v051/polarfire-soc-freertos-amp-demo.elf. Stops if it is not.
#   3. Builds the same tree with -DPFDAQ_AMP_M09: <outdir>/polarfire-soc-freertos-amp-demo.elf
#   4. Generates payload.bin with the project's amp.yaml.in (U-Boot on harts 1-3, the M09 ELF on hart 4
#      at 0x91C00000, same definition as the proven payload).
#   5. Offline checks: experiments/amp-station/verify_payload.py (structure against the proven payload)
#      and check_variant.py (the M09 ELF has the M09 code and none of the other variants').
#
# Environment: PFDAQ_UBOOT (u-boot.bin, default: Yocto deploy directory, read only),
#              PFDAQ_AMP_EXAMPLES (git URL or path used only when build/amp-examples does not exist)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/experiments/amp-m09
NAME=m09
MACRO=PFDAQ_AMP_M09
OUT=$(realpath -m "${1:-$ROOT/build/amp-$NAME}")
YB=$ROOT/yocto-dev/build-mpfs
YC=$YB/tmp-glibc/sysroots-components
GEN=$YC/x86_64/hss-payload-generator-native/usr/bin/hss-payload-generator
UBOOT=${PFDAQ_UBOOT:-$YB/tmp-glibc/deploy/images/mpfs-disco-kit-amp/u-boot.bin}
PROVEN=$ROOT/images/pfdaq-v051/polarfire-soc-freertos-amp-demo.elf
FW=$ROOT/firmware/u54_4-supervisor
TC=$OUT/toolchain
TREE=$OUT/tree

mkdir -p "$OUT" "$TC/bin"

# --- 1. toolchain: the Yocto cross compiler with an `as`/`ld` shim and a minimal sysroot --------
XB=$YC/x86_64/binutils-cross-riscv64/usr/bin/riscv64-mchp-linux
for t in as ld ld.bfd ar nm objcopy objdump ranlib strip readelf size; do ln -sf "$XB/riscv64-mchp-linux-$t" "$TC/bin/$t"; done
if [ ! -d "$TC/sysroot/usr/include" ]; then
    mkdir -p "$TC/sysroot"
    for c in glibc linux-libc-headers libgcc gcc-runtime; do cp -rs "$YC/riscv64/$c/." "$TC/sysroot/"; done
fi
export PATH=$TC/bin:$YC/x86_64/gcc-cross-riscv64/usr/bin/riscv64-mchp-linux:$XB:$YC/x86_64/makedepend-native/usr/bin:$PATH
CC=riscv64-mchp-linux-gcc
echo "compiler: $($CC --version | head -1)"

# --- 2. source tree ----------------------------------------------------------------------------
if [ ! -d "$TREE/mpfs-rpmsg-freertos" ]; then
    if [ -f "$ROOT/build/amp-examples/.pfdaq-applied" ]; then
        mkdir -p "$TREE"
        cp -a "$ROOT/build/amp-examples/." "$TREE/"
        rm -rf "$TREE"/mpfs-rpmsg-*/Remote-Default "$TREE"/mpfs-rpmsg-*/Master-Default
    else
        git clone -q --depth 1 --branch v2025.07 \
            "${PFDAQ_AMP_EXAMPLES:-https://github.com/polarfire-soc/polarfire-soc-amp-examples.git}" "$TREE"
        "$FW/apply.sh" "$TREE"
        touch "$TREE/.pfdaq-applied"
    fi
fi
for f in pfdaq_supervisor.c $(cd "$FW" && ls pfdaq_*.h); do
    cp "$FW/$f" "$TREE/mpfs-rpmsg-freertos/src/application/inc/"
done

build() {   # build <extra cflags> <log> -> prints ELF path
    ( cd "$TREE/mpfs-rpmsg-freertos"
      make clean BOARD=disco-kit >/dev/null 2>&1 || true
      make REMOTE=1 REMOTEPROC=1 CROSS_COMPILE=riscv64-mchp-linux- BOARD=disco-kit \
           EXT_CFLAGS="--sysroot=$TC/sysroot -DMPFS_HAL_FIRST_HART=4 -DMPFS_HAL_LAST_HART=4 $1" >"$2" 2>&1 )
    echo "$TREE/mpfs-rpmsg-freertos/Remote-Default/mpfs-rpmsg-remote.elf"
}
loadable() { riscv64-mchp-linux-objcopy -O binary "$1" "$2"; sha256sum "$2" | cut -d' ' -f1; }

# --- 3. baseline: the production supervisor must still come out identical ----------------------
BASE=$(build "" "$OUT/build-baseline.log")
cp "$BASE" "$OUT/baseline-no-demo.elf"
B_SHA=$(loadable "$OUT/baseline-no-demo.elf" "$OUT/baseline-no-demo.bin")
P_SHA=$(loadable "$PROVEN" "$OUT/proven.bin")
if [ "$B_SHA" != "$P_SHA" ]; then
    echo "FAIL: production build differs from the proven ELF (loadable image $B_SHA vs $P_SHA)" >&2; exit 1
fi
echo "baseline: loadable image of the build without a demo switch equals the proven ELF ($P_SHA)"

# --- 4. variant ELF ----------------------------------------------------------------------------
DEMO=$(build "-D$MACRO" "$OUT/build-$NAME.log")
cp "$DEMO" "$OUT/polarfire-soc-freertos-amp-demo.elf"
riscv64-mchp-linux-size "$OUT/polarfire-soc-freertos-amp-demo.elf"

# --- 5. payload --------------------------------------------------------------------------------
cp -L "$UBOOT" "$OUT/u-boot.bin"
sed -e "s/@@AMP_DEMO@@/freertos/g" -e "s/@@AMP_PAYLOAD@@/polarfire-soc-freertos-amp-demo.elf/g" \
    -e 's/@@AMP_SKIP-AUTOBOOT@@/false/g' \
    "$ROOT/yocto/meta-pfdaq/recipes-bsp/u-boot/files/mpfs-disco-kit-amp/amp.yaml.in" > "$OUT/amp.yaml"
( cd "$OUT" && "$GEN" -c amp.yaml -v payload.bin >gen.log 2>&1 )
( cd "$OUT" && sha256sum polarfire-soc-freertos-amp-demo.elf u-boot.bin amp.yaml payload.bin > SHA256SUMS )
echo "payload: $OUT/payload.bin $(stat -c %s "$OUT/payload.bin") bytes"

# --- 6. offline verification ------------------------------------------------------------------
python3 "$ROOT/experiments/amp-station/verify_payload.py" --demo-payload "$OUT/payload.bin" --demo-elf "$OUT/polarfire-soc-freertos-amp-demo.elf" \
    --proven-payload "$ROOT/images/pfdaq-v051/payload.bin" --proven-elf "$PROVEN" --uboot "$OUT/u-boot.bin"
python3 "$HERE/check_variant.py" --variant "$NAME" --elf "$OUT/polarfire-soc-freertos-amp-demo.elf" --production "$OUT/baseline-no-demo.elf" \
    --objdump "$TC/bin/objdump"
