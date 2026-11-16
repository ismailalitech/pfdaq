/* SPDX-License-Identifier: MIT
 *
 * F06: the board's LEDs and DIP switches reached from both sides.
 *
 *   side 1, the processor's GPIO block (MSS GPIO2, kernel driver gpio-mpfs):
 *       - gpiochip character device: line information, and a read of DIP5-8 (lines 25 to 28)
 *       - the LED class (/sys/class/leds/ledN), which the leds-gpio driver maps onto lines 17 to 23
 *       - /sys/kernel/debug/gpio, where the kernel prints the value it reads back for a line
 *   side 2, CoreGPIO in the fabric (0x4000_0100), which has no kernel driver:
 *       - the registers through the page of the UIO device "pfdaq_stream_src" (0x4000_0000), 32-bit accesses
 *
 * It prints what software can read. It cannot see an LED; no line of the output says that one lit.
 * Writes: LED class brightness of led1 and CoreGPIO GPIO_OUT; both are put back to the values they had.
 *
 *   gpio_both_sides            run everything once
 *   gpio_both_sides -n         read only (no writes at all)
 *
 * Build: riscv64 Linux GCC, static (see Makefile). Run as root on the board.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DIP_FIRST      25          /* DIP5..DIP8 are MSS GPIO2 bits 25..28 (fabric inputs F2M) */
#define DIP_COUNT      4
#define LED_LINE_FIRST 17          /* LED1..LED7 are MSS GPIO2 bits 17..23 */

/* CoreGPIO v3.2 register map, 32-bit APB (offsets from the core's base 0x4000_0100) */
#define CG_BASE        0x100u      /* offset of the core inside the UIO page */
#define CG_CFG(n)      (CG_BASE + 4u * (n))
#define CG_IRQ         (CG_BASE + 0x80u)
#define CG_IN          (CG_BASE + 0x90u)
#define CG_OUT         (CG_BASE + 0xA0u)

static int read_file(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, (int)n, f)) { fclose(f); return -1; }
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	int r = fputs(text, f) < 0 ? -1 : 0;
	if (fclose(f))
		r = -1;
	return r;
}

/* "out hi" / "out lo" of one line as the kernel prints it in debugfs, or "?" */
static const char *debugfs_state(int line, char *out, size_t n)
{
	char key[32], buf[256];
	FILE *f = fopen("/sys/kernel/debug/gpio", "r");
	int in_chip1 = 0;

	snprintf(out, n, "?");
	if (!f)
		return out;
	snprintf(key, sizeof(key), "gpio-%d ", line);
	while (fgets(buf, sizeof(buf), f)) {
		if (!strncmp(buf, "gpiochip", 8))
			in_chip1 = strstr(buf, "20122000.gpio") != NULL;
		else if (in_chip1 && strstr(buf, key)) {
			char *p = strstr(buf, ") ");      /* " gpio-17  (   |led1   ) out hi " */
			if (p) {
				snprintf(out, n, "%s", p + 2);
				for (size_t i = strlen(out); i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\n'); i--)
					out[i - 1] = 0;
			}
			break;
		}
	}
	fclose(f);
	return out;
}

static int find_uio(const char *name)
{
	DIR *d = opendir("/sys/class/uio");
	struct dirent *e;
	char path[300], val[128];
	int fd = -1;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "uio", 3))
			continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
		if (read_file(path, val, sizeof(val)) || strcmp(val, name))
			continue;
		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		fd = open(path, O_RDWR | O_SYNC);
		break;
	}
	closedir(d);
	return fd;
}

