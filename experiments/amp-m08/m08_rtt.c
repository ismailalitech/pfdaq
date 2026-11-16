// SPDX-License-Identifier: MIT
/*
 * m08_rtt - round-trip time between a Linux program and hart 4, four ways (experiment M08).
 *
 *   rpmsg-query  the production ping: RPMsg QUERY (type 4). The supervisor answers it after its fabric
 *                snapshot, which waits 1 ms (module M02), so this includes that wait.
 *   rpmsg-ping   RPMsg again, but with a message type of the M08 payload that is answered at once.
 *   flag         a word in the non-cached DDR window that both sides poll: Linux writes req = n, hart 4
 *                (busy-polling inside one RPMsg call) writes ack = n.
 *   irq          Linux writes irq_req = n in the same shared block and then raises the fabric interrupt
 *                of latency channel 1 (CTRL.ONESHOT); hart 4's interrupt handler writes irq_ack = n.
 *
 * Each trial runs on one cpu (pinned), is timed with CLOCK_MONOTONIC from just before the request becomes
 * visible to just after the answer is seen, and ends in one of three ways: answered within the timeout
 * (ok), answered after the timeout but within the grace time (late), or not answered at all (lost).
 * The report gives min, median, p95 and max over the answered trials and the counts of late and lost ones.
 *
 * --sim runs everything against threads in this process (stand-ins for hart 4 and the fabric), for the
 * PC tests; nothing in the board paths changes.
 *
 * Build: make -C experiments/amp-m08 host | board
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "pfdaq_m08.h"

#define SUP_CHANNEL   "pfdaq-supervisor"
#define SUP_MSG_MAGIC 0x53444650u
#define SUP_ACK_MAGIC 0x41444650u
#define SUP_QUERY     4u

struct sup_msg { uint32_t magic, type, burst, next_seq, errors, deadline_ms, max_lag; };
struct sup_ack { uint32_t magic, type, state, status_rx, deadline_miss, seq_mismatch, fabric_seq, fabric_frames, uptime_ms; };

/* latency unit channel 1 in the pfdaq APB slot (hw/rtl/pfdaq_latency.v); the slot is at page offset 0xF00 */
#define SRC_OFF       0xF00u
#define LAT1_OFF      0xC0u
#define LAT_CTRL      (0x00u / 4)
#define LAT_STATUS    (0x10u / 4)
#define LAT_LAST      (0x20u / 4)
#define LAT_CTRL_EN   0x1u
#define LAT_CTRL_ONE  0x2u
#define LAT_CTRL_CLR  0x4u

enum { M_QUERY, M_PING, M_FLAG, M_IRQ, M_COUNT };
static const char *const mname[M_COUNT] = { "rpmsg-query", "rpmsg-ping", "flag", "irq" };

enum { T_OK, T_LATE, T_LOST };

struct opt {
	unsigned n, warm, cpu, timeout_us, grace_ms, gap_us;
	unsigned methods;            /* bit per M_* */
	const char *csv;
	const char *json;
	int sim, force, hwlat, probe;
	unsigned sim_drop_every;     /* sim: do not answer every Nth request (lost) */
	unsigned sim_late_every;     /* sim: answer every Nth request after sim_late_us (late) */
	unsigned sim_late_us;
	unsigned sim_extra_us;       /* sim: every answer is delayed by this much */
	int sim_chan_busy;           /* sim: latency channel 1 is enabled (periodic events) before the run */
	int sim_production;          /* sim: hart 4 runs the production firmware (does not know the M08 types) */
};

static struct opt O = { .n = 1000, .warm = 50, .cpu = 2, .timeout_us = 5000, .grace_ms = 50, .gap_us = 1000,
		       .methods = (1u << M_COUNT) - 1, .hwlat = 1, .sim_late_us = 20000 };

static inline uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_us(unsigned us)
{
	struct timespec ts = { us / 1000000u, (long)(us % 1000000u) * 1000 };
	nanosleep(&ts, NULL);
}

/* ---- statistics --------------------------------------------------------------------------------- */
static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

struct stats { uint64_t min, median, p95, max; unsigned n; };

