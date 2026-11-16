// SPDX-License-Identifier: MIT
/*
 * Host tests (x86, gcc) of the station's Linux parts and of the hart 4 logic. No board, no hardware.
 *   1 ring       one producer thread and one consumer thread, 2,000,000 records through a 256-slot ring in
 *                shared memory with futex wake-up: nothing lost, nothing duplicated, order kept, payload intact;
 *                then a slow consumer: the producer sees "full" and counts its drops, the books balance
 *   2 messages   layout of the 64-byte messages (word offsets as documented in pfdaq_station.h), encode and
 *                decode over a SOCK_SEQPACKET socketpair, sizes of the ring records
 *   3 crc        CRC-32 against zlib values computed independently
 *   4 features   frames built like the fabric builds them are verified: clean window, CRC fault, bad magic,
 *                sequence gap, repeated sequence number, payload bit flip; peak and mean of a clean window
 *   5 hart 4     the protection logic of pfdaq_station.h driven message by message (deadline, features over
 *                limit, sequence mismatch, trip once, reset, roll call)
 * "test_units dump" prints the first frames' words for the independent Python model (model_check.py).
 */
#include "station_common.h"
#include "station_link.h"
#include <pthread.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/socket.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- 1 ring */
#define N_REC 2000000u
struct rec { uint32_t seq; uint32_t chk; uint32_t pad[14]; };

static struct ring R;
static uint64_t prod_dropped;
static int prod_retry;

static void *producer(void *arg)
{
	(void)arg;
	for (uint32_t i = 0; i < N_REC; i++) {
		struct rec *r;
		while (!(r = ring_reserve(&R))) {
			if (!prod_retry) { prod_dropped++; goto next; }
			sched_yield();
		}
		r->seq = i;
		r->chk = i * 2654435761u;
		for (int k = 0; k < 14; k++) r->pad[k] = i + k;
		ring_commit(&R);
	next:;
	}
	return NULL;
}

static uint64_t cons_n, cons_bad, cons_order_err;
static uint32_t cons_slow;

static void *consumer(void *arg)
{
	uint32_t last = 0;
	int have = 0, idle = 0;
	(void)arg;
	while (idle < 200) {
		const struct rec *r = ring_peek(&R);
		if (!r) {
			if (ring_wait(&R, 10)) idle = 0; else idle++;
			continue;
		}
		idle = 0;
		if (r->chk != r->seq * 2654435761u) cons_bad++;
		for (int k = 0; k < 14; k++) if (r->pad[k] != r->seq + k) cons_bad++;
		if (have && r->seq <= last) cons_order_err++;
		last = r->seq;
		have = 1;
		cons_n++;
		ring_release(&R);
		if (cons_slow && (cons_n % 64) == 0) usleep(cons_slow);
	}
	return NULL;
}

static void test_ring(void)
{
	char path[] = "/tmp/claude-1000/amp/ringtest-XXXXXX";
	int fd = mkstemp(path);
	pthread_t p, c;
	close(fd);
	CHECK(ring_map(&R, path, 1, sizeof(struct rec), 256) == 0, "ring_map");
	/* a second mapping of the same file, as another process would have */
	struct ring R2;
	CHECK(ring_map(&R2, path, 0, sizeof(struct rec), 256) == 0, "second mapping");
	CHECK(ring_map(&R2, path, 0, 99, 256) != 0, "mapping with the wrong slot size is refused");

	prod_retry = 1; cons_slow = 0; cons_n = cons_bad = cons_order_err = 0;
	uint64_t t0 = mono_ns();
	pthread_create(&c, NULL, consumer, NULL);
	pthread_create(&p, NULL, producer, NULL);
	pthread_join(p, NULL);
	pthread_join(c, NULL);
	printf("ring: %u records, %.0f ns each\n", N_REC, (double)(mono_ns() - t0) / N_REC);
	CHECK(cons_n == N_REC, "received %llu of %u", (unsigned long long)cons_n, N_REC);
	CHECK(cons_bad == 0 && cons_order_err == 0, "corrupt %llu, out of order %llu", (unsigned long long)cons_bad, (unsigned long long)cons_order_err);
	CHECK(ring_count(&R) == 0, "ring empty at the end");

	/* slow consumer: the producer must never block and must count what it could not store */
	R.h->head = R.h->tail = 0;
	prod_retry = 0; prod_dropped = 0; cons_slow = 300; cons_n = cons_bad = cons_order_err = 0;
	pthread_create(&c, NULL, consumer, NULL);
	pthread_create(&p, NULL, producer, NULL);
	pthread_join(p, NULL);
	pthread_join(c, NULL);
	printf("ring, slow consumer: %llu stored, %llu dropped by the producer\n", (unsigned long long)cons_n, (unsigned long long)prod_dropped);
	CHECK(cons_n + prod_dropped == N_REC, "stored %llu + dropped %llu != %u", (unsigned long long)cons_n, (unsigned long long)prod_dropped, N_REC);
	CHECK(prod_dropped > 0, "the slow consumer should have caused drops");
	CHECK(cons_bad == 0 && cons_order_err == 0, "corrupt or reordered records with drops");
	unlink(path);
}

