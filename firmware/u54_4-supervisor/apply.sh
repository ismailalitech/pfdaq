#!/bin/bash
# Turn a pristine checkout of Microchip's polarfire-soc-amp-examples (tag v2025.07) into the
# pfdaq U54_4 firmware tree: adds a Discovery Kit board to both the FreeRTOS and the bare-metal
# RPMsg projects and installs the pfdaq supervisor in place of the vendor demo menu.
#
#   firmware/u54_4-supervisor/apply.sh <amp-examples checkout> [--no-patch]
#
# --no-patch skips the patch to the vendor's own files (the Yocto recipe applies it itself).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DST=$(realpath "${1:?path to a polarfire-soc-amp-examples checkout}")
PY=${PYTHON:-python3}

if [ "${2:-}" != "--no-patch" ]; then
    ( cd "$DST" && patch -p1 --no-backup-if-mismatch < "$HERE/amp-examples-v2025.07.patch" )
fi
for v in freertos bm; do
    P=$DST/mpfs-rpmsg-$v/src
    B=$P/boards/disco-kit
    rm -rf "$B"
    mkdir -p "$B/fpga_design/design_description"
    # linker scripts and platform configuration are the vendor's Icicle Kit ones, unchanged
    cp -r "$P/boards/icicle-kit-es/platform_config" "$B/"
    cp "$HERE/disco-kit/Makefile.$v" "$B/Makefile"
    cp "$HERE/disco-kit/MPFS_DISCOVERY_KIT_MSS_mss_cfg.xml" "$B/fpga_design/design_description/"
    # clock, DDR, I/O and memory-map headers come from the MSS configuration of the fabric design
    ( cd "$DST/mpfs-rpmsg-$v" && "$PY" src/platform/soc_config_generator/mpfs_configuration_generator.py \
        src/boards/disco-kit/fpga_design/design_description/MPFS_DISCOVERY_KIT_MSS_mss_cfg.xml src/boards/disco-kit >/dev/null )
    cp "$HERE/pfdaq_supervisor.c" "$P/application/inc/"
done
echo "pfdaq firmware overlay applied to $DST"
