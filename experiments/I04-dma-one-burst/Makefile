# Build dma_one_burst for the board. Any RISC-V 64-bit Linux GCC works; the program is linked
# statically so it does not depend on the C library version of the board image.
#
#   make CROSS_COMPILE=riscv64-linux-gnu-
#   make CROSS_COMPILE=... LDFLAGS=          dynamic link, if your toolchain has no static libc
CROSS_COMPILE ?= riscv64-linux-gnu-
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -static

dma_one_burst: dma_one_burst.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f dma_one_burst

.PHONY: clean
