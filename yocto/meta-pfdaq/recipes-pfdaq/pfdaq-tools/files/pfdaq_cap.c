// SPDX-License-Identifier: MIT
/*
 * pfdaq_cap - Linux consumer for the pfdaq fabric stream (PolarFire SoC Discovery Kit).
 *
 * Starts bursts on the fabric frame source, receives each burst through the
 * CoreAXI4DMAController AXI4-Stream bridge into non-cached DDR, and verifies every frame
 * (magic, sequence continuity, payload pattern, CRC-32). Two buffers are used alternately:
 * the next burst is started before the previous one is verified.
 *
 * Devices (found by name, nothing hard-coded except register offsets):
 *   uio "pfdaq_stream_src"          stream source registers (page offset 0xF00)
 *   uio "dma-controller@60010000"   DMA registers and completion interrupt
 *   /dev/udmabuf-ddr-nc0            non-cached DDR shared with the fabric (descriptor + data)
 *
 * Optional supervision (-r): every verified burst is reported over RPMsg to the FreeRTOS
 * supervisor on the fourth hart (pfdaq_supervisor.c), which checks a deadline and compares the
 * reported sequence number with the fabric's own counter. Message layout is shared with it.
 *
 * Rule learnt on the board: the stream descriptor must be armed before the packet starts,
 * so each burst is: write descriptor, write stream address register, START.
 *
 * Fabric v0.5 adds (needs VERSION >= 0x00050000):
 *   -m counter|prbs|fir   payload mode; FIR uses -S step and the default coefficient set
 *   -T rate               real-time mode: one frame slot every `rate` fabric cycles (8 ns),
 *                         -n slots, each frame its own packet; up to -d 4 stream descriptors are
 *                         kept armed and one credit is returned per re-armed descriptor. Frames
 *                         that find no credit or a busy source are dropped by the fabric and
 *                         show up as sequence gaps, reconciled against the DROPS counter.
 *   -A us                 graceful abort test: start one burst, ABORT after `us` microseconds,
 *                         check the shortened packet and the DMA completion.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/rpmsg.h>

#define SRC_OFF        0xF00u
#define SRC_ID         0x00
#define SRC_VERSION    0x04
#define SRC_CTRL       0x08
#define SRC_PAYLOAD    0x0C
#define SRC_FRAMES     0x10
#define SRC_GAP        0x14
#define SRC_FAULT      0x18
#define SRC_STATUS     0x1C
#define SRC_SNAP_FRAMES 0x20
#define SRC_SNAP_STALL 0x28
#define SRC_SNAP_SEQ   0x30
#define SRC_MODE       0x34
#define SRC_RATE       0x38
#define SRC_CREDIT     0x3C
#define SRC_DROPS      0x40
#define SRC_FIR_STEP   0x44
#define SRC_COEF0      0x48
#define SRC_ID_VALUE   0x50464451u

#define DMA_STAT       0x10
#define DMA_MASK       0x14
#define DMA_CLEAR      0x18
#define DMA_EXT_ADDR   0x1C
#define DMA_STREAM0    0x460

#define DESC_CFG_ARMED 0xDu           /* valid | destination data ready | incrementing */
#define DESC_OFF       0x0u
#define BUF_OFF        0x1000u
#define MAGIC_HI       0xA55Au
#define MAX_BURST      (8u << 20)      /* core limit for one stream transaction */
#define RT_DESC_STRIDE 0x40u           /* real-time descriptors: 64 bytes apart, 8-byte aligned */


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

/* ---- RPMsg link to the FreeRTOS supervisor (same flow as the vendor rpmsg-pingpong example) ---- */
#define SUP_CHANNEL   "pfdaq-supervisor"
#define SUP_MSG_MAGIC 0x53444650u
#define SUP_ACK_MAGIC 0x41444650u
enum { SUP_ARM = 1, SUP_STATUS = 2, SUP_DISARM = 3, SUP_QUERY = 4, SUP_LATMODE = 5, SUP_MEMCHK = 6 };

struct sup_msg { uint32_t magic, type, burst, next_seq, errors, deadline_ms, max_lag; };
struct sup_ack { uint32_t magic, type, state, status_rx, deadline_miss, seq_mismatch, fabric_seq, fabric_frames, uptime_ms; };

static int sup_fd = -1;
static double rtt_min = 1e9, rtt_max, rtt_sum;
static unsigned rtt_n, sup_fail;

static int write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY), r;
	if (fd < 0)
		return -1;
	r = write(fd, val, strlen(val) + 1) < 0 ? -1 : 0;
	close(fd);
	return r;
}

