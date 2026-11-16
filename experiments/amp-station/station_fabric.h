/* SPDX-License-Identifier: MIT
 *
 * station_fabric.h - the data source seen by the acquisition process.
 *
 *   fabric_hw   the real thing: the pfdaq stream source and the DMA controller through UIO, four
 *               stream descriptors in the non-cached DDR window, credits. The register offsets,
 *               arm_rt() and the loop in next() are the ones of run_realtime() in pfdaq_cap.c
 *               (yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c), verified on the board
 *               (module I07); the difference is that a finished frame is copied straight into a
 *               ring slot and that runs follow each other.
 *   fabric_sim  a software stand-in for the PC: frames are generated at the slot rate, the abort
 *               request comes through a shared-memory word that the stand-in for hart 4 writes.
 *
 * Both implement struct fabric, so acquisition has one loop for both.
 */
#ifndef STATION_FABRIC_H
#define STATION_FABRIC_H

#include "station_common.h"
#include <dirent.h>
#include <poll.h>

struct run_cfg {
	uint32_t slots;             /* frame slots in this run */
	uint32_t rate_cycles;       /* slot interval in 8 ns fabric cycles */
	uint32_t fault;             /* FAULT register bits: 1 corrupt CRC */
	uint32_t fault_frame;       /* index of the frame (within the run) the fault applies to */
};

struct fabric {
	int (*start_run)(struct fabric *f, const struct run_cfg *c);
	/* 1: one frame delivered (copied to dst unless dst is NULL; its sequence number in *seq),
	   0: nothing within timeout_ms, 2: the run ended, -1: error */
	int (*next)(struct fabric *f, uint32_t *dst, uint32_t *seq, int timeout_ms);
	void (*abort_run)(struct fabric *f);
	int (*ended_by_abort)(struct fabric *f);
	uint32_t (*next_seq)(struct fabric *f);      /* sequence number the next slot will carry */
	uint32_t (*drops)(struct fabric *f);         /* fabric DROPS counter since reset */
	void (*close)(struct fabric *f);
};

/* ------------------------------------------------------------------------------------------ */
/* hardware                                                                                      */
/* ------------------------------------------------------------------------------------------ */
#define SRC_OFF        0xF00u
#define SRC_ID         0x00
#define SRC_VERSION    0x04
#define SRC_CTRL       0x08
#define SRC_PAYLOAD    0x0C
#define SRC_FRAMES     0x10
#define SRC_FAULT      0x18
#define SRC_STATUS     0x1C
#define SRC_SNAP_FRAMES 0x20
#define SRC_SNAP_SEQ   0x30
#define SRC_MODE       0x34
#define SRC_RATE       0x38
#define SRC_CREDIT     0x3C
#define SRC_DROPS      0x40
#define SRC_FIR_STEP   0x44
#define SRC_COEF0      0x48
#define SRC_ID_VALUE   0x50464451u
#define CTRL_START     0x01u
#define CTRL_SNAPSHOT  0x02u
#define CTRL_ABORT     0x10u       /* graceful: finish the frame in progress, then stop */

#define DMA_MASK       0x14
#define DMA_CLEAR      0x18
#define DMA_STREAM0    0x460
#define DESC_CFG_ARMED 0xDu
#define DESC_OFF       0x0u
#define BUF_OFF        0x1000u
#define RT_DESC_STRIDE 0x40u
#define HW_NDEST       4

struct fabric_hw {
	struct fabric base;
	volatile uint32_t *src, *dma;
	uint8_t *ddr;
	uint64_t ddr_phys;
	size_t ddr_size;
	int dma_fd;
	uint32_t slot_sz, fbytes;
	unsigned processed, ev_base, ev;
	uint32_t frames0, drops_total, slots;
	int ended_abort;
	uint64_t last_progress_ns;
};

static inline int hw_read_str(const char *path, char *buf, size_t len)
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

static inline int hw_open_uio(const char *name, void **map, size_t maplen)
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
		if (hw_read_str(path, val, sizeof(val)) || strcmp(val, name))
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