/* ---------------------------------------------------------------- 2 messages */
static void test_messages(void)
{
	struct st_msg m;
	struct st_ack a;
	CHECK(sizeof(m) == 64 && sizeof(a) == 64, "message sizes");
	CHECK(offsetof(struct st_msg, magic) == 0 && offsetof(struct st_msg, type) == 4 && offsetof(struct st_msg, burst) == 8 &&
	      offsetof(struct st_msg, next_seq) == 12 && offsetof(struct st_msg, errors) == 16 &&
	      offsetof(struct st_msg, deadline_ms) == 20 && offsetof(struct st_msg, max_lag) == 24 && offsetof(struct st_msg, x) == 28,
	      "st_msg word offsets (first seven words as struct pfdaq_msg)");
	CHECK(offsetof(struct st_ack, state) == 8 && offsetof(struct st_ack, uptime_ms) == 32 && offsetof(struct st_ack, events) == 36 &&
	      offsetof(struct st_ack, last_reason) == 40 && offsetof(struct st_ack, trips) == 44 && offsetof(struct st_ack, text) == 48,
	      "st_ack word offsets");
	CHECK(ST_MSG_MAGIC == 0x53444650u && ST_ACK_MAGIC == 0x41444650u, "magics are PFDS and PFDA");
	CHECK(RING_A_SLOT == 8 + 8 + ST_FRAME_BYTES && RING_B_SLOT == 112 && RING_C_SLOT == 16, "ring record sizes %u %u %u", RING_A_SLOT, RING_B_SLOT, RING_C_SLOT);

	/* over a socketpair: write a HELLO, read it back unchanged; the receiver rejects a wrong magic */
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
	st_msg_init(&m, ST_T_HELLO);
	m.burst = 7; m.next_seq = 2; m.errors = 3; m.deadline_ms = 0xdeadbeef; m.max_lag = 0x12;
	memcpy(&m.x[0], "core 3 here\0\0\0\0\0", 16);
	CHECK(write(sv[0], &m, sizeof(m)) == sizeof(m), "send");
	struct st_msg r;
	CHECK(read(sv[1], &r, sizeof(r)) == sizeof(r) && memcmp(&r, &m, sizeof(m)) == 0, "message survives the socket unchanged");
	CHECK(r.magic == ST_MSG_MAGIC && r.type == ST_T_HELLO && r.burst == 7 && r.next_seq == 2 && r.errors == 3 &&
	      !strncmp((char *)&r.x[0], "core 3 here", 16), "decoded fields");
	/* boundaries are kept: two messages are two reads */
	CHECK(write(sv[0], &m, sizeof(m)) == sizeof(m) && write(sv[0], &m, sizeof(m)) == sizeof(m), "send two");
	CHECK(read(sv[1], &r, sizeof(r)) == sizeof(r) && read(sv[1], &r, sizeof(r)) == sizeof(r), "two reads, two messages");
	close(sv[0]); close(sv[1]);
}

