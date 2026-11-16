// SPDX-License-Identifier: MIT
/*
 * station - the Linux side of the four-core monitoring station (experiment amp-station).
 *
 *   station init   [opts]      create the shared memory (control block, three rings)
 *   station acq    [opts]      CPU0 (hart 1): acquisition, real-time capture into ring A
 *   station ana    [opts]      CPU1 (hart 2): analysis, owns the RPMsg endpoint to hart 4
 *   station tel    [opts]      CPU2 (hart 3): telemetry and operator
 *   station hello  --cpu N     roll call: one HELLO from Linux cpu N to hart 4
 *   station listen             roll call: ask hart 4 to speak and print what it says
 *   station summary            print the shared counters (used by the stop script)
 *
 * Data path        fabric stream source -> DMA -> [acq] -> ring A -> [ana] -> ring B -> [tel]
 * Command path     operator FIFO -> [tel] -> ring C -> [ana] -> RPMsg -> hart 4
 * Protection path  [ana] -> RPMsg (heartbeat, features) -> hart 4 -> CTRL.ABORT on the source
 *                  hart 4 -> RPMsg notice -> [ana] -> ring B -> [tel]
 * See the module M10 page for the figures; every tunable is in struct cfg below and is printed
 * at start-up by each process.
 */
#include "station_common.h"
#include "station_link.h"
#include "station_fabric.h"
#include <sched.h>
#include <signal.h>
#include <stdarg.h>

/* ------------------------------------------------------------------------------------------ */
/* configuration                                                                                 */
/* ------------------------------------------------------------------------------------------ */
struct cfg {
	const char *dir;            /* shared files */
	const char *link;           /* "rpmsg" or "unix:PATH" */
	const char *fabric;         /* "hw" or "sim" */
	const char *fabsim;         /* shared word file of the simulated source (default: <dir>/fabsim) */
	int cpu;                    /* pin to this cpu, -1 = do not pin */
	uint32_t rate_cycles;       /* real-time slot interval, 8 ns cycles: 125000 = 1 ms */
	uint32_t run_slots;         /* slots per run in normal operation */
	uint32_t inject_run_slots;  /* slots per run while a fault is being injected */
	uint32_t inject_frame;      /* index of the corrupted frame within an injection run */
	uint32_t win_ms;            /* analysis window */
	uint32_t hb_ms;             /* heartbeat (STATUS) period */
	uint32_t deadline_ms;       /* hart 4 deadline on any message */
	uint32_t max_lag;           /* frames the fabric may be ahead of the analysis */
	uint32_t lim_bad, lim_drops, lim_peak, lim_mean;   /* per-window limits on hart 4 */
	uint32_t xfer_ms;           /* wait for an acknowledgement */
	uint32_t cpu_hart_offset;   /* Linux cpu N is hart N + 1 */
	int sync_seq;               /* init: read the fabric's NEXT_SEQ into the shared control block (hardware source) */
};

static struct cfg cfg = {
	.dir = "/dev/shm/station", .link = "rpmsg", .fabric = "hw", .cpu = -1,
	.rate_cycles = 125000, .run_slots = 5000, .inject_run_slots = 100, .inject_frame = 10,
	.win_ms = 500, .hb_ms = 100, .deadline_ms = 500, .max_lag = 320,
	.lim_bad = 2, .lim_drops = 4,
	/* nominal FIR window (500 frames): peak 795,986,176, mean |y| 278,229,455 (station_model check) */
	.lim_peak = 900000000u, .lim_mean = 320000000u,
	.xfer_ms = 200, .cpu_hart_offset = 1,
};

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static struct station_ctl *ctl;
static const char *g_tag = "station";

static double rel_s(void)
{
	uint64_t t0 = ctl ? ctl->t0_ns : 0;
	return (double)(mono_ns() - t0) / 1e9;
}

static void say(const char *fmt, ...)
{
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("[%8.3f %s] %s\n", rel_s(), g_tag, buf);
	fflush(stdout);
}

static int stopping(void)
{
	return g_stop || (ctl && __atomic_load_n(&ctl->stop, __ATOMIC_RELAXED));
}

static void path_in(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s/%s", cfg.dir, name);
}