static inline unsigned hw_uio_events(const char *uio_name)
{
	char path[300], val[64], name[128];
	DIR *d = opendir("/sys/class/uio");
	struct dirent *e;
	unsigned v = 0;
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "uio", 3)) continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
		if (hw_read_str(path, name, sizeof(name)) || strcmp(name, uio_name)) continue;
		snprintf(path, sizeof(path), "/sys/class/uio/%s/event", e->d_name);
		if (!hw_read_str(path, val, sizeof(val))) v = strtoul(val, NULL, 0);
	}
	if (d) closedir(d);
	return v;
}

static inline void hw_fence(void) { __sync_synchronize(); }

static inline void hw_snapshot(struct fabric_hw *h)
{
	h->src[SRC_CTRL / 4] = CTRL_SNAPSHOT;
	usleep(200);                    /* a few fabric cycles are enough; be generous */
}

/* One line with everything that says where a stalled run is stuck: the fabric counters, the DMA status,
   the UIO event count against what was consumed, and the four descriptors with the frame each buffer holds. */
static inline void hw_diag(struct fabric_hw *h, const char *why)
{
	uint32_t status, sent, seq, drops, cred, dstat;
	unsigned evs;

	hw_snapshot(h);
	status = h->src[SRC_STATUS / 4];
	sent = h->src[SRC_SNAP_FRAMES / 4] - h->frames0;
	seq = h->src[SRC_SNAP_SEQ / 4];
	drops = h->src[SRC_DROPS / 4];
	cred = h->src[SRC_CREDIT / 4];
	dstat = h->dma[0x10 / 4];
	evs = hw_uio_events("dma-controller@60010000") - h->ev_base;
	fprintf(stderr, "DIAG %s: processed %u of %u, fabric sent %u, status %#x, next_seq %u, drops %u, credits %u, "
		"dma stat %#x, uio events %u (read %u)", why, h->processed, h->slots, sent, status, seq, drops, cred, dstat, evs, h->ev);
	for (int k = 0; k < HW_NDEST; k++) {
		const volatile uint32_t *d = (const volatile uint32_t *)(h->ddr + DESC_OFF + k * RT_DESC_STRIDE);
		const volatile uint32_t *b = (const volatile uint32_t *)(h->ddr + BUF_OFF + k * h->slot_sz);
		fprintf(stderr, ", d%d cfg %#x buf seq %u", k, d[0], b[1]);
	}
	fprintf(stderr, "\n");
}

/* the order of writes is the one of I07: byte count and destination, fence, valid word, fence, address */
static inline void hw_arm_rt(struct fabric_hw *h, int k, uint32_t buf_off, uint32_t nbytes)
{
	volatile uint32_t *desc = (volatile uint32_t *)(h->ddr + DESC_OFF + k * RT_DESC_STRIDE);
	desc[1] = nbytes;
	desc[2] = (uint32_t)(h->ddr_phys + buf_off);
	hw_fence();
	desc[0] = DESC_CFG_ARMED;
	hw_fence();
	h->dma[DMA_STREAM0 / 4 + k] = (uint32_t)(h->ddr_phys + DESC_OFF + k * RT_DESC_STRIDE);
	hw_fence();
}

static inline int hw_start_run(struct fabric *f, const struct run_cfg *c)
{
	struct fabric_hw *h = (struct fabric_hw *)f;

	hw_snapshot(h);
	h->frames0 = h->src[SRC_SNAP_FRAMES / 4];
	h->src[SRC_PAYLOAD / 4] = ST_PAYLOAD_WORDS;
	h->src[SRC_FRAMES / 4] = c->slots;
	h->src[SRC_RATE / 4] = c->rate_cycles;
	h->src[SRC_FAULT / 4] = c->fault ? ((c->fault_frame << 16) | c->fault) : 0;
	h->src[SRC_MODE / 4] = 2u | (1u << 4) | ((uint32_t)(HW_NDEST - 1) << 8);   /* FIR payload, real-time, 4 destinations */
	for (int k = 0; k < HW_NDEST; k++)
		hw_arm_rt(h, k, BUF_OFF + k * h->slot_sz, h->fbytes);
	h->src[SRC_CREDIT / 4] = 0x80000000u | HW_NDEST;
	h->processed = 0;
	h->slots = c->slots;
	h->ended_abort = 0;
	h->ev_base = hw_uio_events("dma-controller@60010000");
	h->ev = 0;
	h->last_progress_ns = mono_ns();
	hw_fence();
	h->src[SRC_CTRL / 4] = CTRL_START;
	return 0;
}