/* ---------------------------------------------------------------- 3 crc */
static void test_crc(void)
{
	crc_init();
	uint32_t z[1] = { 0 };
	uint32_t t[3] = { 1, 2, 3 };
	uint32_t u[3] = { 0xA55A00FC, 5, 7 };
	CHECK(crc32_words(z, 1) == 0x2144df1cu, "crc of one zero word: %08x", crc32_words(z, 1));
	CHECK(crc32_words(t, 3) == 0xb0e02293u, "crc of 1,2,3: %08x", crc32_words(t, 3));
	CHECK(crc32_words(u, 3) == 0xef608205u, "crc of a55a00fc,5,7: %08x", crc32_words(u, 3));
}

/* ---------------------------------------------------------------- 4 features */
static struct frame_slot FS;

static void make(uint32_t seq, struct model *md, int crc_fault, uint32_t flags)
{
	memset(&FS, 0, sizeof(FS));
	frame_build(FS.w, seq, 0, md, crc_fault);
	FS.flags = flags;
}

static void test_features(void)
{
	struct model gen;
	struct verifier v;
	struct window wn;
	crc_init();

	/* clean window of 500 frames */
	model_init(&gen); verifier_init(&v); window_reset(&wn, 0);
	for (uint32_t i = 0; i < 500; i++) { make(100 + i, &gen, 0, i == 0); verify_frame(&v, &wn, &FS); }
	CHECK(wn.frames == 500 && wn.bad == 0 && wn.gaps == 0, "clean window: frames %u bad %u gaps %u", wn.frames, wn.bad, wn.gaps);
	CHECK(wn.peak == 795986176u && window_mean(&wn) == 278229455u, "nominal peak %u mean %u (the limits 900000000 and 320000000 are set from these)", wn.peak, window_mean(&wn));

	/* CRC fault on frame 10 only */
	model_init(&gen); verifier_init(&v); window_reset(&wn, 0);
	for (uint32_t i = 0; i < 50; i++) { make(i, &gen, i == 10, i == 0); verify_frame(&v, &wn, &FS); }
	CHECK(wn.bad == 1 && wn.gaps == 0, "one CRC fault: bad %u gaps %u", wn.bad, wn.gaps);

	/* payload bit flip: payload and CRC both disagree, still one bad frame; later frames are clean again */
	model_init(&gen); verifier_init(&v); window_reset(&wn, 0);
	for (uint32_t i = 0; i < 20; i++) {
		make(i, &gen, 0, i == 0);
		if (i == 5) FS.w[10] ^= 0x400;
		verify_frame(&v, &wn, &FS);
	}
	CHECK(wn.bad == 1, "one flipped payload bit: bad %u", wn.bad);

	/* bad magic */
	model_init(&gen); verifier_init(&v); window_reset(&wn, 0);
	for (uint32_t i = 0; i < 10; i++) { make(i, &gen, 0, i == 0); if (i == 3) FS.w[0] ^= 0x10000; verify_frame(&v, &wn, &FS); }
	CHECK(wn.bad == 1, "bad magic: bad %u", wn.bad);

	/* sequence gap of 3: counted as gaps (drops), not as bad frames; the payload check pauses until the next run */
	model_init(&gen); verifier_init(&v); window_reset(&wn, 0);
	for (uint32_t i = 0; i < 20; i++) {
		make(i < 10 ? i : i + 3, &gen, 0, i == 0);
		if (i >= 10) { /* the three dropped slots did not consume FIR samples here: model unchanged, frames continue */ }
		verify_frame(&v, &wn, &FS);
	}
	CHECK(wn.gaps == 3 && wn.bad == 0, "gap of 3: gaps %u bad %u", wn.gaps, wn.bad);

	/* repeated sequence number */
	model_init(&gen); verifier_init(&v); window_reset(&wn, 0);
	for (uint32_t i = 0; i < 10; i++) { make(i == 6 ? 5 : i, &gen, 0, i == 0); verify_frame(&v, &wn, &FS); }
	CHECK(wn.bad >= 1, "repeated sequence number is a bad frame: bad %u", wn.bad);

	/* a new run restarts the FIR model: first_in_run flag */
	model_init(&gen); verifier_init(&v); window_reset(&wn, 0);
	for (uint32_t i = 0; i < 30; i++) {
		if (i == 15) model_restart(&gen);
		make(i, &gen, 0, i == 0 || i == 15);
		verify_frame(&v, &wn, &FS);
	}
	CHECK(wn.bad == 0, "two runs back to back: bad %u", wn.bad);

	/* resync after a flush: the next frame defines the expected sequence number */
	verifier_resync(&v);
	model_init(&gen);
	make(5000, &gen, 0, 0);
	CHECK(verify_frame(&v, &wn, &FS) == 0 && wn.gaps == 0, "after resync a jump of the sequence number is not a gap");
	/* a run continues after a flush: its frames are out of phase with a restarted model, so the payload check pauses */
	for (uint32_t i = 1; i < 10; i++) { make(5000 + i, &gen, 0, 0); verify_frame(&v, &wn, &FS); }
	CHECK(wn.bad == 0, "after a flush in mid-run the payload check pauses instead of flagging every frame: bad %u", wn.bad);
	/* the next run restarts the model and the check is on again */
	model_init(&gen); make(6000, &gen, 0, 1); verify_frame(&v, &wn, &FS);
	make(6001, &gen, 0, 0); FS.w[20] ^= 1; verify_frame(&v, &wn, &FS);
	CHECK(wn.bad == 1, "payload check is back after the first frame of a new run: bad %u", wn.bad);
}

