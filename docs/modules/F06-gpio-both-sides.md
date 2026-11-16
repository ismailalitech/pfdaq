---
id: F06
title: "GPIO from both sides: MSS GPIO and fabric CoreGPIO"
track: fundamentals
level: 2
summary: The same LED reached from the processor's GPIO block through the kernel and from a GPIO core in the fabric through a user program, DIP5 to DIP8 read through the gpiochip device, and what software can and cannot observe.
needs: [F01, F03]
repro: base
status: validated
hardware: [mss-gpio2, refdesign-peripherals, user-io, u54-linux, plic]
software: [linux-kernel, uio]
sources:
  - experiments/F06-gpio-both-sides/gpio_both_sides.c:CG_OUT,CG_IN,CG_CFG,find_uio,chip_by_label,debugfs_state
  - experiments/F06-gpio-both-sides/run_f06.sh
evidence: [f06-gpio-run, f06-design-wiring]
related: [F01, F03, I01, I02]
---

# GPIO from both sides: MSS GPIO and fabric CoreGPIO

After this module you can say which GPIO controller drives which pin of the Discovery Kit, read the four DIP switches that are real GPIO lines, drive an LED from the kernel's side and from the fabric's side, and tell which of those two you can observe from software and which you cannot.

## Before you start

- **Level 2.** You should know what a memory-mapped register is and have a shell on the board. Module [F01](F01-chip-and-board.md) introduces the chip and board; [F03](F03-fabric-interrupts.md) explains the fabric interrupt lines that DIP1 to DIP4 turn out to be.
- **Board:** the shared baseline, fabric v0.5.1 with the pfdaq image. Nothing else may use the LEDs while the program runs.
- **Tools:** an SSH shell on the board. To build the program, a RISC-V 64-bit Linux GCC on the PC. The libgpiod tools `gpioinfo` and `gpioget` that the image carries are enough to cross-check the first half.
- **Time:** about five minutes. The program flashes LED1 to LED7 for a moment and puts everything back.

## The idea

This chip has two kinds of GPIO, and the board uses both.

**The processor's own GPIO blocks (MSS GPIO0, 1 and 2)** are hard blocks inside the microprocessor subsystem. Linux has a driver for them (`gpio-mpfs`), so they appear as `gpiochip` devices with numbered lines, and other drivers can claim lines: the `leds-gpio` driver claims the LED lines and turns them into the LED class under `/sys/class/leds`. A line that a driver has claimed has a *consumer*, and a program cannot request it.

**CoreGPIO** is a soft IP block placed in the fabric. It is reached over the APB bus through the fabric interface controller (module [F01](F01-chip-and-board.md)), so it is a block of registers at a fixed address. Its width and its configuration are fixed when the design is built. On this image it has **no kernel driver**, so nothing owns it and the only way to reach it from Linux is to map its registers.

The board's LED pins are driven by both. In the reference design each of LED1 to LED7 goes through a two-input OR gate in the fabric: one input is a bit of CoreGPIO's output register, the other is a bit of MSS GPIO2.