/* True when the DMA has written the frame for slot (processed % 4) and cleared the Destination Data Ready bit
   (bit 2) of its descriptor: 0xD while armed, 0x9 when the frame is in memory (module I04). */
static inline int hw_slot_done(const struct fabric_hw *h)
{
	const volatile uint32_t *d = (const volatile uint32_t *)(h->ddr + DESC_OFF + (h->processed % HW_NDEST) * RT_DESC_STRIDE);
	return !(d[0] & 4u);
}

/*
 * Frames are taken in order, one per call. The truth about "frame n is in memory" is the descriptor of its
 * slot, not the UIO event count. The first version of this loop counted events (as run_realtime() in
 * pfdaq_cap does), and on the board an event sometimes goes missing (never seen on an idle board; seen under
 * network load, cause not isolated): the loop was then one frame behind for the rest of the run, so the last
 * frame of a run was never taken and the run ended on the 2 s no-progress limit (module M10). The UIO device
 * is now only the way to sleep until the next frame; whether a frame is there is read from its descriptor.
 */
static inline int hw_next(struct fabric *f, uint32_t *dst, uint32_t *seq, int timeout_ms)
{
	struct fabric_hw *h = (struct fabric_hw *)f;
	struct pollfd p = { .fd = h->dma_fd, .events = POLLIN };
	uint32_t cnt;

	if (!hw_slot_done(h)) {
		int to = timeout_ms < 2 ? timeout_ms : 2;       /* a merged event is noticed within one slot interval */
		if (poll(&p, 1, to) > 0 && read(h->dma_fd, &cnt, 4) == 4)
			h->ev = cnt - h->ev_base;
	}
	if (hw_slot_done(h)) {
		int k = h->processed % HW_NDEST;
		const uint32_t *buf = (const uint32_t *)(h->ddr + BUF_OFF + k * h->slot_sz);
		*seq = buf[1];
		if (dst)
			memcpy(dst, buf, h->fbytes);            /* copy first ... */
		hw_arm_rt(h, k, BUF_OFF + k * h->slot_sz, h->fbytes);   /* ... then re-arm ... */
		h->src[SRC_CREDIT / 4] = 1;                              /* ... then return the credit */
		h->processed++;
		h->last_progress_ns = mono_ns();
		return 1;
	}
	uint32_t status = h->src[SRC_STATUS / 4];
	if ((status & 2) && !(status & 1)) {                        /* done and not busy */
		hw_snapshot(h);
		if (h->src[SRC_SNAP_FRAMES / 4] - h->frames0 == h->processed) {
			h->ended_abort = (status >> 2) & 1;
			h->ev = hw_uio_events("dma-controller@60010000") - h->ev_base;
			if (h->ev != h->processed || getenv("STATION_DEBUG"))
				fprintf(stderr, "run end: %u frames taken from the descriptors, %u UIO events%s\n", h->processed, h->ev,
					h->ev < h->processed ? " (fewer events than completed frames)" : "");
			return 2;
		}
	}
	if (mono_ns() - h->last_progress_ns > 2000ull * 1000000ull) {
		hw_diag(h, "no progress for 2 s");
		return -1;                                              /* no progress for 2 s: same limit as pfdaq_cap */
	}
	return 0;
}

static inline void hw_abort(struct fabric *f)
{
	((struct fabric_hw *)f)->src[SRC_CTRL / 4] = CTRL_ABORT;
}

static inline int hw_ended_by_abort(struct fabric *f) { return ((struct fabric_hw *)f)->ended_abort; }

static inline uint32_t hw_next_seq(struct fabric *f)
{
	struct fabric_hw *h = (struct fabric_hw *)f;
	hw_snapshot(h);
	return h->src[SRC_SNAP_SEQ / 4];
}

static inline uint32_t hw_drops(struct fabric *f)
{
	struct fabric_hw *h = (struct fabric_hw *)f;
	hw_snapshot(h);
	return h->src[SRC_DROPS / 4];
}

static inline void hw_close(struct fabric *f) { (void)f; }

