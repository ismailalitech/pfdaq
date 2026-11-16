/* SPDX-License-Identifier: MIT
 *
 * station_common.h - pieces shared by the three station processes and by the host tests:
 *   - a lock-free single-producer single-consumer ring in POSIX shared memory (futex wake-up)
 *   - the control block the processes share
 *   - the frame format of the project's stream source, its CRC-32 and payload model
 *     (copied from yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c, which was verified
 *     on the board; only the structure around them is new) and the per-window features
 * Header only, so the host tests compile exactly the code that runs on the board.
 */
#ifndef STATION_COMMON_H
#define STATION_COMMON_H

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------------------------------ */
/* time                                                                                         */
/* ------------------------------------------------------------------------------------------ */
static inline uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------------------------ */
/* SPSC ring in shared memory                                                                   */
/*                                                                                              */
/* head and tail are free-running 32-bit counters; slot = counter % nslots (nslots is a power   */
/* of two). The producer owns head, the consumer owns tail. The producer fills a slot, then     */
/* publishes it with a sequentially consistent store of head; the consumer reads head with an   */
/* acquire load, so it sees the whole slot. A consumer that finds the ring empty announces that */
/* it sleeps (`waiting`), re-checks, and sleeps on a futex at `head`; the producer wakes it only */
/* if it announced, so the common case costs no system call.                                     */
/* ------------------------------------------------------------------------------------------ */
#define RING_MAGIC 0x52494e47u

struct ring_hdr {
	uint32_t magic, slot_size, nslots, pad0;
	uint8_t pad1[48];
	uint32_t head;                  /* producer */
	uint8_t pad2[60];
	uint32_t tail;                  /* consumer */
	uint8_t pad3[60];
	uint32_t waiting;               /* consumer is (about to be) asleep */
	uint8_t pad4[60];
};

struct ring {
	struct ring_hdr *h;
	uint8_t *slots;
	size_t maplen;
};

static inline size_t ring_bytes(uint32_t slot_size, uint32_t nslots)
{
	return sizeof(struct ring_hdr) + (size_t)slot_size * nslots;
}

/* create (init = 1: file is created and zeroed) or open an existing ring file */
static inline int ring_map(struct ring *r, const char *path, int init, uint32_t slot_size, uint32_t nslots)
{
	size_t len = ring_bytes(slot_size, nslots);
	int fd = open(path, init ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDWR, 0666);
	if (fd < 0)
		return -1;
	if (init && ftruncate(fd, (off_t)len) < 0) {
		close(fd);
		return -1;
	}
	r->h = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (r->h == MAP_FAILED)
		return -1;
	r->maplen = len;
	r->slots = (uint8_t *)r->h + sizeof(struct ring_hdr);
	if (init) {
		if (nslots & (nslots - 1)) { errno = EINVAL; return -1; }
		r->h->slot_size = slot_size;
		r->h->nslots = nslots;
		__atomic_store_n(&r->h->magic, RING_MAGIC, __ATOMIC_SEQ_CST);
	} else if (__atomic_load_n(&r->h->magic, __ATOMIC_ACQUIRE) != RING_MAGIC ||
		   r->h->slot_size != slot_size || r->h->nslots != nslots) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static inline uint32_t ring_count(const struct ring *r)     /* slots in use (either side may call) */
{
	return __atomic_load_n(&r->h->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->h->tail, __ATOMIC_ACQUIRE);
}

/* producer: pointer to the next free slot, or NULL if the ring is full */
static inline void *ring_reserve(struct ring *r)
{
	uint32_t head = r->h->head;     /* only the producer writes head */
	if (head - __atomic_load_n(&r->h->tail, __ATOMIC_ACQUIRE) >= r->h->nslots)
		return NULL;
	return r->slots + (size_t)(head % r->h->nslots) * r->h->slot_size;
}

static inline void ring_commit(struct ring *r)
{
	__atomic_store_n(&r->h->head, r->h->head + 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&r->h->waiting, __ATOMIC_SEQ_CST))
		syscall(SYS_futex, &r->h->head, FUTEX_WAKE, 1, NULL, NULL, 0);
}

