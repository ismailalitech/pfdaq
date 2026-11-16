# Build cached_dma for the board. Any RISC-V 64-bit Linux GCC works; the program is linked
# statically so it does not depend on the C library version of the board image.
#
#   make CROSS_COMPILE=riscv64-linux-gnu-
CROSS_COMPILE ?= riscv64-linux-gnu-
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -static

cached_dma: cached_dma.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f cached_dma

.PHONY: clean
