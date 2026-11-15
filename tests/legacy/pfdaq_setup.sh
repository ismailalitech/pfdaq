#!/bin/sh
# Run once after boot on the board. The stream source registers (0x40000F00) share a page
# with three fabric peripherals whose drivers claim it, and the kernel then refuses /dev/mem
# access to that page. Unbind them (PWM, I2C, UART in the fabric; not used by this project).
# Temporary: the proper fix is a device-tree node for the stream source.
for p in "mchp-core-pwm 40000000.pwm" "microchip-corei2c 40000200.i2c" "mchp_coreuart 40000300.serial"; do
    set -- $p
    [ -e /sys/bus/platform/drivers/$1/$2 ] && echo $2 > /sys/bus/platform/drivers/$1/unbind
done
devmem2 0x40000F00 w | tail -1