/* sorts v in place. median: mean of the two middle values for an even count. p95: nearest rank, ceil(0.95 n). */
static struct stats compute_stats(uint64_t *v, unsigned n)
{
	struct stats s = { 0, 0, 0, 0, n };
	if (!n)
		return s;
	qsort(v, n, sizeof(*v), cmp_u64);
	s.min = v[0];
	s.max = v[n - 1];
	s.median = (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
	unsigned idx = (unsigned)(((uint64_t)n * 95 + 99) / 100);     /* ceil(0.95 n) */
	s.p95 = v[idx ? idx - 1 : 0];
	return s;
}

/* ---- the two shared things the board methods use ---------------------------------------------- */
static volatile struct m08_shm *shm;       /* shared block, non-cached window */
static volatile uint32_t *lat1;            /* latency channel 1 registers (word pointer) */
static int sup_fd = -1;
static uint64_t shm_phys;

/* ---- sim: stand-ins for hart 4 and the fabric ---------------------------------------------------- */
static pthread_t th_poll, th_irq, th_rpmsg;
static volatile int sim_quit;
static int sim_sock[2];
static volatile uint32_t sim_poll_run;     /* the flag responder runs m08_poll() while this is 1 */
static volatile uint32_t sim_queries;

static uint32_t sim_now_ms(void) { return (uint32_t)(mono_ns() / 1000000ull); }

static void sim_maybe_delay(uint32_t n)
{
	if (O.sim_extra_us)
		sleep_us(O.sim_extra_us);
	if (O.sim_late_every && n % O.sim_late_every == 0)
		sleep_us(O.sim_late_us);
}

static void *sim_poll_thread(void *arg)
{
	(void)arg;
	while (!sim_quit) {
		if (sim_poll_run && shm && shm->magic == M08_MAGIC) {
			uint32_t to;
			/* the real routine of the firmware header; answers are written by it, this thread only adds
			 * the sim's drop and delay by watching req and acting before m08_poll would: instead the
			 * hook below is used when faults are requested */
			if (O.sim_drop_every || O.sim_late_every || O.sim_extra_us) {
				/* fault-injecting variant of the same loop */
				uint32_t last = shm->req, n = 0;
				shm->poll_state = M08_POLL_RUNNING;
				while (!shm->stop && !sim_quit) {
					uint32_t r = shm->req;
					if (r != last) {
						last = r;
						n++;
						if (O.sim_drop_every && n % O.sim_drop_every == 0)
							continue;
						sim_maybe_delay(n);
						shm->ack = r;
						shm->poll_answers++;
					}
				}
				shm->poll_state = M08_POLL_IDLE;
			} else {
				(void)m08_poll(shm, sim_now_ms, 20000, &to);
			}
			sim_poll_run = 0;
		} else {
			usleep(100);
		}
	}
	return NULL;
}

static void *sim_irq_thread(void *arg)
{
	(void)arg;
	uint32_t n = 0;
	while (!sim_quit) {
		if (lat1[LAT_CTRL] & LAT_CTRL_ONE) {          /* the "fabric": the pulse bit is read back as set */
			lat1[LAT_CTRL] &= ~LAT_CTRL_ONE;
			lat1[LAT_STATUS] = 1;                      /* interrupt pending */
			n++;
			if (O.sim_drop_every && n % O.sim_drop_every == 0) {
				lat1[LAT_STATUS] = 0;
				continue;
			}
			sim_maybe_delay(n);
			if (shm && shm->magic == M08_MAGIC)
				m08_irq_answer(shm);                   /* the real routine, as the handler calls it */
			lat1[LAT_LAST] = 7;                        /* 140 ns, a made-up fabric reading for the sim */
			lat1[LAT_STATUS] = 0;
		}
	}
	return NULL;
}

static void *sim_rpmsg_thread(void *arg)
{
	(void)arg;
	uint32_t n = 0;
	struct sup_msg m;
	while (!sim_quit) {
		struct pollfd p = { .fd = sim_sock[1], .events = POLLIN };
		if (poll(&p, 1, 20) <= 0)
			continue;
		if (read(sim_sock[1], &m, sizeof(m)) != (ssize_t)sizeof(m))
			continue;
		struct sup_ack a = { SUP_ACK_MAGIC, m.type, 0, 0, 0, 0, 0, 0, 0 };
		switch (m.type) {
		case SUP_QUERY:
			sim_queries++;
			sleep_us(1000);                            /* the production supervisor's 1 ms snapshot wait */
			break;
		case M08_T_PING:
			a.fabric_frames = m.burst;                 /* echo, as the firmware does for PING */
			break;
		case M08_T_SETUP:
			a.status_rx = (shm && !O.sim_production) ? shm->magic : 0;
			a.fabric_seq = shm ? shm->version : 0;
			break;
		case M08_T_POLL:
			sim_poll_run = 1;
			while (sim_poll_run && !sim_quit)
				usleep(100);
			a.status_rx = shm ? shm->poll_answers : 0;
			break;
		default:
			break;
		}
		n++;
		if (O.sim_drop_every && (m.type == SUP_QUERY || m.type == M08_T_PING) && n % O.sim_drop_every == 0)
			continue;
		if (m.type == SUP_QUERY || m.type == M08_T_PING)
			sim_maybe_delay(n);
		if (write(sim_sock[1], &a, sizeof(a)) < 0)
			break;
	}
	return NULL;
}

static int sim_start(void)
{
	shm = calloc(1, M08_BLOCK_SIZE);
	lat1 = calloc(64, sizeof(uint32_t));
	if (!shm || !lat1 || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sim_sock) < 0)
		return -1;
	sup_fd = sim_sock[0];
	if (O.sim_chan_busy)
		lat1[LAT_CTRL] = LAT_CTRL_EN;
	shm_phys = M08_WINDOW_BASE + M08_BLOCK_OFFSET;
	pthread_create(&th_poll, NULL, sim_poll_thread, NULL);
	pthread_create(&th_irq, NULL, sim_irq_thread, NULL);
	pthread_create(&th_rpmsg, NULL, sim_rpmsg_thread, NULL);
	return 0;
}