static inline struct fabric *fabric_hw_open(void)
{
	static struct fabric_hw h;
	void *m;
	char buf[64];

	memset(&h, 0, sizeof(h));
	h.dma_fd = -1;
	int src_fd = hw_open_uio("pfdaq_stream_src", &m, 0x1000);
	if (src_fd < 0) { perror("uio pfdaq_stream_src"); return NULL; }
	h.src = (volatile uint32_t *)((uint8_t *)m + SRC_OFF);
	h.dma_fd = hw_open_uio("dma-controller@60010000", &m, 0x1000);
	if (h.dma_fd < 0) { perror("uio dma-controller@60010000"); return NULL; }
	h.dma = m;
	if (hw_read_str("/sys/class/u-dma-buf/udmabuf-ddr-nc0/phys_addr", buf, sizeof(buf))) { perror("udmabuf phys_addr"); return NULL; }
	h.ddr_phys = strtoull(buf, NULL, 0);
	if (hw_read_str("/sys/class/u-dma-buf/udmabuf-ddr-nc0/size", buf, sizeof(buf))) { perror("udmabuf size"); return NULL; }
	h.ddr_size = strtoull(buf, NULL, 0);
	int ddr_fd = open("/dev/udmabuf-ddr-nc0", O_RDWR | O_SYNC);
	if (ddr_fd < 0) { perror("/dev/udmabuf-ddr-nc0"); return NULL; }
	h.ddr = mmap(NULL, h.ddr_size, PROT_READ | PROT_WRITE, MAP_SHARED, ddr_fd, 0);
	if (h.ddr == MAP_FAILED) { perror("mmap udmabuf"); return NULL; }
	h.fbytes = ST_FRAME_BYTES;
	h.slot_sz = (h.fbytes + 0xFFF) & ~0xFFFu;
	if (BUF_OFF + HW_NDEST * h.slot_sz > h.ddr_size) { fprintf(stderr, "buffers do not fit the udmabuf\n"); return NULL; }
	if (h.src[SRC_ID / 4] != SRC_ID_VALUE) { fprintf(stderr, "stream source ID %08x\n", h.src[SRC_ID / 4]); return NULL; }
	if (h.src[SRC_STATUS / 4] & 1) { fprintf(stderr, "stream source busy at start\n"); return NULL; }
	if (h.src[SRC_VERSION / 4] < 0x00050000) { fprintf(stderr, "fabric version %08x has no real-time mode\n", h.src[SRC_VERSION / 4]); return NULL; }
	/* FIR payload: step and coefficients as the model in station_common.h */
	struct model md;
	model_init(&md);
	h.src[SRC_FIR_STEP / 4] = md.step;
	for (int k = 0; k < 8; k++)
		h.src[SRC_COEF0 / 4 + k] = (uint16_t)md.coef[k];
	h.dma[DMA_MASK / 4] = 0xF;
	h.dma[DMA_CLEAR / 4] = 0xF;
	{   /* drop completions left over from an earlier run */
		struct pollfd p = { .fd = h.dma_fd, .events = POLLIN };
		uint32_t cnt;
		while (poll(&p, 1, 10) > 0 && read(h.dma_fd, &cnt, 4) == 4)
			;
	}
	h.base = (struct fabric){ hw_start_run, hw_next, hw_abort, hw_ended_by_abort, hw_next_seq, hw_drops, hw_close };
	return &h.base;
}

/* ------------------------------------------------------------------------------------------ */
/* simulation (PC)                                                                               */
/* ------------------------------------------------------------------------------------------ */
struct fabsim_shm {             /* shared with fake_hart4 */
	uint32_t abort_req;         /* written by the stand-in for hart 4 (CTRL.ABORT) */
	uint32_t next_seq;          /* what the fabric would report as NEXT_SEQ */
	uint32_t frames_sent;
	uint32_t drops;
};

struct fabric_sim {
	struct fabric base;
	struct fabsim_shm *sh;
	struct model md;
	struct run_cfg cfg;
	uint32_t sent_in_run, slot_idx;
	uint64_t t_start_ns;
	int ended_abort, running;
};