static int sup_open(void)
{
	char dev[128] = "", path[512], val[128];
	struct rpmsg_endpoint_info ept = { .name = SUP_CHANNEL, .src = 0, .dst = 0 };
	struct dirent *e;
	DIR *d = opendir("/sys/bus/rpmsg/devices");
	int ctrl = -1;

	if (!d)
		return -1;
	while ((e = readdir(d)))
		if (strstr(e->d_name, SUP_CHANNEL))
			snprintf(dev, sizeof(dev), "%s", e->d_name);
	closedir(d);
	if (!dev[0]) { errno = ENODEV; return -1; }
	ept.dst = strtoul(strrchr(dev, '.') + 1, NULL, 10);

	snprintf(path, sizeof(path), "/sys/bus/rpmsg/devices/%s/driver_override", dev);
	write_str(path, "rpmsg_chrdev");
	write_str("/sys/bus/rpmsg/drivers/rpmsg_chrdev/bind", dev);   /* fails harmlessly if already bound */

	/*
	 * Two kernel behaviours exist. On this board (Linux 6.18) binding the channel to
	 * rpmsg_chrdev directly creates /dev/rpmsgN for it. Older flows (vendor rpmsg-pingpong
	 * example) expose an rpmsg_ctrl device on which the endpoint has to be created first.
	 */
	snprintf(path, sizeof(path), "/sys/bus/rpmsg/devices/%s/rpmsg", dev);
	for (int tries = 0; tries < 50 && !(d = opendir(path)); tries++)
		usleep(20000);          /* the chrdev appears shortly after the bind */
	if (!d)
		return -1;
	val[0] = 0;
	while ((e = readdir(d))) {
		if (!strncmp(e->d_name, "rpmsg_ctrl", 10)) {
			snprintf(path, sizeof(path), "/dev/%s", e->d_name);
			ctrl = open(path, O_RDWR);
		} else if (!strncmp(e->d_name, "rpmsg", 5)) {
			snprintf(val, sizeof(val), "/dev/%s", e->d_name);
		}
	}
	closedir(d);
	if (val[0]) {
		if (ctrl >= 0)
			close(ctrl);
		return open(val, O_RDWR);
	}
	if (ctrl < 0) { errno = ENOENT; return -1; }
	if (ioctl(ctrl, RPMSG_CREATE_EPT_IOCTL, &ept) < 0) { close(ctrl); return -1; }
	close(ctrl);
	for (int i = 127; i >= 0; i--) {
		snprintf(path, sizeof(path), "/sys/class/rpmsg/rpmsg%d/name", i);
		if (!read_str(path, val, sizeof(val)) && !strcmp(val, SUP_CHANNEL)) {
			snprintf(path, sizeof(path), "/dev/rpmsg%d", i);
			return open(path, O_RDWR);
		}
	}
	errno = ENOENT;
	return -1;
}

