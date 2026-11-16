// SPDX-License-Identifier: MIT
/*
 * dma_compare - the same transfer by three movers: the fabric DMA (stream source -> DDR), the MSS DMA
 * (the PDMA, memory -> memory, through the kernel's dma-proxy device) and the CPU (memcpy).
 *
 * One configuration per run, one JSON line of output. The buffers are inside the three 32 MiB regions that
 * the device tree reserves for u-dma-buf: udmabuf-ddr-c0 (cached window), udmabuf-ddr-nc0 (non-cached) and
 * udmabuf-ddr-nc-wcb0 (non-cached, write-combining). They are the same DDR bytes seen through three windows.
 *
 *   -m fabric   the stream source sends -b bytes (1 KiB frames, counter payload) and the fabric DMA writes
 *               them into a buffer of the window given with -D. One descriptor armed per transfer, as pfdaq_cap does.
 *   -m pdma     the MSS DMA copies -b bytes from a buffer in window -R to a buffer in window -D, with the
 *               ioctls of /dev/dma-proxyN (MPFS_DMA_PROXY_START_XFER, then FINISH_XFER: sleeps until done).
 *               -c N uses N of the four channels at once, each with its own buffers (-b at most 1 MiB then).
 *   -m memcpy   the CPU copies -b bytes from window -R to window -D with memcpy().
 *   -m idle     sleeps; shows what the system uses with nothing running (the floor of the busy figure).
 * Window letters: c, nc, wcb.   -t seconds   the timed loop runs at least this long (default 1.0).
 * -n reps       and at least this many transfers (default 8).   -C  read 8 MiB elsewhere before every transfer
 *               (untimed), so the source is not in the caches.   -V  skip the correctness check.
 *
 * Per transfer it records wall time and the CPU time of this thread (CLOCK_THREAD_CPUTIME_ID) around the
 * transfer, and over the whole loop the busy time of all CPUs from /proc/stat. Before the timed loop one
 * transfer is checked word for word against what it must contain (the destination is first filled with
 * another pattern), and for a destination in the cached window also after the processor read 8 MiB elsewhere
 * (which pushes its cache lines out), through the non-cached window.
 *
 * Register offsets are those of pfdaq_cap.c (docs/modules/I04). Do not run it while another program uses
 * the stream source or the DMA controller.
 */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>

#define SRC_OFF      0xF00u
#define SRC_ID       0x00
#define SRC_CTRL     0x08
#define SRC_PAYLOAD  0x0C
#define SRC_FRAMES   0x10
#define SRC_GAP      0x14
#define SRC_FAULT    0x18
#define SRC_STATUS   0x1C
#define SRC_SNAP_SEQ 0x30
#define SRC_MODE     0x34
#define SRC_ID_VALUE 0x50464451u
#define DMA_STAT     0x10
#define DMA_MASK     0x14
#define DMA_CLEAR    0x18
#define DMA_STREAM0  0x460
#define DESC_ARMED   0xDu
#define PAYLOAD      252u
#define FLEN         256u               /* words per frame */
#define FBYTES       (FLEN * 4u)        /* 1024 bytes per frame */

/* offsets inside each 32 MiB region (the same bytes in all three windows) */
#define DST_OFF      0x400000u          /* destination of channel k: DST_OFF + k * spacing */
#define SRC_AREA_OFF 0x1000000u         /* source of channel k: SRC_AREA_OFF + k * spacing */
#define THRASH_OFF   0x1800000u         /* 8 MiB read to push lines out of the caches */
#define THRASH_LEN   0x800000u
#define MAX_BYTES    0x400000u

/* the kernel's dma-proxy interface (include/uapi/misc/mpfs-dma-proxy.h of the board's kernel) */
struct proxy_cfg { uint64_t src; uint64_t dst; unsigned long length; };
#define PROXY_START  _IOW('u', '2', struct proxy_cfg *)
#define PROXY_FINISH _IOW('u', '1', int *)

enum { K_C, K_NC, K_WCB, K_N };
static const char *kname[K_N] = { "c", "nc", "wcb" };
static const char *kdev[K_N] = { "udmabuf-ddr-c0", "udmabuf-ddr-nc0", "udmabuf-ddr-nc-wcb0" };
static volatile uint32_t *map[K_N];
static uint64_t phys[K_N];