![The two ways to drive LED1. The kernel's leds-gpio driver, reached through /sys/class/leds/led1, writes bit 17 of MSS GPIO2. A user program writes bit 0 of the GPIO_OUT register of CoreGPIO in the fabric, through the UIO page at 0x4000_0000. In the fabric an OR gate named OR2_LED1 combines the two signals and drives the LED1 pin, so either source lights the LED. Software can read back both registers; it cannot see the LED.](../diagrams/lab-f06-led-paths.svg)

1. The kernel side: writing `1` to `/sys/class/leds/led1/brightness` makes the `leds-gpio` driver set line 17 of `gpiochip1` (MSS GPIO2).
2. The fabric side: a program writes bit 0 of CoreGPIO's `GPIO_OUT` register.
3. In the fabric, `OR2_LED1` combines the two and drives the LED1 pin: the LED is on if either source is high.
4. Software can read back each source's register. Nothing on the board lets it read the pin, so **this module does not claim that an LED lit**.

## On this board

### Which pin belongs to whom

| Pin | Reaches the processor as | Owner on this image | How software sees it |
|---|---|---|---|
| LED1 to LED7 | the OR of MSS GPIO2 bit 17 to 23 and CoreGPIO `GPIO_OUT[0]` to `[6]` | MSS side: `leds-gpio` (consumer `led1` to `led7`); fabric side: nobody | `/sys/class/leds/led1` to `led7`; CoreGPIO through its registers |
| LED8 | MSS GPIO1 bit 9 (`gpiochip0`, line 9), no fabric logic in between | `leds-gpio` (consumer `led8`) | `/sys/class/leds/led8` |
| DIP5 to DIP8 | MSS GPIO2 bits 25 to 28, as inputs, after an inverter in the fabric | nobody | `gpiochip1`, lines 25 to 28 |
| DIP1 to DIP4 | fabric interrupt lines F2M[6] to F2M[9] (PLIC sources 124 to 127), inverted | nobody | not a GPIO line; no handler on this image |

![Where the other board pins go. DIP5 to DIP8 are inputs, through an inverter, to MSS GPIO2 bits 25 to 28, so they can be read through the gpiochip device. DIP1 to DIP4 are not GPIO lines: they are interrupt inputs F2M[6] to F2M[9], PLIC sources 124 to 127, and no handler is registered for them on this image. LED8 is driven by line 9 of the first GPIO chip, with no fabric logic in between.](../diagrams/lab-f06-pins.svg)

1. DIP5 to DIP8 are the only switches that are GPIO lines: read them through `gpiochip1`. The design inverts all eight DIP signals in the fabric, so the value a program reads is the inverse of the pin level; which position of the switch reads 1 was not tested.
2. DIP1 to DIP4 enter the PLIC as interrupt sources 124 to 127 (the formula of module F03: source = 118 + F2M index). The run lists every interrupt that has a handler and none of 124 to 127 is among them.
3. LED8 is on the other GPIO block and has no fabric part, so it has no CoreGPIO bit.

The wiring is read from the reference design's script (`evidence/v0.5.1/build/f06-design-wiring.txt` holds the lines); the program and the run below test what Linux can see of it.

### CoreGPIO in this design

The design builds CoreGPIO 3.2.102 with these parameters, and wires its input to ground:

| Parameter or connection | Value | Consequence |
|---|---|---|
| `APB_WIDTH` | 32 | 32-bit registers |
| `IO_NUM` | 7 | seven bits, one per LED1 to LED7 |
| `FIXED_CONFIG_0` | true | the per-bit configuration registers are not real: they read 0 |
| `IO_TYPE_0` | 1 (output) | the bits are outputs |
| `GPIO_IN` | tied to GND | the input register reads 0 |
| interrupt output | unused | no interrupt from this block |

Its registers, at the core's base address `0x4000_0100` (offsets from the vendor's bare-metal driver header): configuration registers `CFG0..CFG31` at `+0x00`, `IRQ` at `+0x80`, `GPIO_IN` at `+0x90`, `GPIO_OUT` at `+0xA0`. The run shows that `+0xA0` behaves as a seven-bit output register, which is what confirms it.