/* send one message and wait for its acknowledgement; returns 0 and fills *ack on success */
static int sup_xfer(uint32_t type, uint32_t burst, uint32_t next_seq, uint32_t errors,
		    uint32_t deadline_ms, uint32_t max_lag, struct sup_ack *ack)
{
	struct sup_msg m = { SUP_MSG_MAGIC, type, burst, next_seq, errors, deadline_ms, max_lag };
	struct pollfd p = { .fd = sup_fd, .events = POLLIN };
	double t0 = now_s();

	if (sup_fd < 0)
		return -1;
	if (write(sup_fd, &m, sizeof(m)) != sizeof(m) || poll(&p, 1, 1000) <= 0 ||
	    read(sup_fd, ack, sizeof(*ack)) != sizeof(*ack) || ack->magic != SUP_ACK_MAGIC || ack->type != type) {
		sup_fail++;
		return -1;
	}
	double l = now_s() - t0;
	rtt_sum += l; rtt_n++;
	if (l < rtt_min) rtt_min = l;
	if (l > rtt_max) rtt_max = l;
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

/*
 * Shared-memory visibility test between Linux (U54_1-3) and the context on U54_4.
 * Linux writes an xorshift32 pattern, U54_4 verifies it and writes the inverse back, Linux
 * verifies the inverse. Non-cached: /dev/udmabuf-ddr-nc0 (both sides use the non-cached DDR
 * window). Cached: /dev/udmabuf-ddr-c0 mapped cacheable on Linux, accessed through the cached
 * window on U54_4, with no cache maintenance on either side (tests hart-to-hart coherence).
 */
static int run_shm(uint32_t words, uint32_t iters, int cached)
{
	const char *dev = cached ? "udmabuf-ddr-c0" : "udmabuf-ddr-nc0";
	char path[128], buf[64];
	uint64_t phys;
	uint32_t off = cached ? 0x100000u : 0x1F00000u;   /* away from the capture buffers */
	struct sup_ack ack;
	unsigned bad_rtos = 0, bad_linux = 0, fail = 0;
	double t_total = 0;

	snprintf(path, sizeof(path), "/sys/class/u-dma-buf/%s/phys_addr", dev);
	if (read_str(path, buf, sizeof(buf))) { perror(path); return 1; }
	phys = strtoull(buf, NULL, 0);
	snprintf(path, sizeof(path), "/dev/%s", dev);
	int fd = open(path, cached ? O_RDWR : O_RDWR | O_SYNC);
	if (fd < 0) { perror(path); return 1; }
	volatile uint32_t *m = mmap(NULL, 0x2000000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	m += off / 4;
	sup_fd = sup_open();
	if (sup_fd < 0) { perror("supervisor RPMsg endpoint"); return 1; }
	for (uint32_t it = 0; it < iters; it++) {
		uint32_t seed = 0x1234567u + it * 0x9E3779B9u, x = seed ? seed : 1;
		for (uint32_t i = 0; i < words; i++) { x = xs32(x); m[i] = x; }
		__sync_synchronize();
		double t0 = now_s();
		if (sup_xfer(SUP_MEMCHK, (uint32_t)(phys + off), words, seed, 0, 0, &ack)) { fail++; continue; }
		t_total += now_s() - t0;
		bad_rtos += ack.fabric_seq;
		x = seed ? seed : 1;
		for (uint32_t i = 0; i < words; i++) { x = xs32(x); if (m[i] != ~x) bad_linux++; }
	}
	close(sup_fd);
	int pass = !fail && !bad_rtos && !bad_linux;
	printf("{\"pass\": %s, \"mode\": \"shm\", \"region\": \"%s\", \"cached\": %s, \"phys\": \"0x%llx\", \"words\": %u, "
	       "\"iterations\": %u, \"mismatch_seen_by_u54_4\": %u, \"mismatch_seen_by_linux\": %u, \"rpmsg_fail\": %u, "
	       "\"mean_check_ms\": %.3f, \"context\": \"%s\"}\n",
	       pass ? "true" : "false", dev, cached ? "true" : "false", (unsigned long long)(phys + off), words, iters,
	       bad_rtos, bad_linux, fail, iters ? t_total / iters * 1e3 : 0, (ack.uptime_ms & 0x80000000u) ? "bare-metal" : "freertos");
	return pass ? 0 : 1;
}

static int set_latmode(int mode)
{
	struct sup_ack ack;
	sup_fd = sup_open();
	if (sup_fd < 0) { perror("supervisor RPMsg endpoint"); return 1; }
	int r = sup_xfer(SUP_LATMODE, 0, 0, 0, (uint32_t)mode, 0, &ack);
	close(sup_fd);
	printf("{\"pass\": %s, \"mode\": \"latmode\", \"requested\": %d, \"context\": \"%s\"}\n", r ? "false" : "true",
	       mode, (ack.uptime_ms & 0x80000000u) ? "bare-metal" : "freertos");
	return r ? 1 : 0;
}

static void src_snapshot(void)
{
	src[SRC_CTRL / 4] = 0x2;
	usleep(200);                     /* a few fabric cycles are enough; be generous */
}

static void fence_io(void)
{
	__sync_synchronize();
}

static unsigned uio_events(const char *uio_name)
{
	char path[300], val[64], name[128];
	DIR *d = opendir("/sys/class/uio");
	struct dirent *e;
	unsigned v = 0;
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "uio", 3)) continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
		if (read_str(path, name, sizeof(name)) || strcmp(name, uio_name)) continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/event", e->d_name);
		if (!read_str(path, val, sizeof(val))) v = strtoul(val, NULL, 0);
	}
	if (d) closedir(d);
	return v;
}

static void arm_rt(int k, uint32_t buf_off, uint32_t nbytes)
{
	volatile uint32_t *desc = (volatile uint32_t *)(ddr + DESC_OFF + k * RT_DESC_STRIDE);
	desc[1] = nbytes;
	desc[2] = (uint32_t)(ddr_phys + buf_off);
	fence_io();
	desc[0] = DESC_CFG_ARMED;
	fence_io();
	dma[DMA_STREAM0 / 4 + k] = (uint32_t)(ddr_phys + DESC_OFF + k * RT_DESC_STRIDE);
	fence_io();
}

/*
 * Real-time capture: frames arrive on their own schedule, one packet each, rotating over
 * ndest descriptors. Frame n lands in buffer n % ndest; the UIO event count says how many
 * have completed. After a frame is copied out its descriptor is re-armed and one credit is
 * handed back, so the fabric can never send into a descriptor that is not armed.
 */