/* consumer: oldest unread slot or NULL */
static inline const void *ring_peek(struct ring *r)
{
	uint32_t tail = r->h->tail;     /* only the consumer writes tail */
	if (__atomic_load_n(&r->h->head, __ATOMIC_ACQUIRE) == tail)
		return NULL;
	return r->slots + (size_t)(tail % r->h->nslots) * r->h->slot_size;
}

static inline void ring_release(struct ring *r)
{
	__atomic_store_n(&r->h->tail, r->h->tail + 1, __ATOMIC_RELEASE);
}

/* consumer: sleep until the ring is not empty or timeout_ms passed; returns 1 if there is data */
static inline int ring_wait(struct ring *r, int timeout_ms)
{
	uint32_t head;
	struct timespec to = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };

	__atomic_store_n(&r->h->waiting, 1, __ATOMIC_SEQ_CST);
	head = __atomic_load_n(&r->h->head, __ATOMIC_SEQ_CST);
	if (head == r->h->tail) {
		syscall(SYS_futex, &r->h->head, FUTEX_WAIT, head, &to, NULL, 0);
		head = __atomic_load_n(&r->h->head, __ATOMIC_ACQUIRE);
	}
	__atomic_store_n(&r->h->waiting, 0, __ATOMIC_SEQ_CST);
	return head != r->h->tail;
}

/* ------------------------------------------------------------------------------------------ */
/* control block (shared memory); every field has exactly one writing process                   */
/* ------------------------------------------------------------------------------------------ */
#define CTL_MAGIC 0x53544c31u

enum { ACQ_STARTING = 0, ACQ_RUNNING = 1, ACQ_SAFE_WAIT = 2, ACQ_STOPPED = 3 };
enum { INJ_NONE = 0, INJ_CRC = 1 };

struct station_ctl {
	uint32_t magic;
	/* written by init and by the stop script / signal handlers */
	uint32_t stop;
	uint32_t acq_pid, ana_pid, tel_pid;
	uint64_t t0_ns;             /* CLOCK_MONOTONIC at init: the zero of every timestamp in the logs */
	/* written by analysis */
	uint32_t epoch;             /* +1 on every accepted RESET; acquisition restarts when it changes */
	uint32_t inject;            /* cleared by analysis on RESET; set by telemetry on operator command */
	uint32_t hart_state, hart_trips, hart_reason, hart_events;
	uint64_t ana_frames, ana_bad, ana_gaps, ana_windows, ana_xfer_fail, ana_hb;
	/* written by acquisition */
	uint32_t acq_state;
	uint32_t acq_next_seq;      /* next sequence number expected from the fabric */
	uint64_t acq_frames, acq_ring_overruns, acq_fab_drops, acq_runs, acq_aborts;
	/* written by telemetry */
	uint64_t tel_windows;
};

/* ------------------------------------------------------------------------------------------ */
/* records on the rings                                                                          */
/* ------------------------------------------------------------------------------------------ */
#define ST_PAYLOAD_WORDS   252u
#define ST_FRAME_WORDS     (ST_PAYLOAD_WORDS + 4u)
#define ST_FRAME_BYTES     (ST_FRAME_WORDS * 4u)

/* ring A: acquisition -> analysis (one slot per captured frame) */
struct frame_slot {
	uint64_t t_ns;              /* CLOCK_MONOTONIC when the frame was copied out of the DMA buffer */
	uint32_t run_id;
	uint32_t flags;             /* bit 0: first frame of a run (the FIR model restarts) */
	uint32_t w[ST_FRAME_WORDS];
};
#define RING_A_SLOT   ((uint32_t)sizeof(struct frame_slot))
#define RING_A_SLOTS  256u

