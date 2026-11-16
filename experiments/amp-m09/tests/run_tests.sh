#!/bin/bash
# Host tests of M09, no board involved. About 30 s.
#   tests/run_tests.sh [build-dir]
# 1. pfdaq_pmp.h: encoding, the access check, the plan, the refusal rules, the whole sequence against a model of the
#    CSRs (clean state, the state predicted for the board, a leaking lock, failed install, aborted test), and 500 random
#    starting states: no lock bit is ever written and the state always comes back
# 2. the same under qemu-riscv64 (user mode: the logic only, no privilege modes)
# 3. the machine code of the firmware ELF (m09_user_run, m09_trap) in a small RV64 interpreter with a PMP
# 4. m09_ctl and run_m09.sh against a stand-in for hart 4, and deploy_payload.sh against a plain file
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$HERE/build}
FW=$HERE/../../firmware/u54_4-supervisor
ROOT=$(cd "$HERE/../.." && pwd)
fail=0
ok()   { echo "ok   $*"; }
bad()  { echo "FAIL $*"; fail=$((fail+1)); }
make -C "$HERE" host OUT="$OUT" >/dev/null || exit 1

echo "== 1 PMP logic and sequence, on the PC"
"$OUT/host/test_pmp" && ok "test_pmp" || bad "test_pmp"
gcc -O2 -fsanitize=address,undefined -Wall -Wextra -std=gnu11 -I"$FW" -o "$OUT/host/test_pmp_san" "$HERE/tests/test_pmp.c" &&
    "$OUT/host/test_pmp_san" >/dev/null && ok "test_pmp with address and undefined-behaviour sanitizers" || bad "test_pmp with sanitizers"

echo "== 2 the same logic cross-compiled, under qemu-riscv64"
if command -v qemu-riscv64 >/dev/null; then
    RV=${RVGCC:-/usr/local/microchip/Libero_SoC_2026.1/SmartHLS/SmartHLS/swtools/binutils/riscv-gnu-toolchain/bin/riscv64-unknown-linux-gnu-gcc}
    mkdir -p "$OUT/board"
    "$RV" -O2 -Wall -Wextra -std=gnu11 -I"$FW" -static -o "$OUT/board/test_pmp" "$HERE/tests/test_pmp.c" &&
        qemu-riscv64 "$OUT/board/test_pmp" && ok "test_pmp on RISC-V" || bad "test_pmp on RISC-V"
else
    echo "skip: qemu-riscv64 not installed"
fi

echo "== 3 the firmware's assembler in the interpreter"
if [ -f "$ROOT/build/amp-m09/polarfire-soc-freertos-amp-demo.elf" ]; then
    python3 -I "$HERE/tests/test_m09_asm.py" --plan-bin "$OUT/host/test_pmp" | tail -4
    python3 -I "$HERE/tests/test_m09_asm.py" --plan-bin "$OUT/host/test_pmp" | grep -q "^FAIL" && bad "assembler in the interpreter" || ok "assembler in the interpreter"
else
    echo "skip: build/amp-m09 not built (run build_firmware.sh)"
fi

echo "== 4 the scripts"
"$HERE/tests/test_run_m09.sh" | tail -2 && ok "run_m09.sh" || bad "run_m09.sh"
[ -f "$ROOT/build/amp-m09/payload.bin" ] && { "$HERE/tests/test_deploy.sh" >/dev/null 2>&1 && ok "deploy_payload.sh" || bad "deploy_payload.sh"; } || echo "skip: payload not built"
bash -n "$HERE/deploy_payload.sh" && bash -n "$HERE/build_firmware.sh" && ok "bash -n of the scripts"
[ $fail = 0 ] && echo "ALL M09 HOST TESTS PASSED" || echo "$fail failure(s)"
exit $fail