static volatile uint32_t *src, *dma;
static int dma_fd = -1;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double cpu_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int read_str(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f || !fgets(buf, len, f)) { if (f) fclose(f); return -1; }
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int open_uio(const char *name, volatile uint32_t **m)
{
	char path[300], val[128];
	struct dirent *e;
	DIR *d = opendir("/sys/class/uio");
	while (d && (e = readdir(d))) {
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
		if (strncmp(e->d_name, "uio", 3) || read_str(path, val, sizeof(val)) || strcmp(val, name))
			continue;
		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		closedir(d);
		int fd = open(path, O_RDWR | O_SYNC);
		void *p = fd < 0 ? MAP_FAILED : mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) return -1;
		*m = p;
		return fd;
	}
	if (d) closedir(d);
	return -1;
}

static int map_kind(int k)
{
	char path[200], buf[64];
	snprintf(path, sizeof(path), "/sys/class/u-dma-buf/%s/phys_addr", kdev[k]);
	if (read_str(path, buf, sizeof(buf))) return -1;
	phys[k] = strtoull(buf, NULL, 0);
	snprintf(path, sizeof(path), "/dev/%s", kdev[k]);
	int fd = open(path, k == K_C ? O_RDWR : O_RDWR | O_SYNC);
	if (fd < 0) return -1;
	void *m = mmap(NULL, 0x2000000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) return -1;
	map[k] = m;
	return 0;
}

/* whole-system time from /proc/stat, in clock ticks of 10 ms: busy (user, nice, system, irq, softirq, steal) and idle (idle, iowait).
 * The idle figure is exact (the kernel stamps idle entry and exit); the busy figure is sampled by the timer tick and misses short
 * bursts, so the report uses "cores not idle" = online cores - idle time / wall time. */
static void proc_stat(double *busy, double *idle)
{
	FILE *f = fopen("/proc/stat", "r");
	unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, q = 0, sq = 0, st = 0;
	if (f) {
		if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &w, &q, &sq, &st) < 4) u = n = s = i = w = q = sq = st = 0;
		fclose(f);
	}
	*busy = (double)(u + n + s + q + sq + st);
	*idle = (double)(i + w);
}

static uint32_t thrash(void)
{
	volatile uint32_t *t = map[K_C] + THRASH_OFF / 4;
	uint32_t s = 0;
	for (uint32_t i = 0; i < THRASH_LEN / 4; i += 16) s += t[i];
	for (uint32_t i = 0; i < THRASH_LEN / 4; i += 16) s += t[i];
	return s;
}

/* patterns: pseudo-random words that cannot be mistaken for the counter payload */
static inline uint32_t src_w(uint32_t ch, uint32_t i) { return 0x5EC0DE00u ^ ((i + ch * 0x10001u) * 0x9E3779B1u); }
static inline uint32_t pre_w(uint32_t ch, uint32_t i) { return 0x0D0D0000u ^ ((i + ch * 0x10001u) * 0x85EBCA6Bu); }

/* the word the fabric writes at word i of the burst (counter payload); timestamp and CRC words are not counted */
static inline int new_w(uint32_t i, uint32_t seq0, uint32_t *w)
{
	uint32_t f = i / FLEN, j = i % FLEN;
	if (j == 0) { *w = (0xA55Au << 16) | PAYLOAD; return 1; }
	if (j == 1) { *w = seq0 + f; return 1; }
	if (j == 2 || j == FLEN - 1) return 0;
	*w = (((seq0 + f) & 0xFFFFu) << 16) | (j - 3);
	return 1;
}