static void sim_stop(void)
{
	sim_quit = 1;
	pthread_join(th_poll, NULL);
	pthread_join(th_irq, NULL);
	pthread_join(th_rpmsg, NULL);
}

/* ---- board access ---------------------------------------------------------------------------------- */
static int read_str(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY), r;
	if (fd < 0)
		return -1;
	r = write(fd, val, strlen(val) + 1) < 0 ? -1 : 0;
	close(fd);
	return r;
}

/* from pfdaq_cap.c sup_open(), the Linux 6.18 branch: bind the channel to rpmsg_chrdev, open /dev/rpmsgN */
static int open_rpmsg(void)
{
	char dev[300] = "", path[700], node[300] = "";
	struct dirent *e;
	DIR *d = opendir("/sys/bus/rpmsg/devices");

	if (!d)
		return -1;
	while ((e = readdir(d)))
		if (strstr(e->d_name, SUP_CHANNEL))
			snprintf(dev, sizeof(dev), "%s", e->d_name);
	closedir(d);
	if (!dev[0]) { errno = ENODEV; return -1; }
	snprintf(path, sizeof(path), "/sys/bus/rpmsg/devices/%s/driver_override", dev);
	write_str(path, "rpmsg_chrdev");
	write_str("/sys/bus/rpmsg/drivers/rpmsg_chrdev/bind", dev);       /* fails harmlessly if already bound */
	snprintf(path, sizeof(path), "/sys/bus/rpmsg/devices/%s/rpmsg", dev);
	for (int tries = 0; tries < 50 && !(d = opendir(path)); tries++)
		usleep(20000);
	if (!d) { errno = ENOENT; return -1; }
	while ((e = readdir(d)))
		if (!strncmp(e->d_name, "rpmsg", 5) && strncmp(e->d_name, "rpmsg_ctrl", 10))
			snprintf(node, sizeof(node), "/dev/%s", e->d_name);
	closedir(d);
	if (!node[0]) { errno = ENOENT; return -1; }
	return open(node, O_RDWR);
}

static int open_uio_page(const char *name, void **map)
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
		*map = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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

