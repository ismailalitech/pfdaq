// SPDX-License-Identifier: MIT
/*
 * pfdaq_lat - drive the fabric latency unit (pfdaq_latency.v) and report its statistics.
 *
 * The fabric raises a level interrupt and counts 50 MHz clock cycles (20 ns) until software
 * writes the channel's ACK register. All statistics are kept in the fabric; this tool only
 * configures a run, acknowledges (channel 0) and reads the results.
 *
 *   channel 0: interrupt F2M[1] to Linux, delivered through the UIO device "pfdaq_stream_src".
 *              Measured path: PLIC -> kernel UIO handler -> wake-up of this process -> ACK write.
 *   channel 1: interrupt F2M[5] to the context on U54_4, acknowledged in its interrupt handler
 *              or by a FreeRTOS task (select with `pfdaq_cap -L 0|1`). This tool only starts the
 *              run and waits for it to finish.
 *   -k         channel 0 acknowledged inside the Linux kernel interrupt handler: needs the
 *              pfdaq_uio driver bound to the device (see pfdaq_uio.c); this tool only starts the
 *              run and waits.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define SLOT_OFF     0xF00u
#define SRC_VERSION  0x04
#define LAT_BASE(ch) (0x80u + (ch) * 0x40u)
#define L_CTRL       0x00
#define L_PERIOD     0x04
#define L_LIMIT      0x08
#define L_ACK        0x0C
#define L_STATUS     0x10
#define L_EVENTS     0x14
#define L_ACKS       0x18
#define L_OVERRUN    0x1C
#define L_LAST       0x20
#define L_MIN        0x24
#define L_MAX        0x28
#define L_SUM_LO     0x2C
#define L_SUM_HI     0x30
#define L_HIST_SEL   0x34
#define L_HIST_DATA  0x38
#define L_SPURIOUS   0x3C
#define NS_PER_CYCLE 20.0          /* FIC3 clock of the reference design: 50 MHz */

static volatile uint32_t *slot;

static int open_uio(const char *name, void **map)
{
	DIR *d = opendir("/sys/class/uio");
	struct dirent *e;
	char path[300], val[128];

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		FILE *f;
		if (strncmp(e->d_name, "uio", 3))
			continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
		if (!(f = fopen(path, "r")))
			continue;
		if (!fgets(val, sizeof(val), f)) val[0] = 0;
		fclose(f);
		val[strcspn(val, "\n")] = 0;
		if (strcmp(val, name))
			continue;
		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		closedir(d);
		int fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0)
			return -1;
		*map = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		return *map == MAP_FAILED ? -1 : fd;
	}
	closedir(d);
	errno = ENODEV;
	return -1;
}

/* lower and upper bound, in cycles, of histogram bin b (see pfdaq_latency.v) */
static void bin_range(int b, uint64_t *lo, uint64_t *hi)
{
	if (b < 2) { *lo = *hi = b; return; }
	int m = b / 2, s = b % 2;
	*lo = (1ull << m) + (uint64_t)s * (1ull << (m - 1));
	*hi = *lo + (1ull << (m - 1)) - 1;
}