static inline struct fabsim_shm *fabsim_map(const char *path, int create)
{
	int fd = open(path, create ? (O_RDWR | O_CREAT) : O_RDWR, 0666);
	if (fd < 0)
		return NULL;
	if (create && ftruncate(fd, sizeof(struct fabsim_shm)) < 0) { close(fd); return NULL; }
	void *m = mmap(NULL, sizeof(struct fabsim_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return m == MAP_FAILED ? NULL : m;
}

static inline int sim_start_run(struct fabric *f, const struct run_cfg *c)
{
	struct fabric_sim *s = (struct fabric_sim *)f;
	s->cfg = *c;
	s->sent_in_run = s->slot_idx = 0;
	s->ended_abort = 0;
	s->running = 1;
	model_restart(&s->md);
	__atomic_store_n(&s->sh->abort_req, 0, __ATOMIC_SEQ_CST);
	s->t_start_ns = mono_ns();
	return 0;
}

static inline int sim_next(struct fabric *f, uint32_t *dst, uint32_t *seq, int timeout_ms)
{
	struct fabric_sim *s = (struct fabric_sim *)f;
	uint32_t scratch[ST_FRAME_WORDS];
	uint64_t slot_ns = (uint64_t)s->cfg.rate_cycles * 8ull;
	uint64_t deadline = mono_ns() + (uint64_t)timeout_ms * 1000000ull;

	if (!s->running)
		return 2;
	for (;;) {
		if (__atomic_load_n(&s->sh->abort_req, __ATOMIC_SEQ_CST)) {   /* CTRL.ABORT: the run stops at once here */
			s->running = 0;
			s->ended_abort = 1;
			return 2;
		}
		if (s->slot_idx >= s->cfg.slots) {
			s->running = 0;
			return 2;
		}
		uint64_t due = s->t_start_ns + (uint64_t)s->slot_idx * slot_ns, now = mono_ns();
		if (now >= due) {
			/* a consumer that is more than four slots late loses slots, like a fabric without credits */
			if (now > due + 4 * slot_ns) {
				uint32_t late = (uint32_t)((now - due) / slot_ns) - 4;
				if (late > s->cfg.slots - s->slot_idx)
					late = s->cfg.slots - s->slot_idx;
				s->slot_idx += late;
				s->sh->next_seq += late;
				s->sh->drops += late;
				continue;
			}
			uint32_t *w = dst ? dst : scratch;
			uint32_t sq = s->sh->next_seq;
			int crc_fault = (s->cfg.fault & 1) && s->sent_in_run == s->cfg.fault_frame;
			frame_build(w, sq, (uint32_t)(now / 8), &s->md, crc_fault);
			s->sh->next_seq = sq + 1;
			s->sh->frames_sent++;
			s->slot_idx++;
			s->sent_in_run++;
			*seq = sq;
			return 1;
		}
		uint64_t wake = due;
		if (wake > now + 1000000ull)
			wake = now + 1000000ull;                /* look at the abort word at least every millisecond */
		if (wake > deadline)
			wake = deadline;
		struct timespec ts = { (time_t)(wake / 1000000000ull), (long)(wake % 1000000000ull) };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		if (mono_ns() >= deadline && mono_ns() < due)
			return 0;
	}
}

static inline void sim_abort(struct fabric *f) { __atomic_store_n(&((struct fabric_sim *)f)->sh->abort_req, 1, __ATOMIC_SEQ_CST); }
static inline int sim_ended_by_abort(struct fabric *f) { return ((struct fabric_sim *)f)->ended_abort; }
static inline uint32_t sim_next_seq(struct fabric *f) { return ((struct fabric_sim *)f)->sh->next_seq; }
static inline uint32_t sim_drops(struct fabric *f) { return ((struct fabric_sim *)f)->sh->drops; }
static inline void sim_close(struct fabric *f) { (void)f; }

static inline struct fabric *fabric_sim_open(const char *shm_path)
{
	static struct fabric_sim s;
	memset(&s, 0, sizeof(s));
	s.sh = fabsim_map(shm_path, 1);
	if (!s.sh)
		return NULL;
	model_init(&s.md);
	s.base = (struct fabric){ sim_start_run, sim_next, sim_abort, sim_ended_by_abort, sim_next_seq, sim_drops, sim_close };
	return &s.base;
}

#endif /* STATION_FABRIC_H */
