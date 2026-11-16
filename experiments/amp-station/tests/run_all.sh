#!/bin/bash
# Every host test of the station, one after the other. About 3 minutes. No board involved.
HERE=$(cd "$(dirname "$0")" && pwd)
fail=0
run() { echo "######## $*"; "$@" || fail=$((fail+1)); }
make -C "$HERE/.." host >/dev/null || exit 1
run "$HERE/run_tests.sh"
run "$HERE/test_run_demo.sh"
run "$HERE/test_deploy.sh"
run python3 "$HERE/test_capture.py"
run python3 "$HERE/make_figures.py" --check
if command -v qemu-riscv64 >/dev/null; then
    RV=${RVGCC:-/usr/local/microchip/Libero_SoC_2026.1/SmartHLS/SmartHLS/swtools/binutils/riscv-gnu-toolchain/bin/riscv64-unknown-linux-gnu-gcc}
    mkdir -p "$HERE/../build/board"
    "$RV" -O2 -Wall -Wextra -I"$HERE/.." -I"$HERE/../../../firmware/u54_4-supervisor" -std=gnu11 -static -pthread \
        -o "$HERE/../build/board/test_units" "$HERE/test_units.c" && run qemu-riscv64 "$HERE/../build/board/test_units"
fi
[ $fail = 0 ] && echo "ALL HOST TESTS PASSED" || echo "$fail test group(s) failed"
exit $fail
