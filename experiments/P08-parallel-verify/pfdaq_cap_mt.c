// SPDX-License-Identifier: MIT
/*
 * pfdaq_cap_mt - pfdaq_cap's burst loop with the copy and the verification spread over several
 * threads, so that more than one Linux core works on verification at once.
 *
 * The original does, on one thread: arm a burst, start it, copy the previous burst out of the non-cached
 * DMA buffer, verify the copy, wait for the DMA. Here the main thread only drives the DMA
 * (arm, start, wait for the interrupt) and hands each finished burst to a pool of worker threads;
 * each worker copies the burst out of its buffer, gives the buffer back, and verifies its private copy.
 * A ring of buffers in the non-cached region lets the DMA run ahead of the workers.
 *
 * Everything that is checked, and how, is taken unchanged from pfdaq_cap.c (CRC-32, the three payload
 * models, verify, payload_bad, the fault injection and the pass rule). Not copied: supervision over RPMsg
 * (-r), real-time mode, abort test and the shared-memory test.
 *
 *   -n bursts  -f frames  -w payload words  -m counter|prbs|fir  -S fir_step
 *   -j workers (default 1)   -b buffers in the ring (default workers + 2, at most 14 for 1 MiB bursts)
 *   -p        pin the main thread to CPU 0 and the workers to CPUs 1, 2, 0, ...
 *   -F crc|seq|payload -i frame -e every_nth_burst    fault injection, as pfdaq_cap
 *
 * Output: one JSON line like pfdaq_cap's, plus the threads and the CPU time used.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/resource.h>

#define SRC_OFF        0xF00u
#define SRC_ID         0x00
#define SRC_VERSION    0x04
#define SRC_CTRL       0x08
#define SRC_PAYLOAD    0x0C
#define SRC_FRAMES     0x10
#define SRC_GAP        0x14
#define SRC_FAULT      0x18
#define SRC_STATUS     0x1C
#define SRC_SNAP_STALL 0x28
#define SRC_SNAP_SEQ   0x30
#define SRC_MODE       0x34
#define SRC_FIR_STEP   0x44
#define SRC_COEF0      0x48
#define SRC_ID_VALUE   0x50464451u

#define DMA_STAT       0x10
#define DMA_MASK       0x14
#define DMA_CLEAR      0x18
#define DMA_STREAM0    0x460

#define DESC_CFG_ARMED 0xDu
#define DESC_OFF       0x0u
#define BUF_OFF        0x1000u
#define MAGIC_HI       0xA55Au
#define MAX_BURST      (8u << 20)

static volatile uint32_t *src, *dma;
static uint8_t *ddr;
static uint64_t ddr_phys;
static size_t ddr_size;
static int dma_fd = -1;
static uint32_t crc_tab[8][256];

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
		crc_tab[0][i] = c;
	}
	for (uint32_t i = 0; i < 256; i++)
		for (int t = 1; t < 8; t++)
			crc_tab[t][i] = (crc_tab[t - 1][i] >> 8) ^ crc_tab[0][crc_tab[t - 1][i] & 0xFF];
}

/* CRC-32 (IEEE) over n little-endian 32-bit words, slicing-by-4 */
static uint32_t crc32_words(const uint32_t *w, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		c ^= w[i];
		c = crc_tab[3][c & 0xFF] ^ crc_tab[2][(c >> 8) & 0xFF] ^
		    crc_tab[1][(c >> 16) & 0xFF] ^ crc_tab[0][c >> 24];
	}
	return ~c;
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int read_str(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int open_uio(const char *name, void **map, size_t maplen)
{
	DIR *d = opendir("/sys/class/uio");
	struct dirent *e;
	char path[300], val[128];

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "uio", 3))
			continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
		if (read_str(path, val, sizeof(val)) || strcmp(val, name))
			continue;
		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		closedir(d);
		int fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0)
			return -1;
		*map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (*map == MAP_FAILED) {
			close(fd);
			return -1;
		}
		return fd;
	}
	closedir(d);
	errno = ENODEV;
	return -1;
}

static void arm(uint32_t dest_off, uint32_t nbytes)
{
	volatile uint32_t *desc = (volatile uint32_t *)(ddr + DESC_OFF);
	desc[1] = nbytes;
	desc[2] = (uint32_t)(ddr_phys + dest_off);
	desc[0] = DESC_CFG_ARMED;
	dma[DMA_STREAM0 / 4] = (uint32_t)(ddr_phys + DESC_OFF);
}