static int pin_to_cpu(void)
{
	if (cfg.cpu < 0)
		return 0;
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cfg.cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static int map_ctl(int init)
{
	char p[300];
	path_in(p, sizeof(p), "ctl");
	int fd = open(p, init ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDWR, 0666);
	if (fd < 0)
		return -1;
	if (init && ftruncate(fd, sizeof(*ctl)) < 0) { close(fd); return -1; }
	ctl = mmap(NULL, sizeof(*ctl), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (ctl == MAP_FAILED)
		return -1;
	if (init) {
		ctl->t0_ns = mono_ns();
		__atomic_store_n(&ctl->magic, CTL_MAGIC, __ATOMIC_SEQ_CST);
	} else if (ctl->magic != CTL_MAGIC) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int map_ring(struct ring *r, const char *name, int init, uint32_t slot, uint32_t n)
{
	char p[300];
	path_in(p, sizeof(p), name);
	return ring_map(r, p, init, slot, n);
}

static void print_cfg(void)
{
	say("config: slot %u us (%u cycles), %u slots per run, %u slots per injection run",
	    cfg.rate_cycles / 125, cfg.rate_cycles, cfg.run_slots, cfg.inject_run_slots);
	say("config: window %u ms, heartbeat %u ms, deadline %u ms, max lag %u frames", cfg.win_ms, cfg.hb_ms, cfg.deadline_ms, cfg.max_lag);
	say("config: limits per window: bad <= %u, drops <= %u, peak <= %u, mean <= %u", cfg.lim_bad, cfg.lim_drops, cfg.lim_peak, cfg.lim_mean);
}

/* ------------------------------------------------------------------------------------------ */
/* init                                                                                          */
/* ------------------------------------------------------------------------------------------ */
static int role_init(void)
{
	struct ring a, b, c;
	mkdir(cfg.dir, 0777);
	if (map_ctl(1) || map_ring(&a, "ringA", 1, RING_A_SLOT, RING_A_SLOTS) ||
	    map_ring(&b, "ringB", 1, RING_B_SLOT, RING_B_SLOTS) || map_ring(&c, "ringC", 1, RING_C_SLOT, RING_C_SLOTS)) {
		perror("init");
		return 1;
	}
	if (cfg.sync_seq) {
		/* The analysis arms hart 4 before acquisition starts, and hart 4 compares the sequence number Linux reports
		   with the fabric's NEXT_SEQ. The fabric counter keeps its value from run to run (only a reset zeroes it),
		   so Linux has to be told where it stands before the first message. */
		struct fabric *fab = fabric_hw_open();
		if (!fab) { perror("init: fabric"); return 1; }
		ctl->acq_next_seq = fab->next_seq(fab);
		printf("fabric NEXT_SEQ %u taken as the starting sequence number\n", ctl->acq_next_seq);
	}
	printf("shared memory in %s: ring A %u x %u B, ring B %u x %u B, ring C %u x %u B\n", cfg.dir,
	       RING_A_SLOTS, RING_A_SLOT, RING_B_SLOTS, RING_B_SLOT, RING_C_SLOTS, RING_C_SLOT);
	return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* acquisition (CPU0)                                                                            */
/* ------------------------------------------------------------------------------------------ */
static int role_acq(void)
{
	struct ring ra;
	struct fabric *fab;
	char p[300];
	uint32_t run_id = 0, epoch_seen;
	uint32_t drops0, last_inject = 0;

	g_tag = "cpu0 acq";
	if (map_ctl(0) || map_ring(&ra, "ringA", 0, RING_A_SLOT, RING_A_SLOTS)) { perror("acq: shared memory"); return 1; }
	if (pin_to_cpu()) { perror("sched_setaffinity"); return 1; }
	crc_init();
	if (!strcmp(cfg.fabric, "sim")) {
		if (cfg.fabsim)
			snprintf(p, sizeof(p), "%s", cfg.fabsim);
		else
			path_in(p, sizeof(p), "fabsim");
		fab = fabric_sim_open(p);
	} else {
		fab = fabric_hw_open();
	}
	if (!fab) { say("cannot open the data source"); return 1; }
	ctl->acq_pid = (uint32_t)getpid();
	say("pinned to cpu %d (running on cpu %d), source: %s", cfg.cpu, sched_getcpu(), cfg.fabric);
	print_cfg();
	epoch_seen = ctl->epoch;
	drops0 = fab->drops(fab);
	ctl->acq_next_seq = fab->next_seq(fab);

	while (!stopping()) {
		/* hart 4 is SAFE (its notice was seen by analysis) or the source was aborted: no new run before the operator's reset */
		if (ctl->acq_state != ACQ_SAFE_WAIT && ctl->epoch == epoch_seen && __atomic_load_n(&ctl->hart_state, __ATOMIC_ACQUIRE) == ST_S_SAFE) {
			say("hart 4 is SAFE: not starting a run, waiting for reset");
			__atomic_store_n(&ctl->acq_state, ACQ_SAFE_WAIT, __ATOMIC_RELEASE);
		}
		if (ctl->acq_state == ACQ_SAFE_WAIT) {
			/* the source was stopped by hart 4: wait for the operator's reset (analysis bumps the epoch) */
			if (ctl->epoch == epoch_seen) {
				usleep(20000);
				continue;
			}
			epoch_seen = ctl->epoch;
			say("reset accepted (epoch %u): restarting the source", epoch_seen);
		}
		uint32_t inject = __atomic_load_n(&ctl->inject, __ATOMIC_RELAXED);
		struct run_cfg rc = {
			.slots = inject ? cfg.inject_run_slots : cfg.run_slots, .rate_cycles = cfg.rate_cycles,
			.fault = inject ? 1u : 0u, .fault_frame = cfg.inject_frame,
		};
		if (fab->start_run(fab, &rc)) { say("start_run failed"); break; }
		run_id++;
		ctl->acq_runs++;
		__atomic_store_n(&ctl->acq_state, ACQ_RUNNING, __ATOMIC_RELEASE);
		if (ctl->acq_runs == 1 || inject != last_inject)
			say("run %u started: %u slots%s", run_id, rc.slots, inject ? ", CRC fault at frame 10 of every run" : "");
		last_inject = inject;
		int first = 1, r, err = 0, aborted_here = 0;
		for (;;) {
			struct frame_slot *slot = ring_reserve(&ra);
			uint32_t seq;
			r = fab->next(fab, slot ? slot->w : NULL, &seq, 20);
			if (r == 1) {
				ctl->acq_frames++;
				__atomic_store_n(&ctl->acq_next_seq, seq + 1, __ATOMIC_RELEASE);
				if (slot) {
					slot->t_ns = mono_ns();
					slot->run_id = run_id;
					slot->flags = first ? 1u : 0u;
					ring_commit(&ra);
				} else {
					ctl->acq_ring_overruns++;       /* ring A full: dropped here, a gap for analysis */
				}
				first = 0;
			} else if (r == 2) {
				break;
			} else if (r < 0) {
				err = 1;
				break;
			} else if (stopping()) {
				fab->abort_run(fab);            /* orderly shutdown: finish the frame in progress, stop */
			}
			/* Belt and braces: an ABORT that hart 4 writes between two runs is cleared by the next START (RTL:
			   abort_pend is cleared at start), so once analysis has passed the trip on, acquisition repeats it. */
			if (!aborted_here && ctl->epoch == epoch_seen && __atomic_load_n(&ctl->hart_state, __ATOMIC_ACQUIRE) == ST_S_SAFE) {
				fab->abort_run(fab);
				aborted_here = 1;
			}
		}
		ctl->acq_fab_drops = fab->drops(fab) - drops0;
		__atomic_store_n(&ctl->acq_next_seq, fab->next_seq(fab), __ATOMIC_RELEASE);
		if (err) {
			say("run %u: no progress for 2 s, aborting the source", run_id);
			fab->abort_run(fab);
			usleep(100000);
		} else if (fab->ended_by_abort(fab) && !stopping()) {
			ctl->acq_aborts++;
			say("run %u ended by ABORT (hart 4 trip), waiting for reset; frames %llu overruns %llu",
			    run_id, (unsigned long long)ctl->acq_frames, (unsigned long long)ctl->acq_ring_overruns);
			__atomic_store_n(&ctl->acq_state, ACQ_SAFE_WAIT, __ATOMIC_RELEASE);
		}
	}
	__atomic_store_n(&ctl->acq_state, ACQ_STOPPED, __ATOMIC_RELEASE);
	say("stopped: %llu frames in %llu runs, ring overruns %llu, fabric drops %llu", (unsigned long long)ctl->acq_frames,
	    (unsigned long long)ctl->acq_runs, (unsigned long long)ctl->acq_ring_overruns, (unsigned long long)ctl->acq_fab_drops);
	fab->close(fab);
	return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* analysis (CPU1)                                                                               */
/* ------------------------------------------------------------------------------------------ */
struct ana {
	struct ring ra, rb, rc;
	int link;
	struct verifier ver;
	struct window win;
	uint32_t win_id, tel_seq, report_seq;
	uint32_t rtt_us;
	uint64_t link_lost_ns;
};

static struct ana A;

static void emit(uint32_t type, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, const char *text,
		 const uint32_t *wv /* optional 12 words */)
{
	struct tel_rec *t = ring_reserve(&A.rb);
	if (!t)
		return;
	memset(t, 0, sizeof(*t));
	t->type = type;
	t->seq = A.tel_seq++;
	t->t_ns = mono_ns();
	if (wv)
		memcpy(t->w, wv, sizeof(t->w));
	else {
		t->w[0] = w0; t->w[1] = w1; t->w[2] = w2; t->w[3] = w3;
	}
	if (text)
		snprintf(t->text, sizeof(t->text), "%s", text);
	ring_commit(&A.rb);
}

static void ana_note_ack(const struct st_ack *a)
{
	ctl->hart_state = a->state;
	ctl->hart_trips = a->trips;
	ctl->hart_reason = a->last_reason;
	ctl->hart_events = a->events;
}

static void ana_flush(void)
{
	while (ring_peek(&A.ra))
		ring_release(&A.ra);
	verifier_resync(&A.ver);
	window_reset(&A.win, mono_ns());
	A.report_seq = __atomic_load_n(&ctl->acq_next_seq, __ATOMIC_ACQUIRE);
}

static void ana_notice(const struct st_ack *a, void *arg)
{
	(void)arg;
	char r[48];
	if (a->type == ST_T_TRIP_NOTICE) {
		st_reason_str(a->last_reason, r, sizeof(r));
		ana_note_ack(a);
		say("TRIP notice from hart 4: %s, trip #%u, tick %u ms; ring A flushed", r, a->trips, a->uptime_ms);
		ana_flush();
		emit(TR_EVENT, EV_TRIP, a->last_reason, a->trips, a->uptime_ms, "hart 4 tripped", NULL);
	} else if (a->type == ST_T_SPEAK) {
		say("hart 4 says: %.16s", a->text);
	}
}

static int ana_xfer(struct st_msg *m, struct st_ack *ack)
{
	uint64_t t0 = mono_ns();
	int r = link_xfer(A.link, m, ack, (int)cfg.xfer_ms, ana_notice, NULL);
	A.rtt_us = (uint32_t)((mono_ns() - t0) / 1000);
	if (r) {
		ctl->ana_xfer_fail++;
		if (mono_ns() - A.link_lost_ns > 5000000000ull) {
			say("no acknowledgement from hart 4 for message type %u within %u ms", m->type, cfg.xfer_ms);
			emit(TR_EVENT, EV_LINK_LOST, m->type, 0, 0, "no answer from hart 4", NULL);
			A.link_lost_ns = mono_ns();
		}
		return -1;
	}
	ana_note_ack(ack);
	return 0;
}

static void ana_send_limits(void)
{
	struct st_msg m;
	struct st_ack ack;
	st_msg_init(&m, ST_T_SET_LIMITS);
	m.x[0] = cfg.lim_bad; m.x[1] = cfg.lim_drops; m.x[2] = cfg.lim_peak; m.x[3] = cfg.lim_mean;
	ana_xfer(&m, &ack);
}

static void ana_close_window(void)
{
	struct st_msg m;
	struct st_ack ack = { 0 };
	uint32_t w[12] = { 0 };

	A.win_id++;
	st_msg_init(&m, ST_T_FEATURES);
	m.x[0] = A.win_id; m.x[1] = A.win.frames; m.x[2] = A.win.bad; m.x[3] = A.win.gaps;
	m.x[4] = A.win.peak; m.x[5] = window_mean(&A.win); m.x[6] = cfg.win_ms;
	int ok = ana_xfer(&m, &ack) == 0;
	ctl->ana_windows++;
	w[0] = A.win_id; w[1] = A.win.frames; w[2] = A.win.bad; w[3] = A.win.gaps; w[4] = A.win.peak;
	w[5] = window_mean(&A.win); w[6] = A.rtt_us; w[7] = ok ? ack.state : 0xFFu; w[8] = ack.trips;
	w[9] = ack.last_reason; w[10] = ack.events; w[11] = cfg.win_ms;
	emit(TR_WINDOW, 0, 0, 0, 0, NULL, w);
	window_reset(&A.win, mono_ns());
}

static void ana_command(const struct cmd_rec *c)
{
	struct st_msg m;
	struct st_ack ack;

	switch (c->type) {
	case CMD_RESET:
		st_msg_init(&m, ST_T_RESET);
		if (ana_xfer(&m, &ack) == 0 && ack.state == ST_S_ARMED) {
			ana_flush();                                /* nothing from before the trip may reach the new window */
			__atomic_store_n(&ctl->inject, INJ_NONE, __ATOMIC_RELAXED);
			__atomic_add_fetch(&ctl->epoch, 1, __ATOMIC_RELEASE);
			say("RESET: hart 4 %s again (trips so far %u), ring A flushed, source released",
			    st_state_name(ack.state), ack.trips);
			emit(TR_EVENT, EV_RESET_OK, ack.last_reason, ack.trips, ack.uptime_ms, "reset accepted", NULL);
		} else {
			say("RESET refused or lost (hart 4 state %s)", st_state_name(ack.state));
			emit(TR_EVENT, EV_RESET_REFUSED, 0, 0, 0, "reset refused", NULL);
		}
		break;
	case CMD_LIMITS:
		cfg.lim_bad = c->a;
		cfg.lim_drops = c->b;
		ana_send_limits();
		say("limits changed by operator: bad <= %u, drops <= %u", cfg.lim_bad, cfg.lim_drops);
		emit(TR_EVENT, EV_LIMITS, c->a, c->b, 0, "limits changed", NULL);
		break;
	case CMD_QUERY:
		st_msg_init(&m, ST_T_QUERY);
		if (ana_xfer(&m, &ack) == 0)
			say("QUERY: hart 4 state %s, trips %u, events %u, deadline misses %u, tick %u ms",
			    st_state_name(ack.state), ack.trips, ack.events, ack.deadline_miss, ack.uptime_ms);
		break;
	}
}

static int role_ana(void)
{
	struct st_msg m;
	struct st_ack ack;
	uint64_t now, next_hb, win_end;

	g_tag = "cpu1 ana";
	if (map_ctl(0) || map_ring(&A.ra, "ringA", 0, RING_A_SLOT, RING_A_SLOTS) ||
	    map_ring(&A.rb, "ringB", 0, RING_B_SLOT, RING_B_SLOTS) || map_ring(&A.rc, "ringC", 0, RING_C_SLOT, RING_C_SLOTS)) {
		perror("ana: shared memory");
		return 1;
	}
	if (pin_to_cpu()) { perror("sched_setaffinity"); return 1; }
	crc_init();
	A.link = link_open(cfg.link);
	if (A.link < 0) { perror("ana: link to hart 4"); return 1; }
	verifier_init(&A.ver);
	ctl->ana_pid = (uint32_t)getpid();
	say("pinned to cpu %d (running on cpu %d); owns the link to hart 4", cfg.cpu, sched_getcpu());
	print_cfg();

	st_msg_init(&m, ST_T_ARM);
	m.deadline_ms = cfg.deadline_ms;
	m.max_lag = cfg.max_lag;
	if (ana_xfer(&m, &ack) == 0) {
		say("hart 4 armed: state %s, uptime %u ms", st_state_name(ack.state), ack.uptime_ms);
		emit(TR_EVENT, EV_ARMED, 0, ack.trips, ack.uptime_ms, "hart 4 armed", NULL);
	} else {
		say("hart 4 did not acknowledge ARM");
	}
	ana_send_limits();
	A.report_seq = ctl->acq_next_seq;
	window_reset(&A.win, mono_ns());
	now = mono_ns();
	next_hb = now + (uint64_t)cfg.hb_ms * 1000000ull;
	win_end = now + (uint64_t)cfg.win_ms * 1000000ull;

	while (!stopping()) {
		link_drain(A.link, ana_notice, NULL);
		const struct cmd_rec *c;
		while ((c = ring_peek(&A.rc))) {
			struct cmd_rec cmd = *c;
			ring_release(&A.rc);
			ana_command(&cmd);
		}
		const struct frame_slot *f = ring_peek(&A.ra);
		if (f) {
			int bad = verify_frame(&A.ver, &A.win, f);
			A.report_seq = f->w[1] + 1;
			ctl->ana_frames = A.ver.total_frames;
			ctl->ana_bad = A.ver.total_bad;
			ctl->ana_gaps = A.ver.total_gaps;
			if (bad && A.ver.total_bad <= 3)
				say("bad frame: seq %u (magic %08x, crc %s)", f->w[1], f->w[0],
				    crc32_words(f->w, ST_FRAME_WORDS - 1) == f->w[ST_FRAME_WORDS - 1] ? "ok" : "mismatch");
			ring_release(&A.ra);
		} else {
			ring_wait(&A.ra, 10);
		}
		now = mono_ns();
		if (now >= next_hb) {
			st_msg_init(&m, ST_T_STATUS);
			m.burst = (uint32_t)ctl->ana_hb++;
			m.next_seq = A.report_seq;
			m.errors = (uint32_t)A.ver.total_bad;
			ana_xfer(&m, &ack);
			next_hb = now + (uint64_t)cfg.hb_ms * 1000000ull;
		}
		if (now >= win_end) {
			ana_close_window();
			win_end = mono_ns() + (uint64_t)cfg.win_ms * 1000000ull;
		}
	}
	st_msg_init(&m, ST_T_DISARM);
	ana_xfer(&m, &ack);
	say("stopped: %llu frames verified, %llu bad, %llu gaps, %llu windows, %llu failed transfers",
	    (unsigned long long)A.ver.total_frames, (unsigned long long)A.ver.total_bad,
	    (unsigned long long)A.ver.total_gaps, (unsigned long long)ctl->ana_windows, (unsigned long long)ctl->ana_xfer_fail);
	return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* telemetry and operator (CPU2)                                                                 */
/* ------------------------------------------------------------------------------------------ */
static const char *const acq_name[] = { "starting", "running", "stopped-by-abort", "stopped" };

static void tel_send_cmd(struct ring *rc, uint32_t type, uint32_t a, uint32_t b)
{
	struct cmd_rec *c = ring_reserve(rc);
	if (!c) {
		say("command ring full, command dropped");
		return;
	}
	*c = (struct cmd_rec){ type, a, b, 0 };
	ring_commit(rc);
}

static void tel_dashboard(uint64_t *last_frames, uint64_t *last_t, uint64_t last_win_ns, const uint32_t *lw)
{
	uint64_t now = mono_ns(), fr = ctl->ana_frames;
	double dt = (double)(now - *last_t) / 1e9;
	double rate = dt > 0 ? (double)(fr - *last_frames) / dt : 0;
	char silent[32] = "";
	*last_frames = fr;
	*last_t = now;
	if (last_win_ns && now - last_win_ns > 2ull * cfg.win_ms * 1000000ull)
		snprintf(silent, sizeof(silent), " SILENT %.1fs", (double)(now - last_win_ns) / 1e9);
	/* one line, at most 108 characters: the replay figure shows no more. "src" is what acquisition sees
	   (STOP: the source was aborted, even if analysis is frozen); "h4" is the last state analysis heard. */
	static const char *const src_name[] = { "init", "RUN", "STOP", "off" };
	printf("[%5.1f] frames %llu bad %llu drop %llu %.0f/s ovr %llu pk %uM avg %uM src %s h4 %s trips %u%s\n",
	       rel_s(), (unsigned long long)fr, (unsigned long long)ctl->ana_bad, (unsigned long long)ctl->ana_gaps, rate,
	       (unsigned long long)ctl->acq_ring_overruns, lw[4] / 1000000u, lw[5] / 1000000u,
	       src_name[ctl->acq_state & 3], st_state_name(ctl->hart_state), ctl->hart_trips, silent);
	fflush(stdout);
}

static int role_tel(void)
{
	struct ring ra, rb, rc;
	char p[300], line[160];
	FILE *csv;
	int fifo;
	uint32_t lw[12] = { 0 };
	uint64_t last_frames = 0, last_t = mono_ns(), next_dash, last_win_ns = 0;
	size_t fill = 0;

	g_tag = "cpu2 tel";
	if (map_ctl(0) || map_ring(&ra, "ringA", 0, RING_A_SLOT, RING_A_SLOTS) ||
	    map_ring(&rb, "ringB", 0, RING_B_SLOT, RING_B_SLOTS) || map_ring(&rc, "ringC", 0, RING_C_SLOT, RING_C_SLOTS)) {
		perror("tel: shared memory");
		return 1;
	}
	if (pin_to_cpu()) { perror("sched_setaffinity"); return 1; }
	path_in(p, sizeof(p), "cmd");
	unlink(p);
	if (mkfifo(p, 0666) || (fifo = open(p, O_RDWR | O_NONBLOCK)) < 0) { perror("tel: command FIFO"); return 1; }
	path_in(p, sizeof(p), "telemetry.csv");
	csv = fopen(p, "w");
	if (csv)
		fprintf(csv, "t_s,window,frames,bad,drops,peak,mean,rtt_us,hart_state,hart_trips,ring_overruns,fabric_drops\n");
	ctl->tel_pid = (uint32_t)getpid();
	say("pinned to cpu %d (running on cpu %d); commands are read from %s/cmd", cfg.cpu, sched_getcpu(), cfg.dir);
	say("commands: reset | limits <max_bad> <max_drops> | inject crc | inject off | status | quit");
	next_dash = mono_ns() + 1000000000ull;

	while (!stopping()) {
		ring_wait(&rb, 50);
		const struct tel_rec *t;
		while ((t = ring_peek(&rb))) {
			struct tel_rec r = *t;
			ring_release(&rb);
			if (r.type == TR_WINDOW) {
				memcpy(lw, r.w, sizeof(lw));
				last_win_ns = mono_ns();
				ctl->tel_windows++;
				if (csv) {
					fprintf(csv, "%.3f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu\n", rel_s(), r.w[0], r.w[1], r.w[2], r.w[3],
						r.w[4], r.w[5], r.w[6], r.w[7], r.w[8], (unsigned long long)ctl->acq_ring_overruns,
						(unsigned long long)ctl->acq_fab_drops);
					fflush(csv);
				}
			} else {
				char rs[48];
				st_reason_str(r.w[1], rs, sizeof(rs));
				switch (r.w[0]) {
				case EV_TRIP:
					printf("[%5.1f] *** TRIP reported by hart 4: reason %s, trip #%u, hart 4 tick %u ms ***\n", rel_s(), rs, r.w[2], r.w[3]);
					break;
				case EV_RESET_OK:
					printf("[%5.1f] reset accepted by hart 4 (trips so far %u), station resumes\n", rel_s(), r.w[2]);
					break;
				case EV_RESET_REFUSED:
					printf("[%5.1f] reset NOT accepted\n", rel_s());
					break;
				default:
					printf("[%5.1f] event: %s\n", rel_s(), r.text);
				}
				fflush(stdout);
			}
		}
		/* operator commands */
		ssize_t n = read(fifo, line + fill, sizeof(line) - 1 - fill);
		if (n > 0) {
			fill += (size_t)n;
			char *nl;
			line[fill] = 0;
			while ((nl = strchr(line, '\n'))) {
				*nl = 0;
				unsigned a, b;
				char w[16];
				printf("[%5.1f] operator> %s\n", rel_s(), line);
				if (!strcmp(line, "reset")) tel_send_cmd(&rc, CMD_RESET, 0, 0);
				else if (sscanf(line, "limits %u %u", &a, &b) == 2) tel_send_cmd(&rc, CMD_LIMITS, a, b);
				else if (sscanf(line, "inject %15s", w) == 1 && !strcmp(w, "crc")) {
					__atomic_store_n(&ctl->inject, INJ_CRC, __ATOMIC_RELAXED);
					printf("[%5.1f] fault injection on: the next runs carry a CRC fault every %u slots\n", rel_s(), cfg.inject_run_slots);
				} else if (sscanf(line, "inject %15s", w) == 1 && !strcmp(w, "off")) {
					__atomic_store_n(&ctl->inject, INJ_NONE, __ATOMIC_RELAXED);
				} else if (!strcmp(line, "status")) {
					printf("[%5.1f] status: source %s, hart 4 %s, trips %u, last reason %#x, frames %llu, bad %llu, drops %llu, overruns %llu\n",
					       rel_s(), acq_name[ctl->acq_state & 3], st_state_name(ctl->hart_state), ctl->hart_trips,
					       ctl->hart_reason, (unsigned long long)ctl->ana_frames, (unsigned long long)ctl->ana_bad,
					       (unsigned long long)ctl->ana_gaps, (unsigned long long)ctl->acq_ring_overruns);
				} else if (!strcmp(line, "quit")) {
					__atomic_store_n(&ctl->stop, 1, __ATOMIC_RELAXED);
				} else {
					printf("[%5.1f] unknown command\n", rel_s());
				}
				fflush(stdout);
				memmove(line, nl + 1, fill - (size_t)(nl + 1 - line));
				fill -= (size_t)(nl + 1 - line);
				line[fill] = 0;
			}
			if (fill >= sizeof(line) - 1)
				fill = 0;
		}
		if (mono_ns() >= next_dash) {
			tel_dashboard(&last_frames, &last_t, last_win_ns, lw);
			next_dash += 1000000000ull;
		}
	}
	printf("[%5.1f] summary: frames %llu, bad %llu, drops %llu, windows %llu, trips %u\n", rel_s(),
	       (unsigned long long)ctl->ana_frames, (unsigned long long)ctl->ana_bad, (unsigned long long)ctl->ana_gaps,
	       (unsigned long long)ctl->tel_windows, ctl->hart_trips);
	printf("[%5.1f] summary: ring overruns %llu, fabric drops %llu, source runs %llu, aborts %llu\n", rel_s(),
	       (unsigned long long)ctl->acq_ring_overruns, (unsigned long long)ctl->acq_fab_drops,
	       (unsigned long long)ctl->acq_runs, (unsigned long long)ctl->acq_aborts);
	fflush(stdout);
	if (csv)
		fclose(csv);
	return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* roll call                                                                                     */
/* ------------------------------------------------------------------------------------------ */
static double abs_s(void) { return (double)mono_ns() / 1e9; }

static int role_hello(uint32_t seq)
{
	struct st_msg m;
	struct st_ack ack;
	uint64_t t_us;
	int fd, cpu;
	char text[17];

	if (pin_to_cpu()) { perror("sched_setaffinity"); return 1; }
	cpu = sched_getcpu();
	if (cfg.cpu >= 0 && cpu != cfg.cpu) { fprintf(stderr, "running on cpu %d, not on the requested cpu %d\n", cpu, cfg.cpu); return 1; }
	fd = link_open(cfg.link);
	if (fd < 0) { perror("link to hart 4"); return 1; }
	snprintf(text, sizeof(text), "core %u here", (unsigned)(cpu + 1) % 10u);
	t_us = mono_ns() / 1000;
	st_msg_init(&m, ST_T_HELLO);
	m.burst = seq;
	m.next_seq = (uint32_t)cpu;
	m.errors = (uint32_t)cpu + cfg.cpu_hart_offset;
	m.deadline_ms = (uint32_t)t_us;
	m.max_lag = (uint32_t)(t_us >> 32);
	memcpy(&m.x[0], text, 16);
	printf("[%.3f cpu %d] sending HELLO #%u: hart %u (Linux cpu %d), text \"%s\"\n", abs_s(), cpu, seq, m.errors, cpu, text);
	fflush(stdout);
	if (link_xfer(fd, &m, &ack, 1000, NULL, NULL)) { printf("[%.3f cpu %d] no answer from hart 4\n", abs_s(), cpu); return 1; }
	printf("[%.3f cpu %d] hart 4 answered: \"%.16s\", its tick %u ms, %u of 3 Linux cpus heard so far\n",
	       abs_s(), cpu, ack.text, ack.uptime_ms, ack.status_rx);
	close(fd);
	return 0;
}

static int role_listen(void)
{
	struct st_msg m;
	struct st_ack ack;
	int fd = link_open(cfg.link);
	struct pollfd p;

	if (fd < 0) { perror("link to hart 4"); return 1; }
	st_msg_init(&m, ST_T_LISTEN);
	printf("[%.3f] asking hart 4 to speak (LISTEN)\n", abs_s());
	fflush(stdout);
	if (link_xfer(fd, &m, &ack, 1000, NULL, NULL)) { printf("[%.3f] no answer from hart 4\n", abs_s()); return 1; }
	printf("[%.3f] hart 4 acknowledged LISTEN, %u of 3 Linux cpus heard; waiting up to 5 s for its own message\n", abs_s(), ack.status_rx);
	fflush(stdout);
	p = (struct pollfd){ .fd = fd, .events = POLLIN };
	if (poll(&p, 1, 5000) > 0 && read(fd, &ack, sizeof(ack)) == (ssize_t)sizeof(ack) && ack.magic == ST_ACK_MAGIC && ack.type == ST_T_SPEAK) {
		printf("[%.3f] core 4 speaks: \"%.16s\" (hart 4 tick %u ms)\n", abs_s(), ack.text, ack.uptime_ms);
		close(fd);
		return 0;
	}
	printf("[%.3f] hart 4 said nothing\n", abs_s());
	close(fd);
	return 2;
}

/* ------------------------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	const char *role;
	uint32_t seq = 1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s init|acq|ana|tel|hello|listen|summary [--dir D] [--link rpmsg|unix:PATH] [--fabric hw|sim] [--cpu N] ...\n", argv[0]);
		return 2;
	}
	role = argv[1];
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && (i++, 1))
		if (!strcmp(a, "--sync-seq")) cfg.sync_seq = 1;
		else if (OPT("--dir")) cfg.dir = v;
		else if (OPT("--link")) cfg.link = v;
		else if (OPT("--fabric")) cfg.fabric = v;
		else if (OPT("--fabsim")) cfg.fabsim = v;
		else if (OPT("--cpu")) cfg.cpu = atoi(v);
		else if (OPT("--seq")) seq = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--rate")) cfg.rate_cycles = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--run-slots")) cfg.run_slots = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--inject-slots")) cfg.inject_run_slots = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--window-ms")) cfg.win_ms = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--heartbeat-ms")) cfg.hb_ms = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--deadline-ms")) cfg.deadline_ms = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--max-lag")) cfg.max_lag = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--lim-bad")) cfg.lim_bad = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--lim-drops")) cfg.lim_drops = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--lim-peak")) cfg.lim_peak = (uint32_t)strtoul(v, NULL, 0);
		else if (OPT("--lim-mean")) cfg.lim_mean = (uint32_t)strtoul(v, NULL, 0);
		else { fprintf(stderr, "unknown option %s\n", a); return 2; }
#undef OPT
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (!strcmp(role, "init")) return role_init();
	if (!strcmp(role, "acq")) return role_acq();
	if (!strcmp(role, "ana")) return role_ana();
	if (!strcmp(role, "tel")) return role_tel();
	if (!strcmp(role, "hello")) return role_hello(seq);
	if (!strcmp(role, "listen")) return role_listen();
	if (!strcmp(role, "summary")) {
		if (map_ctl(0)) { perror("summary"); return 1; }
		printf("frames %llu bad %llu gaps %llu trips %u overruns %llu fabric_drops %llu runs %llu aborts %llu\n",
		       (unsigned long long)ctl->ana_frames, (unsigned long long)ctl->ana_bad, (unsigned long long)ctl->ana_gaps,
		       ctl->hart_trips, (unsigned long long)ctl->acq_ring_overruns, (unsigned long long)ctl->acq_fab_drops,
		       (unsigned long long)ctl->acq_runs, (unsigned long long)ctl->acq_aborts);
		return 0;
	}
	fprintf(stderr, "unknown role %s\n", role);
	return 2;
}
