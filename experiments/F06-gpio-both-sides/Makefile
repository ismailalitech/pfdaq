# Build gpio_both_sides for the board. Any RISC-V 64-bit Linux GCC works; the program is linked statically.
#
#   make CROSS_COMPILE=riscv64-linux-gnu-
CROSS_COMPILE ?= riscv64-linux-gnu-
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -static

gpio_both_sides: gpio_both_sides.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f gpio_both_sides

.PHONY: clean
