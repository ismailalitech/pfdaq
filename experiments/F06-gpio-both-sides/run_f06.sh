#!/bin/sh
# Run the F06 checks on the board and print everything. Runs on the board (copy gpio_both_sides next to it):
#   sh run_f06.sh [path to gpio_both_sides]
# Reads what the kernel says about the GPIO chips and the LED class, then runs the program once read-only and
# twice with writes. The writes are to led1 (LED class) and to CoreGPIO GPIO_OUT; both are put back.
P=${1:-/root/gpio_both_sides}
echo "### kernel: $(uname -r)"
echo "### gpiodetect"; gpiodetect
echo "### gpioinfo lines held by a driver"; gpioinfo | grep -E "^gpiochip|consumer"
echo "### /sys/class/leds"
for l in /sys/class/leds/led*; do echo "$(basename $l) brightness $(cat $l/brightness) trigger $(sed 's/.*\[\(.*\)\].*/\1/' $l/trigger)"; done
echo "### /sys/kernel/debug/gpio"; cat /sys/kernel/debug/gpio
echo "### UIO devices"
for u in /sys/class/uio/uio*; do echo "$(basename $u) $(cat $u/name) addr $(cat $u/maps/map0/addr) size $(cat $u/maps/map0/size)"; done
echo "### interrupt lines that have a handler (PLIC sources)"; grep PLIC /proc/interrupts | sed 's/^ *\([0-9]*\):.*PLIC *\([0-9]*\) *[A-Za-z]* *\(.*\)$/PLIC source \2: \3/'
echo "### gpio_both_sides -n"; $P -n
echo "### gpio_both_sides, run 1"; $P
echo "### gpio_both_sides, run 2"; $P
echo "### after: led1 brightness $(cat /sys/class/leds/led1/brightness); $(grep 'gpio-17 ' /sys/kernel/debug/gpio)"