/* returns 0 on completion, -1 on timeout or DMA error */
static int wait_done(int timeout_ms, uint32_t *stat)
{
	struct pollfd p = { .fd = dma_fd, .events = POLLIN };
	uint32_t cnt;

	if (poll(&p, 1, timeout_ms) <= 0)
		return -1;
	if (read(dma_fd, &cnt, 4) != 4)
		return -1;
	*stat = dma[DMA_STAT / 4];
	dma[DMA_CLEAR / 4] = 0xF;
	if (*stat & 0xE)
		return -1;
	if (((volatile uint32_t *)(ddr + DESC_OFF))[0] & 0x4)
		return -1;
	return 0;
}

struct stats {
	uint64_t frames, bytes, bad_magic, bad_seq, bad_payload, bad_crc, seq_gap;
};

/* payload model, mirrors pfdaq_stream_src.v / pfdaq_fir_src */
struct model {
	int mode;                 /* 0 counter, 1 PRBS, 2 FIR */
	uint32_t step;
	int32_t coef[8];
	uint64_t fir_i;           /* FIR samples since START */
	int32_t xh[8];            /* xh[0] = newest input sample */
};

static int32_t tri16(uint64_t p)
{
	uint32_t q = (uint32_t)(p & 0xFFFF);
	uint32_t t = (q & 0x8000) ? (~q & 0x7FFF) : (q & 0x7FFF);
	return (int32_t)t - 16384;
}

static uint32_t xs32(uint32_t x)
{
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

static void model_restart(struct model *md)
{
	md->fir_i = 0;
	memset(md->xh, 0, sizeof(md->xh));
}

/* returns 1 if the payload of frame w differs from the model (the model state always advances) */
static int payload_bad(const uint32_t *w, uint32_t payload, struct model *md)
{
	int bad = 0;
	if (md->mode == 1) {
		uint32_t x = w[1] ^ 0x9E3779B9u;
		if (!x) x = 1;
		for (uint32_t k = 0; k < payload; k++) {
			x = xs32(x);
			if (w[3 + k] != x) bad = 1;
		}
	} else if (md->mode == 2) {
		for (uint32_t k = 0; k < payload; k++) {
			memmove(md->xh + 1, md->xh, 7 * sizeof(int32_t));
			md->xh[0] = tri16((md->fir_i + 1) * md->step);
			md->fir_i++;
			int64_t y = 0;
			for (int t = 0; t < 8; t++)
				y += (int64_t)md->coef[t] * md->xh[t];
			if (w[3 + k] != (uint32_t)y) bad = 1;
		}
	} else {
		uint32_t base = (w[1] & 0xFFFF) << 16;
		for (uint32_t k = 0; k < payload; k++)
			if (w[3 + k] != (base | k)) bad = 1;
	}
	return bad;
}

/* allow_gaps: real-time mode, where a forward jump of the sequence number is a dropped frame */
static void verify(const uint32_t *w, uint32_t frames, uint32_t payload, uint32_t *exp_seq,
		   struct stats *st, struct model *md, int allow_gaps)
{
	uint32_t flen = payload + 4;

	for (uint32_t f = 0; f < frames; f++, w += flen) {
		st->frames++;
		st->bytes += flen * 4;
		if (w[0] != ((MAGIC_HI << 16) | payload)) {
			st->bad_magic++;
			continue;
		}
		if (w[1] != *exp_seq) {
			if (allow_gaps && (int32_t)(w[1] - *exp_seq) > 0)
				st->seq_gap += w[1] - *exp_seq;
			else
				st->bad_seq++;
		}
		*exp_seq = w[1] + 1;
		if (payload_bad(w, payload, md))
			st->bad_payload++;
		if (crc32_words(w, flen - 1) != w[flen - 1])
			st->bad_crc++;
	}
}


/* ------------------------------------------------------------------------------------------
 * The multi-threaded part
 * ------------------------------------------------------------------------------------------ */
struct job { int buf; uint32_t first_seq; };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_work = PTHREAD_COND_INITIALIZER, cv_free = PTHREAD_COND_INITIALIZER;
static struct job jobs[64];
static unsigned jhead, jtail, jcount;     /* queue of finished bursts */
static int free_buf[64];
static unsigned fhead, ftail, fcount;     /* queue of free buffers */
static int finished;

struct worker {
	pthread_t th;
	int id, cpu;
	uint32_t frames, payload;
	size_t burst_bytes;
	uint32_t buf_off[64];
	struct model md;
	struct stats st;
	double copy_s, verify_s, idle_s;
	unsigned bursts;
};

static void pin(int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s))
		fprintf(stderr, "cannot pin to cpu %d\n", cpu);
}

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	uint32_t *copy = malloc(w->burst_bytes);
	if (w->cpu >= 0) pin(w->cpu);
	for (;;) {
		struct job j;
		double ti = now_s();
		pthread_mutex_lock(&mu);
		while (!jcount && !finished)
			pthread_cond_wait(&cv_work, &mu);
		if (!jcount) { pthread_mutex_unlock(&mu); break; }
		j = jobs[jhead]; jhead = (jhead + 1) % 64; jcount--;
		pthread_mutex_unlock(&mu);
		w->idle_s += now_s() - ti;

		double ta = now_s();
		memcpy(copy, ddr + w->buf_off[j.buf], w->burst_bytes);
		double tb = now_s();
		pthread_mutex_lock(&mu);                       /* the buffer is free again: the copy is private */
		free_buf[ftail] = j.buf; ftail = (ftail + 1) % 64; fcount++;
		pthread_cond_signal(&cv_free);
		pthread_mutex_unlock(&mu);
		uint32_t exp_seq = j.first_seq;
		model_restart(&w->md);
		verify(copy, w->frames, w->payload, &exp_seq, &w->st, &w->md, 0);
		w->copy_s += tb - ta;
		w->verify_s += now_s() - tb;
		w->bursts++;
	}
	free(copy);
	return NULL;
}