static int run_realtime(uint32_t payload, uint32_t slots, uint32_t rate, int ndest,
			struct model *md, int timeout_ms, uint32_t fault, uint32_t fault_frame)
{
	uint32_t flen = payload + 4, fbytes = flen * 4;
	uint32_t slot_sz = (fbytes + 0xFFF) & ~0xFFFu;
	uint32_t *copy = malloc(fbytes);
	struct stats st = { 0 };
	unsigned processed = 0, base, ev;
	int timed_out = 0;

	if (!copy || BUF_OFF + ndest * slot_sz > ddr_size)
		return 2;
	src_snapshot();
	uint32_t seq0 = src[SRC_SNAP_SEQ / 4], frames0 = src[SRC_SNAP_FRAMES / 4], drops0 = src[SRC_DROPS / 4];
	uint32_t exp_seq = seq0;
	src[SRC_PAYLOAD / 4] = payload;
	src[SRC_FRAMES / 4] = slots;
	src[SRC_RATE / 4] = rate;
	src[SRC_FAULT / 4] = fault ? ((fault_frame << 16) | fault) : 0;
	src[SRC_MODE / 4] = (uint32_t)md->mode | (1u << 4) | ((uint32_t)(ndest - 1) << 8);
	for (int k = 0; k < ndest; k++)
		arm_rt(k, BUF_OFF + k * slot_sz, fbytes);
	src[SRC_CREDIT / 4] = 0x80000000u | (uint32_t)ndest;
	model_restart(md);
	base = uio_events("dma-controller@60010000");
	double t0 = now_s(), last_progress = t0;
	fence_io();
	src[SRC_CTRL / 4] = 0x1;

	struct pollfd p = { .fd = dma_fd, .events = POLLIN };
	for (;;) {
		uint32_t cnt;
		if (poll(&p, 1, 5) > 0 && read(dma_fd, &cnt, 4) == 4)
			ev = cnt - base;
		else
			ev = uio_events("dma-controller@60010000") - base;
		while (processed < ev) {
			int k = processed % ndest;
			memcpy(copy, ddr + BUF_OFF + k * slot_sz, fbytes);
			arm_rt(k, BUF_OFF + k * slot_sz, fbytes);
			src[SRC_CREDIT / 4] = 1;
			verify(copy, 1, payload, &exp_seq, &st, md, 1);
			processed++;
			last_progress = now_s();
		}
		uint32_t status = src[SRC_STATUS / 4];
		if ((status & 2) && !(status & 1)) {
			src_snapshot();
			if (src[SRC_SNAP_FRAMES / 4] - frames0 == processed)
				break;
		}
		if (now_s() - last_progress > timeout_ms / 1000.0) { timed_out = 1; break; }
	}
	double elapsed = now_s() - t0;
	src_snapshot();
	uint32_t sent = src[SRC_SNAP_FRAMES / 4] - frames0, drops = src[SRC_DROPS / 4] - drops0;
	uint32_t next_seq = src[SRC_SNAP_SEQ / 4];
	/* every slot is either sent or dropped, and every drop is a sequence gap or trailing */
	int consistent = sent + drops == slots && st.seq_gap <= drops && next_seq == seq0 + slots +
			 ((fault & 2) && fault_frame < sent ? 1 : 0);
	uint64_t exp_crc = (fault & 5) && fault_frame < sent ? 1 : 0, exp_pay = (fault & 4) && fault_frame < sent ? 1 : 0;
	uint64_t exp_seq_err = (fault & 2) && fault_frame < sent ? 1 : 0;
	/* gaps = drops between sent frames (+1 if a sequence skip was injected); drops after the
	   last sent frame only show in NEXT_SEQ, which `consistent` already checks */
	int pass = !timed_out && consistent && processed == sent && !st.bad_magic && !st.bad_seq &&
		   st.bad_crc == exp_crc && st.bad_payload == exp_pay && st.seq_gap <= drops + exp_seq_err;
	double slot_s = rate * 8e-9;
	printf("{\"pass\": %s, \"mode\": \"realtime\", \"payload_mode\": %d, \"rate_cycles\": %u, \"slot_us\": %.3f, "
	       "\"frame_bytes\": %u, \"offered_mb_per_s\": %.2f, \"destinations\": %d, \"slots\": %u, \"sent\": %u, "
	       "\"dropped\": %u, \"drop_pct\": %.2f, \"verified\": %u, \"seq_gaps\": %llu, \"elapsed_s\": %.3f, "
	       "\"delivered_mb_per_s\": %.2f, \"bad_magic\": %llu, \"bad_seq\": %llu, \"bad_payload\": %llu, "
	       "\"bad_crc\": %llu, \"timed_out\": %d}\n",
	       pass ? "true" : "false", md->mode, rate, slot_s * 1e6, fbytes, fbytes / slot_s / 1e6, ndest, slots,
	       sent, drops, slots ? 100.0 * drops / slots : 0, processed, (unsigned long long)st.seq_gap, elapsed,
	       st.bytes / elapsed / 1e6, (unsigned long long)st.bad_magic, (unsigned long long)st.bad_seq,
	       (unsigned long long)st.bad_payload, (unsigned long long)st.bad_crc, timed_out);
	return pass ? 0 : 1;
}

