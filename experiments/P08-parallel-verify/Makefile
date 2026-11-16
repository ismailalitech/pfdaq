# Build pfdaq_cap_mt for the board. Any RISC-V 64-bit Linux GCC works; the program is linked
# statically so it does not depend on the C library version of the board image.
#
#   make CROSS_COMPILE=riscv64-linux-gnu-
CROSS_COMPILE ?= riscv64-linux-gnu-
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -static
LDLIBS  ?= -lpthread

pfdaq_cap_mt: pfdaq_cap_mt.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f pfdaq_cap_mt

.PHONY: clean
