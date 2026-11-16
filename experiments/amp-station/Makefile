# Build the station programs.
#   make host          x86 build (gcc) for the tests: station, fake_hart4
#   make board         static RISC-V Linux build for the board (SmartHLS toolchain, as I04-dma-one-burst)
#   make test          unit tests and the end-to-end run against the stand-in for hart 4
HERE      := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
FW        := $(HERE)../../firmware/u54_4-supervisor
CFLAGS    ?= -O2 -Wall -Wextra -I$(HERE) -I$(FW) -std=gnu11
RVGCC     ?= /usr/local/microchip/Libero_SoC_2026.1/SmartHLS/SmartHLS/swtools/binutils/riscv-gnu-toolchain/bin/riscv64-unknown-linux-gnu-gcc
HDRS      := station_common.h station_link.h station_fabric.h $(FW)/pfdaq_station.h
OUT       ?= $(HERE)build

host: $(OUT)/host/station $(OUT)/host/fake_hart4
board: $(OUT)/board/station

$(OUT)/host/%: %.c $(HDRS)
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -pthread -o $@ $<

$(OUT)/board/station: station.c $(HDRS)
	@mkdir -p $(dir $@)
	$(RVGCC) $(CFLAGS) -static -o $@ $<

test: host
	$(HERE)tests/run_tests.sh $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: host board test clean