/*
 * Graceful-abort test: one long burst, ABORT after abort_us. The fabric finishes the frame in
 * progress and then sends zero words up to the burst's full length (fabric v0.5: this DMA core
 * completes a stream transfer only on its full byte count, not on an early TLAST). Check that
 * the DMA completes, that FRAMES_SENT frames arrived intact, that the rest of the burst is zero,
 * and that nothing was written past the burst.
 */
static int run_abort(uint32_t payload, uint32_t frames, unsigned abort_us, struct model *md, int timeout_ms)
{
	uint32_t flen = payload + 4;
	uint64_t burst_bytes = (uint64_t)flen * 4 * frames;
	struct stats st = { 0 };
	uint32_t stat = 0;

	if (BUF_OFF + burst_bytes + 0x1000 > ddr_size)
		return 2;
	src_snapshot();
	uint32_t seq0 = src[SRC_SNAP_SEQ / 4], frames0 = src[SRC_SNAP_FRAMES / 4], exp_seq = seq0;
	src[SRC_PAYLOAD / 4] = payload;
	src[SRC_FRAMES / 4] = frames;
	src[SRC_FAULT / 4] = 0;
	src[SRC_MODE / 4] = (uint32_t)md->mode;
	memset(ddr + BUF_OFF, 0xEE, burst_bytes + 64);
	arm(BUF_OFF, (uint32_t)burst_bytes);
	fence_io();
	double t0 = now_s();
	src[SRC_CTRL / 4] = 0x1;
	usleep(abort_us);
	src[SRC_CTRL / 4] = 0x10;
	int dma_ok = !wait_done(timeout_ms, &stat);
	double t1 = now_s();
	usleep(1000);
	src_snapshot();
	uint32_t status = src[SRC_STATUS / 4];
	uint32_t sent = src[SRC_SNAP_FRAMES / 4] - frames0;
	model_restart(md);
	uint32_t *copy = malloc(burst_bytes + 64);
	memcpy(copy, ddr + BUF_OFF, burst_bytes + 64);
	if (sent > frames) sent = frames;
	verify(copy, sent, payload, &exp_seq, &st, md, 0);
	uint64_t fill_words = (burst_bytes / 4) - (uint64_t)sent * flen, fill_bad = 0;
	for (uint64_t i = (uint64_t)sent * flen; i < burst_bytes / 4; i++)
		if (copy[i]) fill_bad++;
	const uint8_t *after = (const uint8_t *)copy + burst_bytes;
	int untouched = 1;
	for (int i = 0; i < 64; i++)
		if (after[i] != 0xEE) untouched = 0;
	int pass = dma_ok && (status & 4) && sent < frames && !st.bad_magic && !st.bad_seq &&
		   !st.bad_payload && !st.bad_crc && !fill_bad && untouched;
	printf("{\"pass\": %s, \"mode\": \"abort\", \"payload_mode\": %d, \"frames_requested\": %u, \"abort_after_us\": %u, "
	       "\"dma_completed\": %s, \"dma_stat\": \"%#x\", \"start_to_irq_s\": %.6f, \"status\": \"%#x\", \"aborted_flag\": %s, "
	       "\"frames_sent\": %u, \"bytes\": %llu, \"bad_magic\": %llu, \"bad_seq\": %llu, \"bad_payload\": %llu, "
	       "\"bad_crc\": %llu, \"fill_words\": %llu, \"fill_nonzero\": %llu, \"nothing_after_burst\": %s}\n",
	       pass ? "true" : "false", md->mode, frames, abort_us, dma_ok ? "true" : "false", stat, t1 - t0, status,
	       (status & 4) ? "true" : "false", sent, (unsigned long long)sent * flen * 4,
	       (unsigned long long)st.bad_magic, (unsigned long long)st.bad_seq, (unsigned long long)st.bad_payload,
	       (unsigned long long)st.bad_crc, (unsigned long long)fill_words, (unsigned long long)fill_bad,
	       untouched ? "true" : "false");
	return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
	uint32_t payload = 252, frames = 1024, bursts = 16, fault = 0, fault_frame = 0, fault_every = 0;
	int timeout_ms = 2000, opt, serial = 0, use_sup = 0, ndest = 4;
	uint32_t rt_rate = 0, abort_us = 0, shm_words = 0;
	int latmode = -1, shm_cached = 0, allow_odd = 0;
	struct model md = { .mode = 0, .step = 256 };
	memcpy(md.coef, (int32_t[8]){ 12000, -15000, 20000, 32767, -32768, 20000, -15000, 12000 }, sizeof(md.coef));
	uint32_t deadline_ms = 500, pause_burst = 0xFFFFFFFFu, pause_ms = 0;
	struct sup_ack ack = { 0 };
	void *m;
	char buf[64];

	while ((opt = getopt(argc, argv, "w:f:n:F:i:e:t:srD:P:m:S:T:d:A:L:X:cOh")) != -1) {
		switch (opt) {
		case 'w': payload = strtoul(optarg, NULL, 0); break;
		case 'f': frames = strtoul(optarg, NULL, 0); break;
		case 'n': bursts = strtoul(optarg, NULL, 0); break;
		case 'F': fault = !strcmp(optarg, "crc") ? 1 : !strcmp(optarg, "seq") ? 2 : !strcmp(optarg, "payload") ? 4 : 0; break;
		case 'i': fault_frame = strtoul(optarg, NULL, 0); break;
		case 'e': fault_every = strtoul(optarg, NULL, 0); break;
		case 't': timeout_ms = atoi(optarg); break;
		case 's': serial = 1; break;
		case 'r': use_sup = 1; break;
		case 'D': deadline_ms = strtoul(optarg, NULL, 0); break;
		case 'P': if (sscanf(optarg, "%u:%u", &pause_burst, &pause_ms) != 2) return 2; break;
		case 'm': md.mode = !strcmp(optarg, "prbs") ? 1 : !strcmp(optarg, "fir") ? 2 : 0; break;
		case 'S': md.step = strtoul(optarg, NULL, 0) & 0xFFFF; break;
		case 'T': rt_rate = strtoul(optarg, NULL, 0); break;
		case 'd': ndest = atoi(optarg); if (ndest < 1 || ndest > 4) return 2; break;
		case 'A': abort_us = strtoul(optarg, NULL, 0); break;
		case 'L': latmode = atoi(optarg); break;
		case 'X': shm_words = strtoul(optarg, NULL, 0); break;
		case 'c': shm_cached = 1; break;
		case 'O': allow_odd = 1; break;
		default:
			fprintf(stderr, "usage: %s [-w payload_words] [-f frames_per_burst] [-n bursts]\n"
				"          [-F crc|seq|payload -i frame_index -e every_nth_burst] [-t timeout_ms] [-s]\n"
				"  -s  serial mode: verify each burst before starting the next (clean DMA timing)\n"
				"  -r  report to the FreeRTOS supervisor over RPMsg; -D deadline_ms (default 500)\n"
				"  -P burst:ms  stall for ms before reporting that burst (supervisor deadline test)\n"
				"  -m counter|prbs|fir  payload mode (fabric v0.5); -S step  FIR triangle phase step\n"
				"  -T rate  real-time mode, one frame slot every rate cycles (8 ns), -n slots, -d destinations 1-4\n"
				"  -A us    graceful abort test: one burst of -f frames, ABORT after us microseconds\n"
				"  -L 0|1   U54_4 latency channel: 0 interrupt handler acknowledges, 1 FreeRTOS task acknowledges\n"
				"  -X words shared-memory test with U54_4, -n iterations, -c cached region\n"
				"  -O       allow an odd frame length (payload+4 words) in multi-frame bursts: known to\n"
				"           corrupt data on this board, for characterisation only\n", argv[0]);
			return 2;
		}
	}
	if (latmode >= 0)
		return set_latmode(latmode);
	if (shm_words)
		return run_shm(shm_words > 0x40000 ? 0x40000 : shm_words, bursts, shm_cached);

	uint32_t flen = payload + 4;
	uint64_t burst_bytes = (uint64_t)flen * 4 * frames;
	/*
	 * Found on the board (fabric v0.5.1): a burst of several frames whose length is an odd
	 * number of 32-bit words loses or misplaces 2 KB chunks at 2/4 KB destination boundaries,
	 * although the stream is correct in simulation and the DMA reports completion. Even frame
	 * lengths are clean. Refuse the odd case unless asked for explicitly.
	 */
	if (((payload + 4) & 1) && frames > 1 && !rt_rate && !allow_odd) {
		fprintf(stderr, "odd frame length (%u words) in a multi-frame burst is a known data-corruption case; "
			"use an even payload length, or -O to run it anyway\n", payload + 4);
		return 2;
	}
	if (!payload || payload > 65535 || !frames || burst_bytes > MAX_BURST) {
		fprintf(stderr, "burst of %llu bytes is outside 1..%u\n", (unsigned long long)burst_bytes, MAX_BURST);
		return 2;
	}

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
	uint32_t buf_off[2] = { BUF_OFF, BUF_OFF + (uint32_t)((burst_bytes + 0xFFF) & ~0xFFFull) + 0x1000 };
	if (buf_off[1] + burst_bytes + 0x1000 > ddr_size) { fprintf(stderr, "two bursts do not fit the %zu byte buffer\n", ddr_size); return 2; }

	if (src[SRC_ID / 4] != SRC_ID_VALUE) { fprintf(stderr, "stream source ID %08x\n", src[SRC_ID / 4]); return 1; }
	if (src[SRC_STATUS / 4] & 1) { fprintf(stderr, "stream source busy at start\n"); return 1; }
	uint32_t version = src[SRC_VERSION / 4];
	if ((md.mode || rt_rate || abort_us) && version < 0x00050000) {
		fprintf(stderr, "fabric version %08x has no payload modes, real-time mode or graceful abort\n", version);
		return 1;
	}
	if (version >= 0x00050000) {
		src[SRC_MODE / 4] = (uint32_t)md.mode;
		src[SRC_FIR_STEP / 4] = md.step;
		for (int k = 0; k < 8; k++)
			src[SRC_COEF0 / 4 + k] = (uint16_t)md.coef[k];
	}

	crc_init();
	dma[DMA_MASK / 4] = 0xF;
	dma[DMA_CLEAR / 4] = 0xF;
	{   /* drop completions left over from an earlier run */
		struct pollfd p = { .fd = dma_fd, .events = POLLIN };
		uint32_t cnt;
		while (poll(&p, 1, 10) > 0 && read(dma_fd, &cnt, 4) == 4)
			;
	}
	if (rt_rate)
		return run_realtime(payload, bursts, rt_rate, ndest, &md, timeout_ms, fault, fault_frame);
	if (abort_us)
		return run_abort(payload, frames, abort_us, &md, timeout_ms);

	src[SRC_CTRL / 4] = 0x2;
	usleep(1000);
	uint32_t exp_seq = src[SRC_SNAP_SEQ / 4], stall0 = src[SRC_SNAP_STALL / 4];
	src[SRC_PAYLOAD / 4] = payload;
	src[SRC_FRAMES / 4] = frames;
	src[SRC_GAP / 4] = 0;

	/* non-cached DDR is slow to read: copy each burst once into normal memory, verify the copy */
	uint32_t *copy = malloc(burst_bytes);
	if (!copy) { perror("malloc"); return 1; }
	double copy_s = 0, verify_s = 0;

	struct stats st = { 0 };
	if (use_sup) {
		sup_fd = sup_open();
		if (sup_fd < 0) { perror("supervisor RPMsg endpoint"); return 1; }
		if (sup_xfer(SUP_ARM, 0, exp_seq, 0, deadline_ms, frames + 16, &ack)) { fprintf(stderr, "supervisor did not acknowledge ARM\n"); return 1; }
	}
	uint32_t injected = 0, dma_fail = 0, stat = 0;
	double lat_min = 1e9, lat_max = 0, lat_sum = 0, t_start = now_s(), t0 = 0;
	int pending = -1;       /* buffer index of a burst that is complete but not yet verified */

	for (uint32_t b = 0; b <= bursts; b++) {
		int k = b & 1;
		if (b < bursts) {
			int inj = fault && fault_every && (b % fault_every == fault_every - 1);
			src[SRC_FAULT / 4] = inj ? ((fault_frame << 16) | fault) : 0;
			injected += inj;
			memset(ddr + buf_off[k], 0xEE, 64);      /* stale-data marker */
			arm(buf_off[k], (uint32_t)burst_bytes);
			t0 = now_s();
			src[SRC_CTRL / 4] = 0x1;
		}
		/* default: verify the previous burst while the fabric fills the other buffer */
		if (pending >= 0 && !serial) {
			double ta = now_s();
			memcpy(copy, ddr + buf_off[pending], burst_bytes);
			double tb = now_s();
			model_restart(&md);
			verify(copy, frames, payload, &exp_seq, &st, &md, 0);
			copy_s += tb - ta;
			verify_s += now_s() - tb;
			pending = -1;
			if (use_sup) {
				uint32_t done = (uint32_t)(st.frames / frames) - 1;
				if (done == pause_burst)
					usleep(pause_ms * 1000);
				sup_xfer(SUP_STATUS, done, exp_seq, (uint32_t)(st.bad_magic + st.bad_seq + st.bad_payload + st.bad_crc), 0, 0, &ack);
			}
		}
		if (b == bursts)
			break;
		if (wait_done(timeout_ms, &stat)) {
			dma_fail++;
			fprintf(stderr, "burst %u: DMA timeout or error, stat=%#x\n", b, stat);
			break;
		}
		double l = now_s() - t0;
		lat_sum += l;
		if (l < lat_min) lat_min = l;
		if (l > lat_max) lat_max = l;
		pending = k;
		if (serial) {
			double ta = now_s();
			memcpy(copy, ddr + buf_off[k], burst_bytes);
			double tb = now_s();
			model_restart(&md);
			verify(copy, frames, payload, &exp_seq, &st, &md, 0);
			copy_s += tb - ta;
			verify_s += now_s() - tb;
			pending = -1;
			if (use_sup) {
				if (b == pause_burst)
					usleep(pause_ms * 1000);
				sup_xfer(SUP_STATUS, b, exp_seq, (uint32_t)(st.bad_magic + st.bad_seq + st.bad_payload + st.bad_crc), 0, 0, &ack);
			}
		}
	}
	double elapsed = now_s() - t_start;
	usleep(1000);
	src[SRC_CTRL / 4] = 0x2;
	usleep(1000);
	uint32_t stall = src[SRC_SNAP_STALL / 4] - stall0;
	uint32_t done_bursts = (uint32_t)(st.frames / frames);

	/* a payload fault is reported twice (payload + crc); a seq fault once; a crc fault once */
	uint64_t exp_crc = (fault == 1 || fault == 4) ? injected : 0;
	uint64_t exp_seq_err = (fault == 2) ? injected : 0;
	uint64_t exp_pay = (fault == 4) ? injected : 0;
	int pass = !dma_fail && done_bursts == bursts && !st.bad_magic && st.bad_crc == exp_crc &&
		   st.bad_seq == exp_seq_err && st.bad_payload == exp_pay;
	char sup_json[400] = "";
	if (use_sup) {
		uint32_t errs = (uint32_t)(st.bad_magic + st.bad_seq + st.bad_payload + st.bad_crc);
		int ok = !sup_xfer(SUP_DISARM, done_bursts, exp_seq, errs, 0, 0, &ack);
		/* expected: every status acknowledged, no sequence mismatch, a deadline miss only if we stalled on purpose */
		int sup_pass = ok && !sup_fail && ack.status_rx == done_bursts && ack.seq_mismatch == 0 &&
			       ((pause_ms > deadline_ms && pause_burst < bursts) ? ack.deadline_miss >= 1 : ack.deadline_miss == 0);
		snprintf(sup_json, sizeof(sup_json),
			 ", \"supervisor\": {\"pass\": %s, \"status_rx\": %u, \"deadline_miss\": %u, \"seq_mismatch\": %u, "
			 "\"fabric_seq\": %u, \"fabric_frames\": %u, \"xfer_fail\": %u, \"uptime_ms\": %u, "
			 "\"rtt_us\": {\"min\": %.0f, \"mean\": %.0f, \"max\": %.0f}}",
			 sup_pass ? "true" : "false", ack.status_rx, ack.deadline_miss, ack.seq_mismatch, ack.fabric_seq,
			 ack.fabric_frames, sup_fail, ack.uptime_ms, rtt_n ? rtt_min * 1e6 : 0, rtt_n ? rtt_sum / rtt_n * 1e6 : 0, rtt_max * 1e6);
		pass = pass && sup_pass;
		close(sup_fd);
	}

	printf("{\"pass\": %s, \"bursts\": %u, \"frames\": %llu, \"bytes\": %llu, \"elapsed_s\": %.6f, "
	       "\"mb_per_s_end_to_end\": %.2f, \"serial\": %d, \"start_to_irq_s\": {\"min\": %.6f, \"mean\": %.6f, \"max\": %.6f}, "
	       "\"copy_mb_per_s\": %.1f, \"verify_mb_per_s\": %.1f, "
	       "\"bad_magic\": %llu, \"bad_seq\": %llu, \"bad_payload\": %llu, \"bad_crc\": %llu, "
	       "\"faults_injected\": %u, \"dma_fail\": %u, \"fabric_stall_cycles\": %u, \"next_seq\": %u%s}\n",
	       pass ? "true" : "false", done_bursts, (unsigned long long)st.frames, (unsigned long long)st.bytes,
	       elapsed, st.bytes / elapsed / 1e6, serial, done_bursts ? lat_min : 0, done_bursts ? lat_sum / done_bursts : 0,
	       lat_max, copy_s > 0 ? st.bytes / copy_s / 1e6 : 0, verify_s > 0 ? st.bytes / verify_s / 1e6 : 0,
	       (unsigned long long)st.bad_magic, (unsigned long long)st.bad_seq,
	       (unsigned long long)st.bad_payload, (unsigned long long)st.bad_crc, injected, dma_fail, stall, exp_seq, sup_json);
	return pass ? 0 : 1;
}