static int board_start(unsigned need_shm, unsigned need_irq)
{
	char buf[128];

	sup_fd = open_rpmsg();
	if (sup_fd < 0) { perror("supervisor RPMsg endpoint (is the M08 payload running?)"); return -1; }
	if (need_shm) {
		if (read_str("/sys/class/u-dma-buf/udmabuf-ddr-nc0/phys_addr", buf, sizeof(buf))) { perror("udmabuf phys_addr"); return -1; }
		uint64_t base = strtoull(buf, NULL, 0);
		if (read_str("/sys/class/u-dma-buf/udmabuf-ddr-nc0/size", buf, sizeof(buf))) { perror("udmabuf size"); return -1; }
		uint64_t size = strtoull(buf, NULL, 0);
		if (base != M08_WINDOW_BASE || size < M08_BLOCK_OFFSET + M08_BLOCK_SIZE) {
			fprintf(stderr, "udmabuf-ddr-nc0 is 0x%llx + 0x%llx, expected the window at 0x%x with the block inside it\n",
				(unsigned long long)base, (unsigned long long)size, M08_WINDOW_BASE);
			return -1;
		}
		shm_phys = base + M08_BLOCK_OFFSET;
		int fd = open("/dev/udmabuf-ddr-nc0", O_RDWR | O_SYNC);          /* O_SYNC: the non-cached mapping */
		if (fd < 0) { perror("/dev/udmabuf-ddr-nc0"); return -1; }
		void *m = mmap(NULL, M08_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, M08_BLOCK_OFFSET);
		if (m == MAP_FAILED) { perror("mmap shared block"); return -1; }
		shm = m;
	}
	if (need_irq) {
		void *m;
		if (open_uio_page("pfdaq_stream_src", &m) < 0) { perror("uio pfdaq_stream_src"); return -1; }
		lat1 = (volatile uint32_t *)((uint8_t *)m + SRC_OFF + LAT1_OFF);
	}
	return 0;
}

/* ---- the trial functions -------------------------------------------------------------------------- */
static uint64_t timeout_ns(void) { return (uint64_t)O.timeout_us * 1000ull; }
static uint64_t grace_ns(void) { return (uint64_t)O.grace_ms * 1000000ull; }

/* RPMsg: send msg, wait for an answer with the right type (and, for PING, the echoed number) */
static int rpmsg_trial(uint32_t type, uint32_t seq, uint64_t *rtt)
{
	struct sup_msg m = { SUP_MSG_MAGIC, type, seq, 0, 0, 0, 0 };
	struct sup_ack a;
	struct pollfd p = { .fd = sup_fd, .events = POLLIN };
	uint64_t t0 = mono_ns(), t1;
	uint64_t limit = timeout_ns() + grace_ns();

	if (write(sup_fd, &m, sizeof(m)) != (ssize_t)sizeof(m))
		return T_LOST;
	for (;;) {
		uint64_t now = mono_ns();
		if (now - t0 >= limit)
			return T_LOST;
		int ms = (int)((limit - (now - t0)) / 1000000ull) + 1;
		if (poll(&p, 1, ms) <= 0)
			continue;
		if (read(sup_fd, &a, sizeof(a)) != (ssize_t)sizeof(a))
			continue;
		t1 = mono_ns();
		if (a.magic != SUP_ACK_MAGIC || a.type != type)
			continue;                                    /* something else: not ours */
		if (type == M08_T_PING && a.fabric_frames != seq)
			continue;                                    /* answer to an earlier, late request */
		*rtt = t1 - t0;
		return *rtt <= timeout_ns() ? T_OK : T_LATE;
	}
}

static int trial_query(uint32_t seq, uint64_t *rtt) { return rpmsg_trial(SUP_QUERY, seq, rtt); }
static int trial_ping(uint32_t seq, uint64_t *rtt) { return rpmsg_trial(M08_T_PING, seq, rtt); }

static int trial_flag(uint32_t seq, uint64_t *rtt)
{
	uint64_t t0 = mono_ns(), t1, limit = timeout_ns() + grace_ns();
	unsigned spin = 0;

	shm->req = seq;
	__sync_synchronize();
	for (;;) {
		if (shm->ack == seq) {
			t1 = mono_ns();
			*rtt = t1 - t0;
			return *rtt <= timeout_ns() ? T_OK : T_LATE;
		}
		if (((++spin) & 0x3Fu) == 0 && mono_ns() - t0 >= limit)
			return T_LOST;
	}
}

static uint32_t lat_hw_last, lat_hw_valid;

