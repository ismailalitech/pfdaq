/* Unit tests of the M08 program and of the hart 4 routines in pfdaq_m08.h, on the PC (and, cross-compiled,
 * under qemu-riscv64). The program is included, not linked, so its static functions can be called. */
#include <stddef.h>
#define main rtt_main
#include "../m08_rtt.c"
#undef main

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void test_stats(void)
{
	uint64_t a[5] = { 50, 10, 30, 20, 40 };
	struct stats s = compute_stats(a, 5);
	CHECK(s.n == 5 && s.min == 10 && s.max == 50 && s.median == 30 && s.p95 == 50);
	uint64_t b[4] = { 4, 1, 3, 2 };
	s = compute_stats(b, 4);
	CHECK(s.median == 2 && s.min == 1 && s.max == 4);          /* (2 + 3) / 2 in integers */
	uint64_t c[20], d[100], e[1] = { 7 };
	for (int i = 0; i < 20; i++) c[i] = 20 - i;               /* 20 .. 1, reversed on purpose */
	s = compute_stats(c, 20);
	CHECK(s.p95 == 19 && s.median == 10);                     /* ceil(0.95 * 20) = 19th value */
	for (int i = 0; i < 100; i++) d[i] = (uint64_t)(i + 1) * 10;
	s = compute_stats(d, 100);
	CHECK(s.p95 == 950 && s.median == 505 && s.max == 1000);  /* 95th of 100; (500 + 510) / 2 */
	s = compute_stats(e, 1);
	CHECK(s.min == 7 && s.median == 7 && s.p95 == 7 && s.max == 7);
	s = compute_stats(e, 0);
	CHECK(s.n == 0 && s.max == 0);
}

static void test_block_ok(void)
{
	CHECK(m08_block_ok(0xC8000000u));
	CHECK(m08_block_ok(0xC9E00000u));                          /* the block the program uses */
	CHECK(m08_block_ok(0xC8000000u + M08_WINDOW_SIZE - M08_BLOCK_SIZE));
	CHECK(!m08_block_ok(0xC8000000u + M08_WINDOW_SIZE));       /* one page past the end */
	CHECK(!m08_block_ok(0xC8000000u + M08_WINDOW_SIZE - 0x800u));   /* straddles the end */
	CHECK(!m08_block_ok(0xC7FFF000u));
	CHECK(!m08_block_ok(0x91D00000u));                         /* the RPMsg rings: never */
	CHECK(!m08_block_ok(0x88100000u));                         /* the cached window: never */
	CHECK(!m08_block_ok(0xC9E00004u));                         /* not page aligned */
	CHECK(!m08_block_ok(0));
	CHECK(!m08_block_ok(0x1FFFFFFFFFFull));
	CHECK(M08_BLOCK_OFFSET + M08_BLOCK_SIZE <= 0x1F00000u);    /* below the M05 shared-memory test (offset 0x1F00000) */
}

/* hart 4 poll loop against a Linux thread */
static struct m08_shm blk;
static volatile int th_done;
static uint32_t clk_ms(void) { return (uint32_t)(mono_ns() / 1000000ull); }

static void *linux_side(void *arg)
{
	unsigned n = (unsigned)(uintptr_t)arg;
	for (uint32_t i = 1; i <= n; i++) {
		blk.req = i;
		__sync_synchronize();
		uint64_t t0 = mono_ns();
		while (blk.ack != i && mono_ns() - t0 < 2000000000ull)
			;
		if (blk.ack != i)
			break;
	}
	blk.stop = 1;
	th_done = 1;
	return NULL;
}

static void test_poll(void)
{
	pthread_t t;
	uint32_t to = 99;
	memset(&blk, 0, sizeof(blk));
	blk.magic = M08_MAGIC;
	pthread_create(&t, NULL, linux_side, (void *)(uintptr_t)5000);
	uint32_t n = m08_poll(&blk, clk_ms, 8000, &to);
	pthread_join(t, NULL);
	CHECK(n == 5000 && to == 0 && blk.ack == 5000 && blk.poll_answers == 5000 && blk.poll_state == M08_POLL_IDLE);

	/* time runs out: nobody writes */
	memset(&blk, 0, sizeof(blk));
	uint64_t t0 = mono_ns();
	n = m08_poll(&blk, clk_ms, 30, &to);
	uint64_t el = (mono_ns() - t0) / 1000000ull;
	CHECK(n == 0 && to == 1 && el >= 29 && el < 200);

	/* a stop that is already set ends the loop at once, with no answer */
	memset(&blk, 0, sizeof(blk));
	blk.stop = 1;
	blk.req = 3;
	n = m08_poll(&blk, clk_ms, 1000, &to);
	CHECK(n == 0 && to == 0 && blk.ack == 0);

	/* the cap on the polling time */
	memset(&blk, 0, sizeof(blk));
	blk.stop = 1;
	n = m08_poll(&blk, clk_ms, 0xFFFFFFFFu, &to);
	CHECK(n == 0);
	CHECK(M08_POLL_MAX_MS == 10000u);

	/* a request that is already pending when the loop starts is not answered: the loop starts from the current value */
	memset(&blk, 0, sizeof(blk));
	blk.req = 7;
	blk.stop = 0;
	n = m08_poll(&blk, clk_ms, 20, &to);
	CHECK(n == 0 && blk.ack == 0 && to == 1);
}

static void test_irq(void)
{
	memset(&blk, 0, sizeof(blk));
	blk.irq_req = 41;
	m08_irq_answer(&blk);
	CHECK(blk.irq_ack == 41 && blk.irq_count == 1);
	blk.irq_req = 42;
	m08_irq_answer(&blk);
	CHECK(blk.irq_ack == 42 && blk.irq_count == 2);
	CHECK(blk.ack == 0 && blk.req == 0);                       /* never touches the flag words */
}

static void test_layout(void)
{
	CHECK(sizeof(struct m08_shm) == 256);
	CHECK(offsetof(struct m08_shm, req) == 8 && offsetof(struct m08_shm, ack) == 12);
	CHECK(offsetof(struct m08_shm, irq_req) == 20 && offsetof(struct m08_shm, irq_ack) == 24);
	CHECK(M08_T_SETUP != 4 && M08_T_POLL != 4 && M08_T_PING != 4 && M08_T_SETUP > 6);   /* no clash with the production types 1..6 */
	CHECK(sizeof(struct sup_msg) == 28 && sizeof(struct sup_ack) == 36);
}

int main(void)
{
	test_stats();
	test_block_ok();
	test_poll();
	test_irq();
	test_layout();
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