The page at `0x4000_0000` is mapped by the UIO device named `pfdaq_stream_src` (the project's stream source, at `+0xF00` of the same page). CoreGPIO sits at `+0x100` in that page, so the program borrows the UIO device to reach it, and touches only `+0x100` to `+0x1A0`. This is a shortcut of the lab: a design of your own would give CoreGPIO its own device-tree node.

### The program

`gpio_both_sides.c` has three parts and prints everything it reads. The first part uses the gpiochip character device to list the lines and read DIP5 to DIP8:

```c
struct gpiohandle_request rq = { 0 };
for (int i = 0; i < DIP_COUNT; i++)
    rq.lineoffsets[i] = DIP_FIRST + i;               /* lines 25 to 28 */
rq.lines = DIP_COUNT;
rq.flags = GPIOHANDLE_REQUEST_INPUT;
ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &rq);           /* fails if a driver holds a line */
ioctl(rq.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &d);  /* d.values[0..3] */
```

The second part maps the UIO page and works on CoreGPIO with 32-bit accesses (the program is C: in the project's first fabric tests Python's `mmap` stores were byte-wise and corrupted writes on the APB bus):

```c
volatile uint32_t *pg = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, ufd, 0);
#define CG(off) pg[(off) / 4]
CG(CG_OUT) = 0xFFFFFFFFu;        /* width probe: which bits keep their value? */
```

The third part drives LED1 from both sides in all four combinations. The kernel side is the LED class (`brightness`), and the program reads back the brightness, the value the kernel prints for line 17 in `/sys/kernel/debug/gpio`, and CoreGPIO's register. At the end it writes `brightness` and `GPIO_OUT` back to what they were. `-n` makes the program read only.

## Run it

```sh
make -C experiments/F06-gpio-both-sides CROSS_COMPILE=riscv64-linux-gnu-
scp experiments/F06-gpio-both-sides/gpio_both_sides experiments/F06-gpio-both-sides/run_f06.sh root@<board>:/root/
ssh root@<board> 'sh /root/run_f06.sh'
```

`run_f06.sh` lists the GPIO chips, the line owners, the LED class, the debugfs view, the UIO devices and the interrupt sources that have a handler, then runs the program once read-only and twice with writes. Cross-check the first half by hand with `gpioinfo`, `gpioget --numeric -c gpiochip1 25 26 27 28` and `cat /sys/kernel/debug/gpio`. Requesting DIP5 to DIP8 switches those four lines to inputs until the next reboot (the chip reports them as outputs after boot).

## What you should see

![Recorded output of the program. Line 17 of MSS GPIO2 is held by the kernel for led1 and a request for it fails with device busy. DIP5 to DIP8 read 0 0 0 0. CoreGPIO's configuration registers read 0, its GPIO_OUT register keeps 7 bits (writing 0xffffffff reads back 0x7f, and bit 7 reads back as 0), and its GPIO_IN reads 0. The four combinations of the kernel side and the fabric side for LED1 each read back what was written to that side, independently.](../diagrams/lab-f06-run.svg)

The recorded run (`f06-gpio-run`, kernel 6.18.17, two runs with writes, identical output):

1. **Line owners.** `gpiochip1` lines 17 to 23 have the consumers `led1` to `led7`, and `gpiochip0` line 9 has `led8`; nothing else is held. A request for line 17 fails with `Device or resource busy`.
2. **DIP5 to DIP8** read `0 0 0 0`, twice per run, in all three runs. The positions of the switches were not looked at, so this shows that the read path works and what it returned, not that the value matches the switches.
3. **No handler for DIP1 to DIP4.** The list of interrupt sources that have a handler has none of 124 to 127.
4. **CoreGPIO.** `CFG0..CFG7` read `00000000`, `IRQ` and `GPIO_IN` read `00000000`. After writing `0xffffffff`, `GPIO_OUT` reads `0000007f`: seven bits. Writing each bit in turn reads back that bit, and bit 7 reads back as `0`. `GPIO_IN` stays `00000000` in every combination, including those where `GPIO_OUT` bit 0 is 1.
5. **Two sources.** In the four combinations of the kernel side and the fabric side, each side reads back what was written to it (`brightness` and the debugfs line `out hi` or `out lo` follow the kernel side, bit 0 of `GPIO_OUT` follows the fabric side), whatever the other side holds.
6. **Put back.** After each run `brightness` is `0` and `GPIO_OUT` is `00000000`.

What this does not show: that an LED lit. Nobody watched the board during the runs, and no register reads the LED pin. The OR gate is from the design's script, not from a measurement.

## Evidence

- [`f06-gpio-run`](../../evidence/v0.5.1/board/f06-gpio-run.txt): the whole output of `run_f06.sh`: the kernel's view, the read-only run and the two runs with writes.
- [`f06-design-wiring`](../../evidence/v0.5.1/build/f06-design-wiring.txt): the connections and parameters quoted from the reference design's script (the LED OR gates, the DIP pins, `GPIO_IN` tied to ground, the CoreGPIO parameters).

## When it goes wrong

| Symptom | Likely cause |
|---|---|
| `Device or resource busy` when requesting a line | a driver holds it: lines 17 to 23 of `gpiochip1` and line 9 of `gpiochip0` do (see `gpioinfo`). Use the LED class instead |
| `GPIO_OUT` reads back fewer bits than you wrote | the core was built with fewer outputs: here 7, so bit 7 and above read 0 |
| An LED stays on after the kernel side reads `0` | the pin is the OR of two sources: read CoreGPIO `GPIO_OUT` (the design's script, not observed here) |
| DIP1 to DIP4 do not show up under `gpioget` | they are interrupt inputs, not GPIO lines; they have no handler on this image |
| DIP5 to DIP8 were listed as outputs before you read them | the chip reports them that way after boot; the request in the program makes them inputs until the next reboot |
| A configuration register of CoreGPIO reads 0 | the core is built with fixed configuration: there is nothing to configure |
| The program cannot find the UIO device | the name is `pfdaq_stream_src`; look it up in `/sys/class/uio/uio*/name` |

## Trade-offs and going further

- **A hard block has a driver; a soft block has your code.** MSS GPIO comes with names, ownership and the LED class. CoreGPIO is smaller to ignore and larger to use: you write the register access yourself, and its width is a build-time choice.
- **The OR gate is a design decision with a cost.** Both sources can light an LED, and neither can switch it off alone. For a shared LED, that is the price of letting the kernel and a fabric program both use it without a handover.
- **Borrowing a UIO page is a shortcut.** It works because CoreGPIO sits in a page the project already maps. A product would give CoreGPIO its own device-tree node, with a driver or `generic-uio`.
- **Reading back is not seeing.** The register tells you what the block was told, not what the pin does. Say which of the two a test shows.
- **Exercise:** flip DIP5 and run `gpio_both_sides -n` again. Which of the four values changes? What does the same run show when you flip DIP1 instead?
- **Exercise:** run `gpioinfo -c gpiochip1` before and after the program. Which lines changed direction, and why does `gpioinfo` not list a consumer for them afterwards?

## Do the same in your project

1. **List the pins and their owners first.** `gpioinfo` shows the consumer of every line; a claimed line belongs to a driver.
2. **Use the driver's own interface for what it owns.** Here the LED class, not the raw line.
3. **For a soft IP block, read its build parameters before its registers.** Width, fixed configuration and the connection of its input decide what a read can mean.
4. **Probe the width by writing ones.** Write `0xffffffff` to an output register and see which bits keep their value.
5. **Map only what you need.** Name the offsets you touch, use 32-bit accesses, and put every register back to its old value at the end.
6. **Say what you cannot observe.** A register readback and a lit LED are two different results; do not write the second when you only have the first.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [F01 The chip and the board: MSS, fabric and the interfaces between them](F01-chip-and-board.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md) |
| **How to run** | Runs on the shared baseline: fabric v0.5.1 and the pfdaq image |
| **In the system** | [MSS GPIO2](../system/README.md#mss-gpio2) · [Reference design peripherals](../system/README.md#refdesign-peripherals) · [LEDs, switches and headers](../system/README.md#user-io) · [U54 harts 1 to 3](../system/README.md#u54-linux) · [Platform-level interrupt controller](../system/README.md#plic) · [Linux 6.18.17](../system/README.md#linux-kernel) · [UIO devices](../system/README.md#uio) |
| **Source** | [`experiments/F06-gpio-both-sides/gpio_both_sides.c`](../../experiments/F06-gpio-both-sides/gpio_both_sides.c): `CG_OUT`, `CG_IN`, `CG_CFG`, `find_uio`, `chip_by_label`, `debugfs_state`<br>[`experiments/F06-gpio-both-sides/run_f06.sh`](../../experiments/F06-gpio-both-sides/run_f06.sh) |
| **Evidence** | [GPIO from both sides on the board: GPIO chips and line owners, the LED class, the interrupt sources with a handler, and gpio_both_sides read-only and twice with writes](../../evidence/v0.5.1/board/f06-gpio-run.txt) (board, fabric v0.5.1, 2026-11-16)<br>[The connections and CoreGPIO parameters of the reference design that F06 relies on (LED OR gates, DIP pins, GPIO_IN tied to ground)](../../evidence/v0.5.1/build/f06-design-wiring.txt) (build, fabric v0.5.1, 2026-11-16) |
| **Related** | [F01 The chip and the board: MSS, fabric and the interfaces between them](F01-chip-and-board.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md)<br>[I01 Reaching fabric registers from Linux](I01-fabric-registers-from-linux.md)<br>[I02 Fabric interrupts in user space, and the double wake-up](I02-fabric-interrupts-user-space.md) |
| **Status** | validated |
<!-- nav:end -->