static int wait_done(int timeout_ms, uint32_t *stat)
{
	struct pollfd p = { .fd = dma_fd, .events = POLLIN };
	uint32_t cnt;
	if (poll(&p, 1, timeout_ms) <= 0 || read(dma_fd, &cnt, 4) != 4) return -1;
	*stat = dma[DMA_STAT / 4];
	dma[DMA_CLEAR / 4] = 0xF;
	if (*stat & 0xE) return -1;
	if (map[K_NC][0] & 0x4) return -1;                 /* descriptor still marked ready */
	return 0;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* ---- the three movers ---- */
static uint32_t nbytes;
static int dk, sk, nch = 1;
static uint32_t spacing;
static int pfd[4] = { -1, -1, -1, -1 };

static int op_fabric(void)
{
	uint32_t stat;
	volatile uint32_t *desc = map[K_NC];
	desc[1] = nbytes;
	desc[2] = (uint32_t)(phys[dk] + DST_OFF);
	desc[0] = DESC_ARMED;
	__sync_synchronize();
	dma[DMA_STREAM0 / 4] = (uint32_t)(phys[K_NC] + 0);
	__sync_synchronize();
	src[SRC_CTRL / 4] = 0x1;
	return wait_done(2000, &stat);
}

static int op_pdma(void)
{
	struct proxy_cfg cfg[4];
	int st[4], bad = 0;
	for (int k = 0; k < nch; k++) {
		cfg[k].src = phys[sk] + SRC_AREA_OFF + (uint64_t)k * spacing;
		cfg[k].dst = phys[dk] + DST_OFF + (uint64_t)k * spacing;
		cfg[k].length = nbytes;
		if (ioctl(pfd[k], PROXY_START, &cfg[k])) bad = 1;
	}
	for (int k = 0; k < nch; k++) {
		st[k] = -1;
		if (ioctl(pfd[k], PROXY_FINISH, &st[k]) || st[k] != 0) bad = 1;
	}
	return bad;
}

static int op_memcpy(void)
{
	memcpy((void *)(map[dk] + DST_OFF / 4), (const void *)(map[sk] + SRC_AREA_OFF / 4), nbytes);
	__asm__ volatile("" ::: "memory");
	return 0;
}

/* the CPU time that the two thread-clock calls themselves put inside the timed section */
static double clock_overhead(void)
{
	double sum = 0;
	for (int i = 0; i < 4000; i++) {
		double c0 = cpu_s(), t0 = now_s();
		double t1 = now_s(), c1 = cpu_s();
		(void)t0; (void)t1;
		sum += c1 - c0;
	}
	return sum / 4000;
}

int main(int argc, char **argv)
{
	const char *mode = "pdma";
	double tmin = 1.0;
	int opt, minreps = 8, cold = 0, verify = 1;

	dk = K_NC; sk = K_NC; nbytes = 0x100000u;
	while ((opt = getopt(argc, argv, "m:b:D:R:t:n:c:CV")) != -1) {
		switch (opt) {
		case 'm': mode = optarg; break;
		case 'b': nbytes = strtoul(optarg, NULL, 0); break;
		case 'D': case 'R': {
			int k = !strcmp(optarg, "c") ? K_C : !strcmp(optarg, "nc") ? K_NC : !strcmp(optarg, "wcb") ? K_WCB : -1;
			if (k < 0) { fprintf(stderr, "window must be c, nc or wcb\n"); return 2; }
			if (opt == 'D') dk = k; else sk = k;
			break;
		}
		case 't': tmin = atof(optarg); break;
		case 'n': minreps = atoi(optarg); break;
		case 'c': nch = atoi(optarg); break;
		case 'C': cold = 1; break;
		case 'V': verify = 0; break;
		default:
			fprintf(stderr, "usage: %s -m fabric|pdma|memcpy|idle -b bytes [-D c|nc|wcb] [-R c|nc|wcb] [-c channels] [-t s] [-n reps] [-C] [-V]\n", argv[0]);
			return 2;
		}
	}
	int is_fabric = !strcmp(mode, "fabric"), is_pdma = !strcmp(mode, "pdma"), is_memcpy = !strcmp(mode, "memcpy"), is_idle = !strcmp(mode, "idle");
	if (!is_fabric && !is_pdma && !is_memcpy && !is_idle) { fprintf(stderr, "unknown mode\n"); return 2; }
	if (nch < 1 || nch > 4 || (nch > 1 && !is_pdma)) { fprintf(stderr, "-c 1..4, pdma only\n"); return 2; }
	spacing = nch == 1 ? 0x400000u : 0x100000u;
	if (!nbytes || nbytes > MAX_BYTES || (nch > 1 && nbytes > 0x100000u) || (is_fabric && nbytes % FBYTES)) {
		fprintf(stderr, "bytes: 1..4 MiB (1 MiB with several channels), a multiple of 1024 for the fabric\n");
		return 2;
	}

	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (is_idle) {
		double b0, i0, b1, i1, t0 = now_s();
		proc_stat(&b0, &i0);
		usleep((useconds_t)(tmin * 1e6));
		proc_stat(&b1, &i1);
		double el = now_s() - t0;
		printf("{\"mode\": \"idle\", \"elapsed_s\": %.3f, \"non_idle_cores\": %.3f, \"ticks_idle\": %.0f, \"online_cores\": %ld}\n",
		       el, (double)ncpu - (i1 - i0) / 100.0 / el, i1 - i0, ncpu);
		return 0;
	}

	for (int k = 0; k < K_N; k++)
		if (map_kind(k)) { fprintf(stderr, "cannot map %s\n", kdev[k]); return 1; }
	if (is_fabric) {
		int src_fd = open_uio("pfdaq_stream_src", &src), dfd = open_uio("dma-controller@60010000", &dma);
		if (src_fd < 0 || dfd < 0) { perror("uio"); return 1; }
		dma_fd = dfd;
		src = (volatile uint32_t *)((volatile uint8_t *)src + SRC_OFF);
		if (src[SRC_ID / 4] != SRC_ID_VALUE) { fprintf(stderr, "stream source ID %08x\n", src[SRC_ID / 4]); return 1; }
		if (src[SRC_STATUS / 4] & 1) { fprintf(stderr, "stream source busy\n"); return 1; }
		src[SRC_MODE / 4] = 0;
		dma[DMA_MASK / 4] = 0xF;
		dma[DMA_CLEAR / 4] = 0xF;
		{ struct pollfd p = { .fd = dma_fd, .events = POLLIN }; uint32_t c; while (poll(&p, 1, 10) > 0 && read(dma_fd, &c, 4) == 4) ; }
		src[SRC_CTRL / 4] = 0x2;
		usleep(1000);
		src[SRC_PAYLOAD / 4] = PAYLOAD;
		src[SRC_FRAMES / 4] = nbytes / FBYTES;
		src[SRC_GAP / 4] = 0;
		src[SRC_FAULT / 4] = 0;
	}
	if (is_pdma) {
		for (int k = 0; k < nch; k++) {
			char p[64];
			snprintf(p, sizeof(p), "/dev/dma-proxy%d", k);
			pfd[k] = open(p, O_RDWR);
			if (pfd[k] < 0) { perror(p); return 1; }
		}
	}

	int (*op)(void) = is_fabric ? op_fabric : is_pdma ? op_pdma : op_memcpy;
	uint32_t words = nbytes / 4;

	/* ---- correctness of one transfer ---- */
	uint32_t bad_own = 0, bad_ddr = 0, counted = 0, seq0 = 0;
	int ok_op = 0, vdone = 0;
	/* Earlier programs may have left modified lines of these buffers in the caches (they wrote through the cached window).
	 * Written back later, they would overwrite what is written through the other windows: push them out first. */
	thrash();
	if (verify) {
		vdone = 1;
		if (is_fabric) {
			src[SRC_CTRL / 4] = 0x2;
			usleep(1000);
			seq0 = src[SRC_SNAP_SEQ / 4];
			volatile uint32_t *d = map[dk] + DST_OFF / 4;
			for (uint32_t i = 0; i < words; i++) d[i] = pre_w(0, i);
		} else {
			for (int k = 0; k < nch; k++) {
				volatile uint32_t *s = map[sk] + (SRC_AREA_OFF + k * spacing) / 4, *d = map[dk] + (DST_OFF + k * spacing) / 4;
				for (uint32_t i = 0; i < words; i++) s[i] = src_w(k, i);
				for (uint32_t i = 0; i < words; i++) d[i] = pre_w(k, i);
			}
		}
		__sync_synchronize();
		ok_op = op() == 0;
		__sync_synchronize();
		for (int pass = 0; pass < 2; pass++) {
			uint32_t bad = 0, n = 0;
			if (pass == 1) thrash();                          /* pass 1: what DDR holds, read through the non-cached window */
			for (int k = 0; k < nch; k++) {
				volatile uint32_t *d = (pass == 0 ? map[dk] : map[K_NC]) + (DST_OFF + k * spacing) / 4;
				for (uint32_t i = 0; i < words; i++) {
					uint32_t w;
					if (is_fabric) { if (!new_w(i, seq0, &w)) continue; }
					else w = src_w(k, i);
					n++;
					if (d[i] != w) bad++;
				}
			}
			if (pass == 0) { bad_own = bad; counted = n; } else bad_ddr = bad;
		}
		if (is_fabric) {
			src[SRC_CTRL / 4] = 0x2;
			usleep(1000);
		}
	} else {
		for (int k = 0; k < nch; k++) {
			volatile uint32_t *s = map[sk] + (SRC_AREA_OFF + k * spacing) / 4;
			for (uint32_t i = 0; i < words; i++) s[i] = src_w(k, i);
		}
		ok_op = 1;
	}

	/* ---- the timed loop ---- */
	double ovh = clock_overhead();
	int cap = 200000;
	double *dt = malloc(cap * sizeof(double)), *dc = malloc(cap * sizeof(double));
	int reps = 0, fails = 0;
	double b0, i0, b1, i1;
	struct rusage r0, r1;
	thrash();
	proc_stat(&b0, &i0);
	getrusage(RUSAGE_SELF, &r0);
	double t_start = now_s(), c_start = cpu_s();
	while (reps < cap && (reps < minreps || now_s() - t_start < tmin)) {
		if (cold) thrash();
		double c0 = cpu_s(), t0 = now_s();
		if (op()) fails++;
		double t1 = now_s(), c1 = cpu_s();
		dt[reps] = t1 - t0;
		dc[reps] = c1 - c0;
		reps++;
	}
	double wall = now_s() - t_start, cpu_loop = cpu_s() - c_start;
	getrusage(RUSAGE_SELF, &r1);
	proc_stat(&b1, &i1);
	if (is_fabric) src[SRC_CTRL / 4] = 0x2;

	double sum = 0, sumc = 0, mn = 1e9, mx = 0;
	for (int i = 0; i < reps; i++) {
		sum += dt[i];
		sumc += dc[i];
		if (dt[i] < mn) mn = dt[i];
		if (dt[i] > mx) mx = dt[i];
	}
	double mean = sum / reps, cmean = sumc / reps;
	qsort(dt, reps, sizeof(double), cmp_d);
	double med = dt[reps / 2];
	double total = (double)nbytes * nch;
	double idle_ticks = i1 - i0;
	int sys_ok = !cold;                                       /* with -C the loop includes the untimed 8 MiB read */

	printf("{\"mode\": \"%s\", \"src\": \"%s\", \"dst\": \"%s\", \"channels\": %d, \"bytes\": %u, \"cold\": %s, \"reps\": %d, \"fails\": %d, "
	       "\"verify_done\": %s, \"op_ok\": %s, \"counted_words\": %u, \"bad_words_own_view\": %u, \"bad_words_ddr_view\": %u, "
	       "\"ms_mean\": %.4f, \"ms_median\": %.4f, \"ms_min\": %.4f, \"ms_max\": %.4f, \"mb_per_s\": %.2f, \"ms_per_mib\": %.4f, "
	       "\"cpu_ms_mean\": %.4f, \"cpu_clock_overhead_ms\": %.4f, \"cpu_ms_net\": %.4f, \"cpu_share\": %.4f, \"loop_wall_s\": %.3f, \"loop_cpu_s\": %.3f, ",
	       mode, is_fabric ? "stream" : kname[sk], kname[dk], nch, nbytes, cold ? "true" : "false", reps, fails,
	       vdone ? "true" : "false", ok_op ? "true" : "false", counted, bad_own, bad_ddr,
	       mean * 1e3, med * 1e3, mn * 1e3, mx * 1e3, total / mean / 1e6, mean * 1e3 / (total / 1048576.0),
	       cmean * 1e3, ovh * 1e3, (cmean > ovh ? cmean - ovh : 0) * 1e3, (cmean > ovh ? cmean - ovh : 0) / mean, wall, cpu_loop);
	if (sys_ok)
		printf("\"non_idle_cores\": %.3f, \"online_cores\": %ld, ", (double)ncpu - idle_ticks / 100.0 / wall, ncpu);
	else
		printf("\"non_idle_cores\": null, \"online_cores\": %ld, ", ncpu);
	printf("\"rusage_utime_s\": %.3f, \"rusage_stime_s\": %.3f, \"vol_ctx\": %ld, \"invol_ctx\": %ld}\n",
	       (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) * 1e-6,
	       (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) * 1e-6,
	       r1.ru_nvcsw - r0.ru_nvcsw, r1.ru_nivcsw - r0.ru_nivcsw);
	return (fails || !ok_op) ? 1 : 0;
}