/* ring B: analysis -> telemetry */
enum { TR_WINDOW = 1, TR_EVENT = 2 };
enum { EV_ARMED = 1, EV_TRIP = 2, EV_RESET_OK = 3, EV_RESET_REFUSED = 4, EV_LIMITS = 5, EV_FLUSH = 6, EV_LINK_LOST = 7, EV_INFO = 8 };
struct tel_rec {
	uint32_t type;
	uint32_t seq;
	uint64_t t_ns;
	/* WINDOW: win_id frames bad gaps peak mean rtt_us hart_state trips reason events win_ms
	   EVENT : code reason trips hart_tick_ms */
	uint32_t w[12];
	char text[48];
};
#define RING_B_SLOT   ((uint32_t)sizeof(struct tel_rec))
#define RING_B_SLOTS  64u

/* ring C: telemetry -> analysis (operator commands) */
enum { CMD_RESET = 1, CMD_LIMITS = 2, CMD_QUERY = 3 };
struct cmd_rec {
	uint32_t type, a, b, c;
};
#define RING_C_SLOT   ((uint32_t)sizeof(struct cmd_rec))
#define RING_C_SLOTS  16u

/* ------------------------------------------------------------------------------------------ */
/* CRC-32 and payload model (from pfdaq_cap.c)                                                   */
/* ------------------------------------------------------------------------------------------ */
#define MAGIC_HI 0xA55Au

static uint32_t crc_tab[8][256];

static inline void crc_init(void)
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
static inline uint32_t crc32_words(const uint32_t *w, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		c ^= w[i];
		c = crc_tab[3][c & 0xFF] ^ crc_tab[2][(c >> 8) & 0xFF] ^
		    crc_tab[1][(c >> 16) & 0xFF] ^ crc_tab[0][c >> 24];
	}
	return ~c;
}

/* FIR payload model: 8-tap FIR over a triangle wave, continuous across the frames of one run */
struct model {
	uint32_t step;
	int32_t coef[8];
	uint64_t fir_i;             /* FIR samples since the run started */
	int32_t xh[8];              /* xh[0] = newest input sample */
	int valid;                  /* cleared after a sequence gap: payload check pauses until the next run */
};

static inline void model_init(struct model *md)
{
	md->step = 256;
	memcpy(md->coef, (int32_t[8]){ 12000, -15000, 20000, 32767, -32768, 20000, -15000, 12000 }, sizeof(md->coef));
	md->fir_i = 0;
	memset(md->xh, 0, sizeof(md->xh));
	md->valid = 1;
}

static inline void model_restart(struct model *md)
{
	md->fir_i = 0;
	memset(md->xh, 0, sizeof(md->xh));
	md->valid = 1;
}

static inline int32_t tri16(uint64_t p)
{
	uint32_t q = (uint32_t)(p & 0xFFFF);
	uint32_t t = (q & 0x8000) ? (~q & 0x7FFF) : (q & 0x7FFF);
	return (int32_t)t - 16384;
}

/* next FIR output sample of the model (the model state advances) */
static inline uint32_t model_next(struct model *md)
{
	memmove(md->xh + 1, md->xh, 7 * sizeof(int32_t));
	md->xh[0] = tri16((md->fir_i + 1) * md->step);
	md->fir_i++;
	int64_t y = 0;
	for (int t = 0; t < 8; t++)
		y += (int64_t)md->coef[t] * md->xh[t];
	return (uint32_t)y;
}

/* returns 1 if the payload of frame w differs from the model (the model state always advances) */
static inline int payload_bad(const uint32_t *w, uint32_t payload, struct model *md)
{
	int bad = 0;
	for (uint32_t k = 0; k < payload; k++)
		if (model_next(md) != w[3 + k])
			bad = 1;
	return bad;
}