static int trial_irq(uint32_t seq, uint64_t *rtt)
{
	uint64_t t0, t1, limit = timeout_ns() + grace_ns();
	unsigned spin = 0;
	uint32_t ctrl = lat1[LAT_CTRL] & 0xFFFF0000u;              /* keep the jitter mask, ENABLE stays 0 */

	shm->irq_req = seq;
	__sync_synchronize();
	t0 = mono_ns();
	lat1[LAT_CTRL] = ctrl | LAT_CTRL_ONE;                      /* the "software interrupt": one event now */
	for (;;) {
		if (shm->irq_ack == seq) {
			t1 = mono_ns();
			*rtt = t1 - t0;
			if (O.hwlat) {                                     /* outside the timed part: the fabric's own reading */
				for (unsigned i = 0; i < 1000 && (lat1[LAT_STATUS] & 1u); i++)
					;
				lat_hw_last = lat1[LAT_LAST];
				lat_hw_valid = 1;
			}
			return *rtt <= timeout_ns() ? T_OK : T_LATE;
		}
		if (((++spin) & 0x3Fu) == 0 && mono_ns() - t0 >= limit)
			return T_LOST;
	}
}

/* ---- one method -------------------------------------------------------------------------------------- */
struct result {
	int ran;
	const char *note;
	unsigned trials, ok, late, lost;
	struct stats st;                 /* over ok + late answers, ns */
	struct stats hw;                 /* irq only: the fabric's reading, cycles */
	unsigned hw_n;
	uint32_t hart_count;             /* what hart 4 reports having answered; 0xFFFFFFFF = not available */
	uint32_t hart_expect;
};
static struct result res[M_COUNT];
static FILE *csv;

static int sup_exchange(uint32_t type, uint32_t burst, uint32_t next_seq, struct sup_ack *a, int timeout_ms)
{
	struct sup_msg m = { SUP_MSG_MAGIC, type, burst, next_seq, 0, 0, 0 };
	struct pollfd p = { .fd = sup_fd, .events = POLLIN };
	uint64_t t0 = mono_ns();

	if (write(sup_fd, &m, sizeof(m)) != (ssize_t)sizeof(m))
		return -1;
	for (;;) {
		int left = timeout_ms - (int)((mono_ns() - t0) / 1000000ull);
		if (left <= 0 || poll(&p, 1, left) <= 0)
			return -1;
		if (read(sup_fd, a, sizeof(*a)) != (ssize_t)sizeof(*a))
			return -1;
		if (a->magic == SUP_ACK_MAGIC && a->type == type)
			return 0;
	}
}

static void shm_init(void)
{
	memset((void *)shm, 0, sizeof(*shm));
	shm->version = M08_VERSION;
	__sync_synchronize();
	shm->magic = M08_MAGIC;
	__sync_synchronize();
}

static int do_setup(void)
{
	struct sup_ack a;

	if (sup_exchange(M08_T_SETUP, (uint32_t)shm_phys, 0, &a, 2000)) {
		fprintf(stderr, "no answer to SETUP: is the M08 payload running on hart 4?\n");
		return -1;
	}
	if (a.status_rx != M08_MAGIC) {
		fprintf(stderr, "hart 4 sees %08x at the shared block, expected %08x (%s)\n", a.status_rx, M08_MAGIC,
			a.status_rx == 0xFFFFFFFFu ? "it refused the address" : "the two sides do not see the same memory");
		return -1;
	}
	return 0;
}

typedef int (*trial_fn)(uint32_t seq, uint64_t *rtt);

static void record(int m, uint32_t seq, int r, uint64_t rtt, uint64_t *samples, unsigned *ns, int warm)
{
	if (csv)
		fprintf(csv, "%s,%u,%s,%s,%llu\n", mname[m], seq, warm ? "warm" : "run",
			r == T_OK ? "ok" : r == T_LATE ? "late" : "lost", (unsigned long long)(r == T_LOST ? 0 : rtt));
	if (warm)
		return;
	res[m].trials++;
	if (r == T_LOST) {
		res[m].lost++;
		return;
	}
	if (r == T_OK)
		res[m].ok++;
	else
		res[m].late++;
	samples[(*ns)++] = rtt;
}

