# F06: GPIO from both sides

Belongs to module [F06, GPIO from both sides](../../docs/modules/F06-gpio-both-sides.md).

`gpio_both_sides.c` reaches the board's LEDs and DIP switches from both sides of the chip and prints what software can read back:

- **the processor's GPIO block (MSS GPIO2)**: the gpiochip character device (line owners, a read of DIP5 to DIP8), the LED class that the kernel's `leds-gpio` driver provides on lines 17 to 23, and the value the kernel prints for a line in `/sys/kernel/debug/gpio`;
- **CoreGPIO in the fabric**, which has no kernel driver: its registers through the page of the UIO device `pfdaq_stream_src`.

It cannot see an LED, and no line of its output says that one lit.

## Build and run

```sh
make CROSS_COMPILE=riscv64-linux-gnu-          # static RISC-V binary
scp gpio_both_sides run_f06.sh root@<board>:/root/
ssh root@<board> 'sh /root/run_f06.sh'
```

`gpio_both_sides -n` reads only. Without `-n` it writes the CoreGPIO output register and `brightness` of `led1`, and puts both back. Run as root, on the shared baseline (fabric v0.5.1 and the pfdaq image), with no other program using the LEDs. Requesting DIP5 to DIP8 switches those four MSS GPIO2 lines to inputs until the next reboot (they were reported as outputs after boot).