/* ---------------------------------------------------------------- 5 hart 4 logic */
static char plog[4096];
static int n_abort, n_sent;
static struct st_ack sent[16];
static uint32_t sent_remote[16];

static void t_print(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	strncat(plog, b, sizeof(plog) - strlen(plog) - 2);
	strcat(plog, "\n");
}
static void t_abort(void) { n_abort++; }
static void t_send(uint32_t remote, const void *b, uint32_t len)
{
	if (len == sizeof(struct st_ack) && n_sent < 16) { memcpy(&sent[n_sent], b, len); sent_remote[n_sent] = remote; n_sent++; }
}
static int n_delay;
static void t_delay(uint32_t ms) { n_delay += (int)ms; }

static void reset_io(void) { plog[0] = 0; n_abort = n_sent = n_delay = 0; }

static void send_features(struct st_ctx *c, uint32_t win, uint32_t frames, uint32_t bad, uint32_t drops, uint32_t peak, uint32_t mean, uint32_t now)
{
	struct st_msg m;
	st_msg_init(&m, ST_T_FEATURES);
	m.x[0] = win; m.x[1] = frames; m.x[2] = bad; m.x[3] = drops; m.x[4] = peak; m.x[5] = mean; m.x[6] = 500;
	st_handle(c, &m, 0x400, 1000, 1000, now);
}