static void run_trials(int m, trial_fn fn, unsigned first, unsigned count, uint64_t *samples, unsigned *ns, uint64_t *hw, unsigned *nhw,
		       int stop_on_lost)
{
	for (unsigned i = 0; i < count; i++) {
		uint32_t seq = first + i + 1;
		uint64_t rtt = 0;
		int warm = i < O.warm && first == 0;
		lat_hw_valid = 0;
		int r = fn(seq, &rtt);
		record(m, seq, r, rtt, samples, ns, warm);
		if (!warm && m == M_IRQ && r != T_LOST && lat_hw_valid)
			hw[(*nhw)++] = lat_hw_last;
		if (r == T_LOST && stop_on_lost) {
			res[m].note = "stopped at the first lost answer: later answers could not be matched to their requests";
			return;
		}
		if (r == T_LOST && m == M_IRQ && (lat1[LAT_STATUS] & 1u)) {
			res[m].note = "stopped: the interrupt stayed pending (hart 4 did not acknowledge it)";
			return;
		}
		if (O.gap_us)
			sleep_us(O.gap_us);
	}
}

static void finish_stats(int m, uint64_t *samples, unsigned ns, uint64_t *hw, unsigned nhw)
{
	res[m].st = compute_stats(samples, ns);
	res[m].hw = compute_stats(hw, nhw);
	res[m].hw_n = nhw;
}

static void run_method(int m)
{
	unsigned total = O.n + O.warm;
	uint64_t *samples = calloc(total + 1, sizeof(uint64_t)), *hw = calloc(total + 1, sizeof(uint64_t));
	unsigned ns = 0, nhw = 0;

	res[m].ran = 1;
	res[m].hart_count = 0xFFFFFFFFu;
	if (m == M_QUERY || m == M_PING) {
		run_trials(m, m == M_QUERY ? trial_query : trial_ping, 0, total, samples, &ns, hw, &nhw, 1);
	} else if (m == M_FLAG) {
		/* hart 4 polls inside one RPMsg call of at most M08_POLL_MAX_MS: do the trials in chunks that fit */
		unsigned per_trial_ms = O.gap_us / 1000u + 1u + O.timeout_us / 1000u;
		unsigned chunk = (M08_POLL_MAX_MS - 2000u) / per_trial_ms;
		unsigned done = 0, answers_total = 0;
		if (chunk < 10)
			chunk = 10;
		shm_init();
		if (do_setup()) { res[m].note = "setup failed"; free(samples); free(hw); return; }
		while (done < total) {
			unsigned c = total - done < chunk ? total - done : chunk;
			uint32_t max_ms = c * per_trial_ms + 1500u;
			struct sup_msg pm = { SUP_MSG_MAGIC, M08_T_POLL, 0, max_ms, 0, 0, 0 };
			struct sup_ack pa;
			shm->stop = 0;
			__sync_synchronize();
			if (write(sup_fd, &pm, sizeof(pm)) != (ssize_t)sizeof(pm)) { res[m].note = "POLL not sent"; break; }
			for (unsigned w = 0; w < 5000 && shm->poll_state != M08_POLL_RUNNING; w++)
				usleep(1000);                              /* wait until hart 4 is really polling */
			if (shm->poll_state != M08_POLL_RUNNING) { res[m].note = "hart 4 did not start polling within 5 s"; break; }
			run_trials(m, trial_flag, done, c, samples, &ns, hw, &nhw, 0);
			shm->stop = 1;
			__sync_synchronize();
			struct pollfd p = { .fd = sup_fd, .events = POLLIN };
			int got = 0;
			uint64_t t0 = mono_ns();
			while (!got && mono_ns() - t0 < (uint64_t)(max_ms + 3000u) * 1000000ull) {
				if (poll(&p, 1, 100) > 0 && read(sup_fd, &pa, sizeof(pa)) == (ssize_t)sizeof(pa) &&
				    pa.magic == SUP_ACK_MAGIC && pa.type == M08_T_POLL)
					got = 1;
			}
			if (!got) { res[m].note = "hart 4 did not end the polling loop: reboot it (see the page, risk list)"; break; }
			answers_total += pa.status_rx;
			done += c;
		}
		res[m].hart_count = answers_total;
		res[m].hart_expect = total;
	} else {
		unsigned chunk_done = 0;
		shm_init();
		if (do_setup()) { res[m].note = "setup failed"; free(samples); free(hw); return; }
		uint32_t ctrl = lat1[LAT_CTRL];
		if (((ctrl & LAT_CTRL_EN) || (lat1[LAT_STATUS] & 1u)) && !O.force) {
			res[m].note = "latency channel 1 is enabled or has an interrupt pending: not touched (--force to override)";
			free(samples); free(hw);
			return;
		}
		lat1[LAT_CTRL] = (ctrl & 0xFFFF0000u) | LAT_CTRL_CLR;        /* clear the channel's statistics, as P02 does */
		sleep_us(1000);
		run_trials(m, trial_irq, chunk_done, total, samples, &ns, hw, &nhw, 0);
		res[m].hart_count = shm->irq_count;
		res[m].hart_expect = total;
		(void)chunk_done;
	}
	finish_stats(m, samples, ns, hw, nhw);
	free(samples);
	free(hw);
}