static int chip_by_label(const char *label, char *devpath, size_t n)
{
	for (int i = 0; i < 8; i++) {
		struct gpiochip_info ci;
		char p[64];
		snprintf(p, sizeof(p), "/dev/gpiochip%d", i);
		int fd = open(p, O_RDONLY);
		if (fd < 0)
			continue;
		if (!ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &ci) && !strcmp(ci.label, label)) {
			snprintf(devpath, n, "%s", p);
			close(fd);
			return 0;
		}
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	int ro = argc > 1 && !strcmp(argv[1], "-n");
	char chip[64], buf[256], st[64];
	int fd;

	/* ---- side 1: MSS GPIO2 through the gpiochip device ---------------------------------------- */
	if (chip_by_label("20122000.gpio", chip, sizeof(chip))) {
		fprintf(stderr, "no gpiochip with label 20122000.gpio\n");
		return 1;
	}
	fd = open(chip, O_RDONLY);
	if (fd < 0) { perror(chip); return 1; }
	{
		struct gpiochip_info ci;
		ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &ci);
		printf("mss  chip %s label %s lines %u\n", chip, ci.label, ci.lines);
	}
	printf("mss  line owners (kernel = held by a driver, out/in = direction the driver reports):\n");
	for (unsigned l = LED_LINE_FIRST - 1; l < DIP_FIRST + DIP_COUNT + 1; l++) {
		struct gpioline_info li = { .line_offset = l };
		ioctl(fd, GPIO_GET_LINEINFO_IOCTL, &li);
		printf("mss    line %2u  %-3s  %-6s  consumer \"%s\"\n", l, (li.flags & GPIOLINE_FLAG_IS_OUT) ? "out" : "in",
		       (li.flags & GPIOLINE_FLAG_KERNEL) ? "kernel" : "free", li.consumer);
	}

	/* DIP5..DIP8: request the four lines as inputs and read them twice */
	{
		struct gpiohandle_request rq;
		struct gpiohandle_data d;
		memset(&rq, 0, sizeof(rq));
		for (int i = 0; i < DIP_COUNT; i++)
			rq.lineoffsets[i] = DIP_FIRST + i;
		rq.lines = DIP_COUNT;
		rq.flags = GPIOHANDLE_REQUEST_INPUT;
		snprintf(rq.consumer_label, sizeof(rq.consumer_label), "f06");
		if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &rq) < 0) {
			printf("mss  DIP5-8 request failed: %s\n", strerror(errno));
		} else {
			for (int pass = 0; pass < 2; pass++) {
				if (ioctl(rq.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &d) < 0) {
					printf("mss  DIP5-8 read failed: %s\n", strerror(errno));
					break;
				}
				printf("mss  DIP5..DIP8 read %d %d %d %d (read %d)\n", d.values[0], d.values[1], d.values[2], d.values[3], pass + 1);
			}
			close(rq.fd);
		}
	}
	close(fd);

	/* a line that a driver holds cannot be requested */
	{
		struct gpiohandle_request rq;
		memset(&rq, 0, sizeof(rq));
		fd = open(chip, O_RDONLY);
		rq.lineoffsets[0] = LED_LINE_FIRST;
		rq.lines = 1;
		rq.flags = GPIOHANDLE_REQUEST_INPUT;
		int r = ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &rq);
		printf("mss  request of line %d (led1, held by leds-gpio): %s\n", LED_LINE_FIRST, r < 0 ? strerror(errno) : "granted");
		if (r >= 0)
			close(rq.fd);
		close(fd);
	}

	/* ---- side 2: CoreGPIO through the UIO page ------------------------------------------------ */
	int ufd = find_uio("pfdaq_stream_src");
	if (ufd < 0) { perror("uio pfdaq_stream_src"); return 1; }
	volatile uint32_t *pg = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, ufd, 0);
	if (pg == MAP_FAILED) { perror("mmap"); return 1; }
#define CG(off) pg[(off) / 4]
	printf("core CoreGPIO at UIO page + 0x%x (0x4000_%04x)\n", CG_BASE, CG_BASE);
	printf("core CFG0..CFG7 %08x %08x %08x %08x %08x %08x %08x %08x\n", CG(CG_CFG(0)), CG(CG_CFG(1)), CG(CG_CFG(2)), CG(CG_CFG(3)),
	       CG(CG_CFG(4)), CG(CG_CFG(5)), CG(CG_CFG(6)), CG(CG_CFG(7)));
	printf("core IRQ %08x  GPIO_IN %08x  GPIO_OUT %08x\n", CG(CG_IRQ), CG(CG_IN), CG(CG_OUT));

	if (ro) {
		printf("read-only run: nothing written\n");
		return 0;
	}

	uint32_t out0 = CG(CG_OUT);

	/* width of the output register: write ones, read back */
	CG(CG_OUT) = 0xFFFFFFFFu;
	printf("core GPIO_OUT after writing 0xffffffff: %08x\n", CG(CG_OUT));
	CG(CG_OUT) = 0;
	printf("core GPIO_OUT after writing 0:          %08x\n", CG(CG_OUT));
	for (int b = 0; b < 8; b++) {
		CG(CG_OUT) = 1u << b;
		printf("core GPIO_OUT after writing bit %d:      %08x\n", b, CG(CG_OUT));
	}
	CG(CG_OUT) = out0;

	/* ---- both sides on LED1 --------------------------------------------------------------------- */
	printf("both LED1: MSS GPIO2 line %d (kernel LED class) and CoreGPIO GPIO_OUT bit 0\n", LED_LINE_FIRST);
	const char *bp = "/sys/class/leds/led1/brightness";
	char orig[16];
	if (read_file(bp, orig, sizeof(orig))) { perror(bp); return 1; }
	for (int mss = 0; mss < 2; mss++) {
		for (int core = 0; core < 2; core++) {
			char v[8];
			snprintf(v, sizeof(v), "%d", mss);
			if (write_file(bp, v)) { perror(bp); return 1; }
			CG(CG_OUT) = (CG(CG_OUT) & ~1u) | (uint32_t)core;
			read_file(bp, buf, sizeof(buf));
			printf("both  mss=%d core=%d: led1 brightness %s, debugfs gpio-%d %s | CoreGPIO OUT bit0 %u, IN %08x\n",
			       mss, core, buf, LED_LINE_FIRST, debugfs_state(LED_LINE_FIRST, st, sizeof(st)), CG(CG_OUT) & 1u, CG(CG_IN));
		}
	}
	write_file(bp, orig);
	CG(CG_OUT) = out0;
	read_file(bp, buf, sizeof(buf));
	printf("both  restored: led1 brightness %s, GPIO_OUT %08x\n", buf, CG(CG_OUT));
	return 0;
}
