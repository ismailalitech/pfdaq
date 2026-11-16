# Build the M08 Linux program.
#   make host          x86 build (gcc), runs against the simulated hart 4 (--sim) for the PC tests
#   make board         static RISC-V Linux build for the board (SmartHLS toolchain, as experiments/amp-station)
#   make test          the host tests
HERE      := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
FW        := $(HERE)../../firmware/u54_4-supervisor
CFLAGS    ?= -O2 -Wall -Wextra -I$(FW) -std=gnu11
RVGCC     ?= /usr/local/microchip/Libero_SoC_2026.1/SmartHLS/SmartHLS/swtools/binutils/riscv-gnu-toolchain/bin/riscv64-unknown-linux-gnu-gcc
OUT       ?= $(HERE)build

host: $(OUT)/host/m08_rtt
board: $(OUT)/board/m08_rtt

$(OUT)/host/m08_rtt: m08_rtt.c $(FW)/pfdaq_m08.h
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -pthread -o $@ $<

$(OUT)/board/m08_rtt: m08_rtt.c $(FW)/pfdaq_m08.h
	@mkdir -p $(dir $@)
	$(RVGCC) $(CFLAGS) -static -pthread -o $@ $<

test: host
	$(HERE)tests/run_tests.sh $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: host board test clean