/* ---- report ---------------------------------------------------------------------------------------- */
static double us(uint64_t ns) { return (double)ns / 1000.0; }

static void report(uint64_t timer_cost)
{
	printf("\nround trip in microseconds, %u trials per method after %u warm-up, timeout %u us, grace %u ms, gap %u us, cpu %u\n",
	       O.n, O.warm, O.timeout_us, O.grace_ms, O.gap_us, O.cpu);
	printf("(clock_gettime cost on this cpu: about %llu ns, included in every figure)\n\n", (unsigned long long)timer_cost);
	printf("%-12s %6s %5s %5s %9s %9s %9s %9s\n", "method", "ok", "late", "lost", "min", "median", "p95", "max");
	for (int m = 0; m < M_COUNT; m++) {
		if (!res[m].ran)
			continue;
		if (res[m].st.n)
			printf("%-12s %6u %5u %5u %9.1f %9.1f %9.1f %9.1f\n", mname[m], res[m].ok, res[m].late, res[m].lost,
			       us(res[m].st.min), us(res[m].st.median), us(res[m].st.p95), us(res[m].st.max));
		else
			printf("%-12s %6u %5u %5u %9s %9s %9s %9s\n", mname[m], res[m].ok, res[m].late, res[m].lost, "-", "-", "-", "-");
		if (res[m].note)
			printf("             note: %s\n", res[m].note);
		if (res[m].hart_count != 0xFFFFFFFFu)
			printf("             hart 4 counted %u answers for %u requests%s\n", res[m].hart_count, res[m].hart_expect,
			       res[m].hart_count == res[m].hart_expect ? "" : "  <-- differs");
		if (m == M_IRQ && res[m].hw_n)
			printf("             fabric reading, interrupt raised to acknowledged: min %llu median %llu p95 %llu max %llu cycles (%u samples; 20 ns per cycle if PCLK is 50 MHz)\n",
			       (unsigned long long)res[m].hw.min, (unsigned long long)res[m].hw.median, (unsigned long long)res[m].hw.p95,
			       (unsigned long long)res[m].hw.max, res[m].hw_n);
	}
	FILE *j = O.json ? fopen(O.json, "w") : NULL;
	for (int m = 0; m < M_COUNT; m++) {
		if (!res[m].ran)
			continue;
		char line[700];
		snprintf(line, sizeof(line),
			 "{\"experiment\": \"m08\", \"method\": \"%s\", \"cpu\": %u, \"trials\": %u, \"ok\": %u, \"late\": %u, \"lost\": %u, "
			 "\"min_us\": %.1f, \"median_us\": %.1f, \"p95_us\": %.1f, \"max_us\": %.1f, \"timeout_us\": %u, \"gap_us\": %u, "
			 "\"hart4_answers\": %d, \"sim\": %s}\n",
			 mname[m], O.cpu, res[m].trials, res[m].ok, res[m].late, res[m].lost, us(res[m].st.min), us(res[m].st.median),
			 us(res[m].st.p95), us(res[m].st.max), O.timeout_us, O.gap_us,
			 res[m].hart_count == 0xFFFFFFFFu ? -1 : (int)res[m].hart_count, O.sim ? "true" : "false");
		fputs(line, stdout);
		if (j)
			fputs(line, j);
	}
	if (j)
		fclose(j);
}

static uint64_t timer_cost_ns(void)
{
	uint64_t best = ~0ull;
	for (int i = 0; i < 2000; i++) {
		uint64_t a = mono_ns(), b = mono_ns();
		if (b - a < best)
			best = b - a;
	}
	return best;
}