static void test_hart4(void)
{
	static struct st_ctx c;
	const struct st_io io = { t_print, t_abort, t_send, t_delay };
	struct st_msg m;

	st_init(&c, &io);
	reset_io();
	st_msg_init(&m, ST_T_ARM); m.deadline_ms = 500; m.max_lag = 320;
	st_handle(&c, &m, 0x401, 100, 100, 1000);
	CHECK(c.state == ST_S_ARMED && n_sent == 1 && sent[0].type == ST_T_ARM && sent[0].state == ST_S_ARMED && sent_remote[0] == 0x401, "ARM answered from the sender's address");
	CHECK(st_timeout(&c, 77) == 500, "armed: receive timeout is the deadline");
	st_msg_init(&m, ST_T_SET_LIMITS); m.x[0] = 2; m.x[1] = 4; m.x[2] = 900000000u; m.x[3] = 320000000u;
	st_handle(&c, &m, 0x401, 100, 100, 1001);
	CHECK(c.lim.max_bad == 2 && c.lim.max_drops == 4 && c.lim.max_peak == 900000000u && c.lim.max_mean == 320000000u, "limits stored");

	/* a clean window and a heartbeat inside the lag: no trip */
	send_features(&c, 1, 500, 2, 4, 795986176u, 278229455u, 1500);
	CHECK(c.state == ST_S_ARMED && n_abort == 0, "limits are inclusive: bad 2, drops 4 are allowed");
	st_msg_init(&m, ST_T_STATUS); m.next_seq = 100 - 300;
	st_handle(&c, &m, 0x401, 100, 100, 1600);
	CHECK(c.state == ST_S_ARMED && c.seq_mismatch == 0, "lag 300 <= 320: no trip");

	/* over the limit: trip once */
	reset_io();
	send_features(&c, 2, 500, 3, 0, 1, 1, 2000);
	CHECK(c.state == ST_S_SAFE && n_abort == 1 && c.trips == 1 && c.last_reason == ST_R_BAD_FRAMES, "bad frames over the limit trip the system");
	CHECK(n_sent == 2 && sent[0].type == ST_T_TRIP_NOTICE && sent[0].state == ST_S_SAFE && sent[0].last_reason == ST_R_BAD_FRAMES && sent[1].type == ST_T_FEATURES,
	      "a notice goes to Linux first, then the acknowledgement; it carries the reason");
	CHECK(strstr(plog, "[station] TRIP #1: BAD_FRAMES at t=2000 ms, source aborted, state SAFE") && strstr(plog, "window 2: frames 500, bad 3, drops 0, peak 1, mean 1"), "trip console lines: %s", plog);
	CHECK(sent[1].events == c.events && sent[1].trips == 1 && sent[1].last_reason == ST_R_BAD_FRAMES, "every acknowledgement carries events, trips and last reason");
	reset_io();
	send_features(&c, 3, 500, 99, 99, 0xFFFFFFF0u, 0xFFFFFFF0u, 2500);
	CHECK(n_abort == 0 && c.trips == 1 && n_sent == 1, "while SAFE a further violation does not trip again");

	/* a heartbeat while SAFE repeats the ABORT (one written between two runs would be lost), without a new trip */
	reset_io();
	st_msg_init(&m, ST_T_STATUS); m.next_seq = 1000;
	st_handle(&c, &m, 0x401, 1000, 1000, 2600);
	CHECK(n_abort == 1 && c.trips == 1 && c.state == ST_S_SAFE, "STATUS while SAFE repeats the abort, no new trip (aborts %d)", n_abort);

	/* reset */
	reset_io();
	st_msg_init(&m, ST_T_RESET);
	st_handle(&c, &m, 0x401, 1000, 1000, 3000);
	CHECK(c.state == ST_S_ARMED && sent[0].state == ST_S_ARMED && c.trips == 1 && strstr(plog, "RESET from operator: trip #1 (BAD_FRAMES) cleared, ARMED again"), "reset clears the trip: %s", plog);
	reset_io();
	st_handle(&c, &m, 0x401, 1000, 1000, 3100);
	CHECK(strstr(plog, "nothing to clear") && c.state == ST_S_ARMED, "reset while armed does nothing");

	/* several limits at once, peak and mean */
	reset_io();
	send_features(&c, 4, 500, 0, 5, 900000001u, 320000001u, 3500);
	CHECK(c.last_reason == (ST_R_DROPS | ST_R_PEAK | ST_R_MEAN) && n_abort == 1, "reason is a mask: %#x", c.last_reason);
	st_msg_init(&m, ST_T_RESET); st_handle(&c, &m, 0x401, 1000, 1000, 3600);

	/* sequence check */
	reset_io();
	st_msg_init(&m, ST_T_STATUS); m.next_seq = 1000 - 321;
	st_handle(&c, &m, 0x401, 1000, 1000, 4000);
	CHECK(c.state == ST_S_SAFE && c.last_reason == ST_R_SEQUENCE && c.seq_mismatch == 1 && n_abort == 1, "linux more than max_lag behind the fabric trips with SEQUENCE");
	st_msg_init(&m, ST_T_RESET); st_handle(&c, &m, 0x401, 1000, 1000, 4100);

	/* deadline: no message within the timeout while armed: ALARM line, then TRIP */
	reset_io();
	st_on_timeout(&c, 1000, 1000, 5000);
	CHECK(c.state == ST_S_SAFE && c.last_reason == ST_R_DEADLINE && c.deadline_miss == 1 && n_abort == 1 &&
	      strstr(plog, "ALARM: no message within 500 ms") && strstr(plog, "TRIP #4: DEADLINE at t=5000 ms"), "deadline: %s", plog);
	CHECK(sent[0].type == ST_T_TRIP_NOTICE && sent_remote[0] == 0x401, "the notice goes to the endpoint that talked last");
	CHECK(st_timeout(&c, 77) == 77, "safe: the receive call blocks, no repeated alarm for one silence");
	reset_io();
	st_on_timeout(&c, 1000, 1000, 5600);
	CHECK(n_abort == 0 && c.deadline_miss == 1, "no second deadline miss while SAFE");

	/* disarm */
	reset_io();
	st_msg_init(&m, ST_T_DISARM); st_handle(&c, &m, 0x401, 1000, 1000, 6000);
	CHECK(c.state == ST_S_IDLE && strstr(plog, "DISARM from SAFE"), "disarm");

	/* roll call: three hellos, then LISTEN makes hart 4 speak once; LISTEN too early does not */
	static struct st_ctx h;
	st_init(&h, &io);
	reset_io();
	for (uint32_t cpu = 0; cpu < 3; cpu++) {
		st_msg_init(&m, ST_T_HELLO); m.burst = 1; m.next_seq = cpu; m.errors = cpu + 1;
		snprintf((char *)&m.x[0], 16, "core %u here", cpu + 1);
		st_handle(&h, &m, 0x400 + cpu, 5, 5, 100 + cpu);
		CHECK(sent[n_sent - 1].type == ST_T_HELLO && sent[n_sent - 1].status_rx == cpu + 1 && !strncmp(sent[n_sent - 1].text, "hart 4 FreeRTOS", 16), "HELLO %u answered, heard %u", cpu, sent[n_sent - 1].status_rx);
	}
	CHECK(strstr(plog, "[roll-call] hart 4 (FreeRTOS) got HELLO #1 from hart 2 (Linux cpu 1): core 2 here") && strstr(plog, "heard all three Linux harts"), "roll-call console lines: %s", plog);
	reset_io();
	st_msg_init(&m, ST_T_LISTEN);
	st_handle(&h, &m, 0x500, 5, 5, 1000);
	CHECK(n_sent == 2 && sent[0].type == ST_T_LISTEN && sent[1].type == ST_T_SPEAK && !strncmp(sent[1].text, "core 4 speaks", 16) && n_delay == 500 && sent_remote[1] == 0x500,
	      "LISTEN: acknowledgement, then one message of hart 4's own, %d sent", n_sent);
	CHECK(strstr(plog, "[roll-call] core 4 speaks"), "speak line");
	reset_io();
	st_handle(&h, &m, 0x500, 5, 5, 2000);
	CHECK(n_sent == 1 && strstr(plog, "nothing to say yet"), "second LISTEN: hart 4 has nothing to say until the next roll call");
}

static void dump(void)
{
	struct model gen;
	crc_init();
	model_init(&gen);
	for (uint32_t i = 0; i < 3; i++) {
		make(1000 + i, &gen, 0, i == 0);
		printf("frame %u", i);
		for (uint32_t k = 0; k < ST_FRAME_WORDS; k++) printf(" %08x", FS.w[k]);
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "dump")) { dump(); return 0; }
	test_crc();
	test_messages();
	test_features();
	test_hart4();
	test_ring();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