int main(int argc, char **argv)
{
	unsigned ch = 0, period_us = 1000, count = 10000, jitter = 0x3FF, rt_prio = 0, kack = 0;
	int opt, timeout_ms = 2000;
	void *m;

	while ((opt = getopt(argc, argv, "c:p:n:j:R:t:kh")) != -1) {
		switch (opt) {
		case 'c': ch = strtoul(optarg, NULL, 0); break;
		case 'p': period_us = strtoul(optarg, NULL, 0); break;
		case 'n': count = strtoul(optarg, NULL, 0); break;
		case 'j': jitter = strtoul(optarg, NULL, 0) & 0xFFFF; break;
		case 'R': rt_prio = strtoul(optarg, NULL, 0); break;
		case 't': timeout_ms = atoi(optarg); break;
		case 'k': kack = 1; break;
		default:
			fprintf(stderr, "usage: %s [-c 0|1] [-p period_us] [-n events] [-j jitter_mask_cycles] [-R fifo_prio] [-t timeout_ms]\n"
				"  -c 0  Linux acknowledges from this process (default); -c 1  FreeRTOS acknowledges in its ISR\n"
				"  -R n  run this process with SCHED_FIFO priority n and locked memory (channel 0)\n", argv[0]);
			return 2;
		}
	}
	if (ch > 1 || !count || !period_us)
		return 2;

	int fd = open_uio("pfdaq_stream_src", &m);
	if (fd < 0) { perror("uio pfdaq_stream_src"); return 1; }
	slot = (volatile uint32_t *)((uint8_t *)m + SLOT_OFF);
	volatile uint32_t *lat = slot + LAT_BASE(ch) / 4;
	uint32_t version = slot[SRC_VERSION / 4];
	if (version < 0x00040000) { fprintf(stderr, "fabric version %08x has no latency unit\n", version); return 1; }

	if (kack) {
		/* the in-kernel acknowledge only exists with the pfdaq_uio driver bound */
		char drv[300] = "", path[300], val[16] = "";
		DIR *d = opendir("/sys/class/uio");
		struct dirent *e;
		while (d && (e = readdir(d))) {
			char nm[64] = "";
			FILE *f;
			snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
			if (!(f = fopen(path, "r"))) continue;
			if (!fgets(nm, sizeof(nm), f)) nm[0] = 0;
			fclose(f);
			nm[strcspn(nm, "\n")] = 0;
			if (strcmp(nm, "pfdaq_stream_src")) continue;
			snprintf(path, sizeof(path), "/sys/class/uio/%s/device/driver", e->d_name);
			ssize_t n = readlink(path, drv, sizeof(drv) - 1);
			drv[n > 0 ? n : 0] = 0;
		}
		if (d) closedir(d);
		FILE *f = fopen("/sys/module/pfdaq_uio/parameters/kernel_ack", "r");
		if (f) { if (!fgets(val, sizeof(val), f)) val[0] = 0; fclose(f); }
		if (!strstr(drv, "pfdaq_uio") || val[0] != 'Y') {
			fprintf(stderr, "-k needs the pfdaq_uio driver bound with kernel_ack=Y (driver: %s)\n", drv[0] ? drv : "none");
			return 1;
		}
		ch = 0;
	}
	if (rt_prio && ch == 0) {
		struct sched_param sp = { .sched_priority = (int)rt_prio };
		if (sched_setscheduler(0, SCHED_FIFO, &sp)) perror("sched_setscheduler");
		if (mlockall(MCL_CURRENT | MCL_FUTURE)) perror("mlockall");
	}

	uint32_t period = (uint32_t)(period_us * 1000.0 / NS_PER_CYCLE);
	uint32_t one = 1, cnt;
	struct pollfd p = { .fd = fd, .events = POLLIN };
	struct timespec t0, t1;
	unsigned timeouts = 0, empty_wakeups = 0;

	lat[L_CTRL / 4] = 0;                       /* stop, then drop a stale interrupt */
	if (lat[L_STATUS / 4] & 1) lat[L_ACK / 4] = 1;
	lat[L_CTRL / 4] = 0x4;                     /* clear statistics */
	lat[L_PERIOD / 4] = period;
	lat[L_LIMIT / 4] = count;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (ch == 0 && !kack) {
		if (write(fd, &one, 4) != 4) { perror("uio irq enable"); return 1; }
		while (poll(&p, 1, 0) > 0 && read(fd, &cnt, 4) == 4)      /* drop old UIO events */
			if (write(fd, &one, 4) != 4) break;
		lat[L_CTRL / 4] = ((uint32_t)jitter << 16) | 1;
		for (;;) {
			if (poll(&p, 1, timeout_ms) <= 0) {
				if (!(lat[L_STATUS / 4] & 3)) break;       /* run finished */
				timeouts++;
				if (timeouts > 3) break;
				continue;
			}
			if (read(fd, &cnt, 4) != 4) break;
			/*
			 * Seen on the board: every event wakes this process twice. The PLIC latches the
			 * still-high level again when the kernel handler completes, and delivers it once
			 * the line is re-enabled although it has been acknowledged by then. Only a wake-up
			 * with the fabric interrupt really pending is acknowledged; the others are counted.
			 */
			if (lat[L_STATUS / 4] & 1)
				lat[L_ACK / 4] = 1;                            /* stops the fabric timer */
			else
				empty_wakeups++;
			if (write(fd, &one, 4) != 4) break;                /* re-enable the interrupt line */
			if (!(lat[L_STATUS / 4] & 3)) break;
		}
	} else {
		lat[L_CTRL / 4] = ((uint32_t)jitter << 16) | 1;
		double limit_s = (double)count * (period + jitter) * NS_PER_CYCLE * 1e-9 * 1.5 + 2.0, waited = 0;
		while ((lat[L_STATUS / 4] & 3) && waited < limit_s) { usleep(20000); waited += 0.02; }
		if (lat[L_STATUS / 4] & 3) timeouts = 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	lat[L_CTRL / 4] = 0;

	uint32_t events = lat[L_EVENTS / 4], acks = lat[L_ACKS / 4], overrun = lat[L_OVERRUN / 4];
	uint32_t lmin = lat[L_MIN / 4], lmax = lat[L_MAX / 4], spurious = lat[L_SPURIOUS / 4];
	uint64_t sum = ((uint64_t)lat[L_SUM_HI / 4] << 32) | lat[L_SUM_LO / 4];
	uint32_t hist[64];
	uint64_t total = 0;
	for (int b = 0; b < 64; b++) { lat[L_HIST_SEL / 4] = b; hist[b] = lat[L_HIST_DATA / 4]; total += hist[b]; }

	/* percentiles as upper bounds: the top of the first bin that reaches the fraction */
	static const double frac[] = { 0.50, 0.90, 0.99, 0.999 };
	double pct[4] = { 0 };
	for (int k = 0; k < 4; k++) {
		uint64_t acc = 0, lo, hi;
		for (int b = 0; b < 64; b++) {
			acc += hist[b];
			if (total && acc >= (uint64_t)(frac[k] * total + 0.999999)) { bin_range(b, &lo, &hi); pct[k] = hi * NS_PER_CYCLE / 1000.0; break; }
		}
	}
	int pass = acks == count && events == count && !overrun && !timeouts && total == acks;

	printf("{\"pass\": %s, \"channel\": %u, \"context\": \"%s\", \"period_us\": %u, \"jitter_mask\": %u, \"rt_prio\": %u, "
	       "\"events\": %u, \"acks\": %u, \"overrun\": %u, \"spurious\": %u, \"empty_wakeups\": %u, \"timeouts\": %u, \"elapsed_s\": %.3f, "
	       "\"latency_us\": {\"min\": %.2f, \"mean\": %.2f, \"max\": %.2f, \"p50_le\": %.2f, \"p90_le\": %.2f, \"p99_le\": %.2f, \"p999_le\": %.2f}, "
	       "\"resolution_ns\": %.0f, \"histogram_us\": [",
	       pass ? "true" : "false", ch, ch ? "u54_4" : (kack ? "linux-kernel-isr" : (rt_prio ? "linux-uio-fifo" : "linux-uio")), period_us, jitter, ch ? 0 : rt_prio,
	       events, acks, overrun, spurious, empty_wakeups, timeouts,
	       (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9,
	       acks ? lmin * NS_PER_CYCLE / 1000.0 : 0, acks ? (double)sum / acks * NS_PER_CYCLE / 1000.0 : 0, lmax * NS_PER_CYCLE / 1000.0,
	       pct[0], pct[1], pct[2], pct[3], NS_PER_CYCLE);
	int first = 1;
	for (int b = 0; b < 64; b++) {
		uint64_t lo, hi;
		if (!hist[b]) continue;
		bin_range(b, &lo, &hi);
		printf("%s[%.2f, %.2f, %u]", first ? "" : ", ", lo * NS_PER_CYCLE / 1000.0, (hi + 1) * NS_PER_CYCLE / 1000.0, hist[b]);
		first = 0;
	}
	printf("]}\n");
	return pass ? 0 : 1;
}