/* build a frame the way the fabric does (used by the simulated source and the tests) */
static inline void frame_build(uint32_t *w, uint32_t seq, uint32_t ts, struct model *md, int crc_fault)
{
	w[0] = (MAGIC_HI << 16) | ST_PAYLOAD_WORDS;
	w[1] = seq;
	w[2] = ts;
	for (uint32_t k = 0; k < ST_PAYLOAD_WORDS; k++)
		w[3 + k] = model_next(md);
	w[ST_FRAME_WORDS - 1] = crc32_words(w, ST_FRAME_WORDS - 1) ^ (crc_fault ? 1u : 0u);   /* the fabric corrupts the CRC */
}

/* ------------------------------------------------------------------------------------------ */
/* verification and window features                                                              */
/* ------------------------------------------------------------------------------------------ */
struct window {
	uint32_t frames, bad, gaps;
	uint32_t peak;              /* largest |payload word| read as int32 */
	uint64_t sum_abs;           /* sum of |payload word|, for the mean */
	uint64_t words;
	uint64_t t_start_ns;
};

static inline uint32_t abs32(int32_t v) { return v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v; }

static inline void window_reset(struct window *wn, uint64_t now_ns)
{
	memset(wn, 0, sizeof(*wn));
	wn->t_start_ns = now_ns;
}

static inline uint32_t window_mean(const struct window *wn)
{
	return wn->words ? (uint32_t)(wn->sum_abs / wn->words) : 0;
}

struct verifier {
	struct model md;
	uint32_t exp_seq;
	int have_seq;
	uint64_t total_frames, total_bad, total_gaps;
};

static inline void verifier_init(struct verifier *v)
{
	model_init(&v->md);
	v->have_seq = 0;
	v->exp_seq = 0;
	v->total_frames = v->total_bad = v->total_gaps = 0;
}

/* forget the expected sequence number (after a flush) so the next frame defines it; the payload check
   pauses until the first frame of the next run restarts the model */
static inline void verifier_resync(struct verifier *v)
{
	v->have_seq = 0;
	v->md.valid = 0;            /* the run may continue: the payload model is out of phase until a run starts */
}

/*
 * Check one frame (magic, sequence continuity with forward gaps counted, payload model, CRC) and
 * add it to the window. A frame is "bad" if any check fails. A forward jump of the sequence number
 * is a gap (dropped frames), not a bad frame; the payload model pauses after a gap until the next
 * run, because dropped slots may or may not consume FIR samples (not known from the project's
 * records), while the CRC still covers the frame. Returns 1 if the frame is bad.
 */
static inline int verify_frame(struct verifier *v, struct window *wn, const struct frame_slot *f)
{
	const uint32_t *w = f->w;
	int bad = 0;

	if (f->flags & 1u)
		model_restart(&v->md);
	wn->frames++;
	v->total_frames++;
	if (w[0] != ((MAGIC_HI << 16) | ST_PAYLOAD_WORDS)) {
		bad = 1;
		v->md.valid = 0;
	} else {
		if (v->have_seq && w[1] != v->exp_seq) {
			if ((int32_t)(w[1] - v->exp_seq) > 0) {
				wn->gaps += w[1] - v->exp_seq;
				v->total_gaps += w[1] - v->exp_seq;
				v->md.valid = 0;
			} else {
				bad = 1;            /* repeated or reordered sequence number */
			}
		}
		v->exp_seq = w[1] + 1;
		v->have_seq = 1;
		if (v->md.valid) {
			if (payload_bad(w, ST_PAYLOAD_WORDS, &v->md))
				bad = 1;
		}
		if (crc32_words(w, ST_FRAME_WORDS - 1) != w[ST_FRAME_WORDS - 1])
			bad = 1;
		for (uint32_t k = 0; k < ST_PAYLOAD_WORDS; k++) {
			uint32_t a = abs32((int32_t)w[3 + k]);
			if (a > wn->peak)
				wn->peak = a;
			wn->sum_abs += a;
		}
		wn->words += ST_PAYLOAD_WORDS;
	}
	if (bad) {
		wn->bad++;
		v->total_bad++;
	}
	return bad;
}

#endif /* STATION_COMMON_H */