static int pin(unsigned cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: m08_rtt [-n trials] [-w warmup] [-c cpu] [-t timeout_us] [-g grace_ms] [-d gap_us] [-m list] [--csv file] [--json file]\n"
		"               [--no-hwlat] [--force] [--probe] [--sim [--sim-drop N] [--sim-late N] [--sim-late-us US] [--sim-extra-us US]]\n"
		"  -m  comma list of rpmsg-query, rpmsg-ping, flag, irq (default: all four)\n"
		"  --probe checks that hart 4 runs the M08 variant and that latency channel 1 is idle, then exits (nothing is measured)\n"
		"  --sim runs against stand-ins in this process (PC tests); without it the board is used\n");
	exit(2);
}

int main(int argc, char **argv)
{
	O.methods = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
#define NEXT (++i < argc ? argv[i] : (usage(), (char *)0))
		if (!strcmp(a, "-n")) O.n = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "-w")) O.warm = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "-c")) O.cpu = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "-t")) O.timeout_us = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "-g")) O.grace_ms = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "-d")) O.gap_us = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "--csv")) O.csv = NEXT;
		else if (!strcmp(a, "--json")) O.json = NEXT;
		else if (!strcmp(a, "--no-hwlat")) O.hwlat = 0;
		else if (!strcmp(a, "--force")) O.force = 1;
		else if (!strcmp(a, "--sim")) O.sim = 1;
		else if (!strcmp(a, "--probe")) O.probe = 1;
		else if (!strcmp(a, "--sim-drop")) O.sim_drop_every = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "--sim-late")) O.sim_late_every = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "--sim-late-us")) O.sim_late_us = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "--sim-production")) O.sim_production = 1;
		else if (!strcmp(a, "--sim-chan-busy")) O.sim_chan_busy = 1;
		else if (!strcmp(a, "--sim-extra-us")) O.sim_extra_us = (unsigned)strtoul(NEXT, NULL, 0);
		else if (!strcmp(a, "-m")) {
			char *list = strdup(NEXT), *save = NULL;
			for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
				int found = 0;
				for (int m = 0; m < M_COUNT; m++)
					if (!strcmp(t, mname[m])) { O.methods |= 1u << m; found = 1; }
				if (!found) { fprintf(stderr, "unknown method %s\n", t); usage(); }
			}
			free(list);
		} else usage();
	}
	if (!O.methods)
		O.methods = (1u << M_COUNT) - 1;
	if (!O.n || O.n > 1000000 || O.timeout_us == 0 || O.timeout_us > 1000000)
		usage();

	if (O.sim && sim_start()) {                     /* the stand-in threads start before pinning, so they keep all cpus */
		perror("sim");
		return 1;
	}
	if (pin(O.cpu) < 0) {
		perror("sched_setaffinity");
		return 1;
	}
	if (O.sim) {
		/* started above */
	} else if (board_start(O.probe || !!(O.methods & ((1u << M_FLAG) | (1u << M_IRQ))), O.probe || !!(O.methods & (1u << M_IRQ)))) {
		return 1;
	}
	if (O.csv) {
		csv = fopen(O.csv, "w");
		if (!csv) { perror(O.csv); return 1; }
		fprintf(csv, "method,seq,phase,class,rtt_ns\n");
	}
	if (O.probe) {
		int rc = 0;
		shm_init();
		if (do_setup()) {
			rc = 1;
		} else {
			uint32_t ctrl = lat1[LAT_CTRL], st = lat1[LAT_STATUS];
			printf("probe: hart 4 sees the shared block at 0x%llx (magic ok), M08 variant running\n", (unsigned long long)shm_phys);
			printf("probe: latency channel 1: CTRL 0x%08x (ENABLE %u), STATUS 0x%x (pending %u)%s\n", ctrl, ctrl & 1u, st, st & 1u,
			       ((ctrl & 1u) || (st & 1u)) ? "  <-- not idle: the irq method will refuse to run" : "");
		}
		if (O.sim)
			sim_stop();
		return rc;
	}
	uint64_t tc = timer_cost_ns();
	printf("m08_rtt: cpu %u (Linux cpu N is hart N+1), shared block at 0x%llx, %s\n", O.cpu, (unsigned long long)shm_phys,
	       O.sim ? "SIMULATED hart 4 and fabric (threads in this process)" : "board");
	for (int m = 0; m < M_COUNT; m++)
		if (O.methods & (1u << m)) {
			printf("running %s ...\n", mname[m]);
			fflush(stdout);
			run_method(m);
		}
	if (O.sim)
		sim_stop();
	if (csv)
		fclose(csv);
	report(tc);
	return 0;
}