int main(int argc, char **argv)
{
	uint32_t payload = 252, frames = 1024, bursts = 32, fault = 0, fault_frame = 0, fault_every = 0;
	int opt, nw = 1, nb = 0, pinned = 0, timeout_ms = 2000;
	struct model md0 = { .mode = 0, .step = 256 };
	memcpy(md0.coef, (int32_t[8]){ 12000, -15000, 20000, 32767, -32768, 20000, -15000, 12000 }, sizeof(md0.coef));
	void *m;
	char buf[64];

	while ((opt = getopt(argc, argv, "w:f:n:F:i:e:m:S:j:b:ph")) != -1) {
		switch (opt) {
		case 'w': payload = strtoul(optarg, NULL, 0); break;
		case 'f': frames = strtoul(optarg, NULL, 0); break;
		case 'n': bursts = strtoul(optarg, NULL, 0); break;
		case 'F': fault = !strcmp(optarg, "crc") ? 1 : !strcmp(optarg, "seq") ? 2 : !strcmp(optarg, "payload") ? 4 : 0; break;
		case 'i': fault_frame = strtoul(optarg, NULL, 0); break;
		case 'e': fault_every = strtoul(optarg, NULL, 0); break;
		case 'm': md0.mode = !strcmp(optarg, "prbs") ? 1 : !strcmp(optarg, "fir") ? 2 : 0; break;
		case 'S': md0.step = strtoul(optarg, NULL, 0) & 0xFFFF; break;
		case 'j': nw = atoi(optarg); break;
		case 'b': nb = atoi(optarg); break;
		case 'p': pinned = 1; break;
		default:
			fprintf(stderr, "usage: %s [-n bursts] [-f frames] [-w payload] [-m counter|prbs|fir] [-S step]\n"
				"          [-j workers] [-b buffers] [-p] [-F crc|seq|payload -i frame -e every_nth_burst]\n", argv[0]);
			return 2;
		}
	}
	if (nw < 1 || nw > 8) return 2;
	if (!nb) nb = nw + 2;
	if (nb < 2 || nb > 60) return 2;
	uint32_t flen = payload + 4;
	uint64_t burst_bytes = (uint64_t)flen * 4 * frames;
	if (((payload + 4) & 1) && frames > 1) { fprintf(stderr, "odd frame length: see pfdaq_cap.c\n"); return 2; }
	if (!payload || payload > 65535 || !frames || burst_bytes > MAX_BURST) return 2;

	int src_fd = open_uio("pfdaq_stream_src", &m, 0x1000);
	if (src_fd < 0) { perror("uio pfdaq_stream_src"); return 1; }
	src = (volatile uint32_t *)((uint8_t *)m + SRC_OFF);
	dma_fd = open_uio("dma-controller@60010000", &m, 0x1000);
	if (dma_fd < 0) { perror("uio dma-controller@60010000"); return 1; }
	dma = m;
	if (read_str("/sys/class/u-dma-buf/udmabuf-ddr-nc0/phys_addr", buf, sizeof(buf))) { perror("udmabuf phys_addr"); return 1; }
	ddr_phys = strtoull(buf, NULL, 0);
	if (read_str("/sys/class/u-dma-buf/udmabuf-ddr-nc0/size", buf, sizeof(buf))) { perror("udmabuf size"); return 1; }
	ddr_size = strtoull(buf, NULL, 0);
	int ddr_fd = open("/dev/udmabuf-ddr-nc0", O_RDWR | O_SYNC);
	if (ddr_fd < 0) { perror("/dev/udmabuf-ddr-nc0"); return 1; }
	ddr = mmap(NULL, ddr_size, PROT_READ | PROT_WRITE, MAP_SHARED, ddr_fd, 0);
	if (ddr == MAP_FAILED) { perror("mmap udmabuf"); return 1; }
	uint32_t slot = (uint32_t)((burst_bytes + 0xFFF) & ~0xFFFull) + 0x1000;
	uint32_t buf_off[64];
	for (int k = 0; k < nb; k++) buf_off[k] = BUF_OFF + k * slot;
	if (buf_off[nb - 1] + burst_bytes + 0x1000 > ddr_size) { fprintf(stderr, "%d buffers do not fit the %zu byte region\n", nb, ddr_size); return 2; }

	if (src[SRC_ID / 4] != SRC_ID_VALUE) { fprintf(stderr, "stream source ID %08x\n", src[SRC_ID / 4]); return 1; }
	if (src[SRC_STATUS / 4] & 1) { fprintf(stderr, "stream source busy at start\n"); return 1; }
	uint32_t version = src[SRC_VERSION / 4];
	if (md0.mode && version < 0x00050000) { fprintf(stderr, "fabric version %08x has no payload modes\n", version); return 1; }
	if (version >= 0x00050000) {
		src[SRC_MODE / 4] = (uint32_t)md0.mode;
		src[SRC_FIR_STEP / 4] = md0.step;
		for (int k = 0; k < 8; k++)
			src[SRC_COEF0 / 4 + k] = (uint16_t)md0.coef[k];
	}
	crc_init();
	dma[DMA_MASK / 4] = 0xF;
	dma[DMA_CLEAR / 4] = 0xF;
	{
		struct pollfd p = { .fd = dma_fd, .events = POLLIN };
		uint32_t cnt;
		while (poll(&p, 1, 10) > 0 && read(dma_fd, &cnt, 4) == 4)
			;
	}
	src[SRC_CTRL / 4] = 0x2;
	usleep(1000);
	uint32_t seq0 = src[SRC_SNAP_SEQ / 4], stall0 = src[SRC_SNAP_STALL / 4];
	src[SRC_PAYLOAD / 4] = payload;
	src[SRC_FRAMES / 4] = frames;
	src[SRC_GAP / 4] = 0;

	for (int k = 0; k < nb; k++) { free_buf[ftail++] = k; fcount++; }
	struct worker *w = calloc(nw, sizeof(*w));
	for (int i = 0; i < nw; i++) {
		w[i].id = i;
		w[i].cpu = pinned ? (i + 1) % 3 : -1;
		w[i].frames = frames; w[i].payload = payload; w[i].burst_bytes = burst_bytes;
		memcpy(w[i].buf_off, buf_off, sizeof(buf_off));
		w[i].md = md0;
	}
	if (pinned) pin(0);
	struct rusage ru0, ru1;
	getrusage(RUSAGE_SELF, &ru0);
	double t_start = now_s();
	for (int i = 0; i < nw; i++)
		pthread_create(&w[i].th, NULL, worker_main, &w[i]);

	uint32_t injected = 0, dma_fail = 0, stat = 0, skew = 0, issued = 0;
	double lat_min = 1e9, lat_max = 0, lat_sum = 0, blocked_s = 0;
	for (uint32_t b = 0; b < bursts; b++) {
		double tw = now_s();
		pthread_mutex_lock(&mu);
		while (!fcount)
			pthread_cond_wait(&cv_free, &mu);
		int k = free_buf[fhead]; fhead = (fhead + 1) % 64; fcount--;
		pthread_mutex_unlock(&mu);
		blocked_s += now_s() - tw;
		int inj = fault && fault_every && (b % fault_every == fault_every - 1);
		src[SRC_FAULT / 4] = inj ? ((fault_frame << 16) | fault) : 0;
		injected += inj;
		memset(ddr + buf_off[k], 0xEE, 64);              /* stale-data marker */
		arm(buf_off[k], (uint32_t)burst_bytes);
		double t0 = now_s();
		src[SRC_CTRL / 4] = 0x1;
		if (wait_done(timeout_ms, &stat)) {
			dma_fail++;
			fprintf(stderr, "burst %u: DMA timeout or error, stat=%#x\n", b, stat);
			break;
		}
		double l = now_s() - t0;
		lat_sum += l;
		if (l < lat_min) lat_min = l;
		if (l > lat_max) lat_max = l;
		pthread_mutex_lock(&mu);
		jobs[jtail] = (struct job){ k, seq0 + b * frames + skew };
		jtail = (jtail + 1) % 64; jcount++; issued++;
		pthread_cond_signal(&cv_work);
		pthread_mutex_unlock(&mu);
		if (inj && fault == 2)
			skew++;                                  /* a skipped sequence number: later bursts start one higher */
	}
	pthread_mutex_lock(&mu);
	finished = 1;
	pthread_cond_broadcast(&cv_work);
	pthread_mutex_unlock(&mu);
	for (int i = 0; i < nw; i++)
		pthread_join(w[i].th, NULL);
	double elapsed = now_s() - t_start;
	getrusage(RUSAGE_SELF, &ru1);
	usleep(1000);
	src[SRC_CTRL / 4] = 0x2;
	usleep(1000);
	uint32_t stall = src[SRC_SNAP_STALL / 4] - stall0;

	struct stats st = { 0 };
	double copy_s = 0, verify_s = 0;
	unsigned per[8] = { 0 };
	for (int i = 0; i < nw; i++) {
		st.frames += w[i].st.frames; st.bytes += w[i].st.bytes; st.bad_magic += w[i].st.bad_magic;
		st.bad_seq += w[i].st.bad_seq; st.bad_payload += w[i].st.bad_payload; st.bad_crc += w[i].st.bad_crc;
		copy_s += w[i].copy_s; verify_s += w[i].verify_s; per[i] = w[i].bursts;
	}
	uint32_t done_bursts = (uint32_t)(st.frames / frames);
	uint64_t exp_crc = (fault == 1 || fault == 4) ? injected : 0;
	uint64_t exp_seq_err = (fault == 2) ? injected : 0;
	uint64_t exp_pay = (fault == 4) ? injected : 0;
	int pass = !dma_fail && done_bursts == bursts && !st.bad_magic && st.bad_crc == exp_crc &&
		   st.bad_seq == exp_seq_err && st.bad_payload == exp_pay;
	double cpu_s = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) * 1e-6 +
		       (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) * 1e-6;
	printf("{\"pass\": %s, \"workers\": %d, \"buffers\": %d, \"pinned\": %d, \"payload_mode\": %d, \"bursts\": %u, \"frames\": %llu, "
	       "\"bytes\": %llu, \"elapsed_s\": %.6f, \"mb_per_s_end_to_end\": %.2f, "
	       "\"start_to_irq_s\": {\"min\": %.6f, \"mean\": %.6f, \"max\": %.6f}, \"dma_blocked_on_buffers_s\": %.4f, "
	       "\"copy_s_total\": %.4f, \"verify_s_total\": %.4f, \"copy_mb_per_s\": %.1f, \"verify_mb_per_s\": %.1f, "
	       "\"cpu_s\": %.4f, \"bursts_per_worker\": [",
	       pass ? "true" : "false", nw, nb, pinned, md0.mode, done_bursts, (unsigned long long)st.frames,
	       (unsigned long long)st.bytes, elapsed, st.bytes / elapsed / 1e6,
	       issued ? lat_min : 0, issued ? lat_sum / issued : 0, lat_max, blocked_s,
	       copy_s, verify_s, copy_s > 0 ? st.bytes / copy_s / 1e6 : 0, verify_s > 0 ? st.bytes / verify_s / 1e6 : 0, cpu_s);
	for (int i = 0; i < nw; i++) printf("%s%u", i ? ", " : "", per[i]);
	printf("], \"bad_magic\": %llu, \"bad_seq\": %llu, \"bad_payload\": %llu, \"bad_crc\": %llu, "
	       "\"faults_injected\": %u, \"dma_fail\": %u, \"fabric_stall_cycles\": %u}\n",
	       (unsigned long long)st.bad_magic, (unsigned long long)st.bad_seq, (unsigned long long)st.bad_payload,
	       (unsigned long long)st.bad_crc, injected, dma_fail, stall);
	return pass ? 0 : 1;
}
