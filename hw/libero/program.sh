#!/bin/bash
# Program a pfdaq fabric job (FABRIC + SNVM) with FlashPro Express, verify it and read the device
# back. Shut Linux down on the board first. The on-board FlashPro5 JTAG interface (USB interface 0)
# must not be bound to the ftdi_sio serial driver.
#   hw/libero/program.sh <file.job> [action: PROGRAM|VERIFY|DEVICE_INFO, default all three]
set -euo pipefail
JOBFILE=$(realpath "${1:?job file required}")
FPX=${FPEXPRESS:-/usr/local/microchip/Libero_SoC_2026.1/Libero_SoC/Designer/bin/FPExpress}
DIR=$(dirname "$JOBFILE")/fpexpress
mkdir -p "$DIR"
ACTIONS=${2:-"PROGRAM VERIFY DEVICE_INFO"}
T=$DIR/run.tcl
echo "create_job_project -job_project_location {$DIR} -job_file {$JOBFILE} -overwrite 1" > "$T"
for a in $ACTIONS; do
    echo "set_programming_action -name {MPFS095T} -action {$a}" >> "$T"
    echo "run_selected_actions" >> "$T"
done
echo "close_project" >> "$T"
"$FPX" script:"$T" logfile:"$DIR/program.log" > "$DIR/program.stdout" 2>&1
grep -E 'action .* (PASSED|FAILED)|Error|CHECKSUM\[|ENVM digest|DESIGNVER|Design Name' "$DIR/program.stdout" | sed 's/.*device .MPFS095T. : //' | sort -u
