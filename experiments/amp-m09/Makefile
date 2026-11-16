# Build the M09 Linux tool and the host test programs.
#   make host          x86 build (gcc): m09_ctl, fake_hart4_m09, test_pmp
#   make board         static RISC-V Linux build of m09_ctl for the board (SmartHLS toolchain, as experiments/amp-station)
#   make test          the host tests
HERE      := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
FW        := $(HERE)../../firmware/u54_4-supervisor
CFLAGS    ?= -O2 -Wall -Wextra -I$(FW) -std=gnu11
RVGCC     ?= /usr/local/microchip/Libero_SoC_2026.1/SmartHLS/SmartHLS/swtools/binutils/riscv-gnu-toolchain/bin/riscv64-unknown-linux-gnu-gcc
OUT       ?= $(HERE)build
HDRS      := $(FW)/pfdaq_pmp.h

host: $(OUT)/host/m09_ctl $(OUT)/host/fake_hart4_m09 $(OUT)/host/test_pmp
board: $(OUT)/board/m09_ctl

$(OUT)/host/%: %.c $(HDRS)
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -o $@ $<

$(OUT)/host/test_pmp: tests/test_pmp.c $(HDRS)
	@mkdir -p $(dir $@)
	gcc -O1 -g -Wall -Wextra -I$(FW) -std=gnu11 -o $@ $<

$(OUT)/board/m09_ctl: m09_ctl.c $(HDRS)
	@mkdir -p $(dir $@)
	$(RVGCC) $(CFLAGS) -static -o $@ $<

test: host
	$(HERE)tests/run_tests.sh $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: host board test clean
