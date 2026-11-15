#!/bin/bash
# Build the pfdaq fabric: Microchip Discovery Kit reference design v2026.04 + the project overlay.
#
#   hw/libero/build.sh <design_version 1..65535> [job_name]
#
# Environment:
#   LIBERO            path to the libero launcher          (default: Libero SoC 2026.1 install)
#   LM_LICENSE_FILE   Libero licence                       (default: 1702@localhost)
#   PFDAQ_REFDESIGN   pristine reference design checkout   (default: build/disco-ref, cloned if absent)
#
# The script stops the build early in the two situations that once produced a bitstream that
# met timing but was wrong: a constraint file error, and I/Os left unplaced because the pin
# constraints were not passed to place and route.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VER=${1:?design version (integer) required}
JOB=${2:-PFDAQ_dv$VER}
LIBERO=${LIBERO:-/usr/local/microchip/Libero_SoC_2026.1/Libero_SoC/Designer/bin/libero}
export LM_LICENSE_FILE=${LM_LICENSE_FILE:-1702@localhost}
export SNPSLMD_LICENSE_FILE=${SNPSLMD_LICENSE_FILE:-$LM_LICENSE_FILE}
REF=${PFDAQ_REFDESIGN:-$ROOT/build/disco-ref}
OUT=$ROOT/build/libero
WORK=$OUT/work
TAG=v2026.04

mkdir -p "$OUT"
if [ ! -d "$REF" ]; then
    git clone --depth 1 --branch $TAG \
        https://github.com/polarfire-soc/polarfire-soc-discovery-kit-reference-design.git "$REF"
fi
[ -d "$WORK" ] && mv "$WORK" "$OUT/work.old.$(date +%s)"
mkdir -p "$WORK"
( cd "$REF" && tar cf - --exclude=.git . ) | ( cd "$WORK" && tar xf - )

# overlay: project RTL, integration script, constraints, and the changes to the vendor build script
OV=$ROOT/hw/libero/overlay
( cd "$WORK" && patch -p1 --no-backup-if-mismatch < "$OV/reference-design-v2026.04.patch" )
mkdir -p "$WORK/script_support/additional_configurations/PFDAQ" "$WORK/script_support/hdl/pfdaq"
cp "$OV/PFDAQ.tcl" "$WORK/script_support/additional_configurations/PFDAQ/"
cp "$OV/pfdaq_cdc.sdc" "$WORK/script_support/constraints/"
cp "$ROOT"/hw/rtl/*.v "$WORK/script_support/hdl/pfdaq/"
cp "$ROOT/hw/libero/export_job.tcl" "$ROOT/hw/libero/sta_without_cdc.tcl" "$WORK/"

LOG=$OUT/build-dv$VER.log
cd "$WORK"
"$LIBERO" script:MPFS_DISCOVERY_KIT_REFERENCE_DESIGN.tcl \
    SCRIPT_ARGS:AXI4_STREAM_DEMO+VERIFY_TIMING+DESIGN_VERSION:$VER logfile:"$OUT/libero-dv$VER.log" > "$LOG" 2>&1 &
PID=$!
while kill -0 $PID 2>/dev/null; do
    if grep -q 'Error: SDC00' "$LOG"; then
        echo "ABORT: constraint error"; grep -m2 'Error: SDC00' "$LOG"; pkill -P $PID || true; kill $PID || true; exit 1
    fi
    io=$(grep -a -m1 'I/O Bank and Globals Assigner identified [0-9]* fixed' "$LOG" || true)
    if [ -n "$io" ] && ! echo "$io" | grep -q ' 0 unfixed'; then
        echo "ABORT: unplaced I/Os: $io"; pkill -P $PID || true; kill $PID || true; exit 1
    fi
    sleep 15
done
wait $PID || { echo "Libero failed, see $LOG"; exit 1; }

V="$WORK/PFDAQ/designer/MPFS_DISCOVERY_KIT/MPFS_DISCOVERY_KIT_has_violations"
grep -a -m1 'I/O Bank and Globals Assigner identified' "$LOG"
grep multi_corner "$V"
if grep multi_corner "$V" | grep -q violated; then echo "ABORT: timing violated"; exit 1; fi
grep -a 'Logic Element' "$WORK/PFDAQ/designer/MPFS_DISCOVERY_KIT/MPFS_DISCOVERY_KIT_layout_log.log" | tail -1

PFDAQ_JOB_NAME=$JOB PFDAQ_JOB_DIR=$OUT "$LIBERO" script:export_job.tcl > "$OUT/export-dv$VER.log" 2>&1
grep -a 'Fabric component bitstream digest' "$OUT/export-dv$VER.log"
ls -la "$OUT/$JOB.job"
