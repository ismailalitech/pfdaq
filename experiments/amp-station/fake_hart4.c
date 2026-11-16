// SPDX-License-Identifier: MIT
/*
 * fake_hart4 - stand-in for hart 4 on the PC. It is not a re-implementation: it runs the very file
 * that runs on hart 4 (firmware/u54_4-supervisor/pfdaq_station.h), with the three environment
 * functions replaced:
 *   print        -> stdout
 *   abort_source -> sets the abort word that the simulated data source watches (CTRL.ABORT)
 *   send         -> write to the Unix SOCK_SEQPACKET connection (instead of an RPMsg endpoint)
 * and the supervisor's receive loop (timeout = deadline while armed) written out in a few lines.
 */
#include "station_common.h"
#include "station_fabric.h"
#include "pfdaq_station.h"
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>

static struct fabsim_shm *sh;
static int conn = -1;
static uint64_t t0;
static volatile sig_atomic_t g_stop;

static uint32_t now_ms(void) { return (uint32_t)((mono_ns() - t0) / 1000000ull); }

static void f_print(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("%s\n", buf);
	fflush(stdout);
}

static void f_abort(void) { __atomic_store_n(&sh->abort_req, 1, __ATOMIC_SEQ_CST); }
static void f_send(uint32_t remote, const void *b, uint32_t len)
{
	(void)remote;
	if (conn >= 0 && write(conn, b, len) < 0)
		perror("fake_hart4 send");
}
static void f_delay(uint32_t ms) { usleep(ms * 1000u); }
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv)
{
	const char *sock = NULL, *fab = NULL;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--sock")) sock = argv[i + 1];
		else if (!strcmp(argv[i], "--fabsim")) fab = argv[i + 1];
	}
	if (!sock || !fab) { fprintf(stderr, "usage: fake_hart4 --sock PATH --fabsim PATH\n"); return 2; }
	sh = fabsim_map(fab, 1);
	if (!sh) { perror("fabsim"); return 1; }
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	signal(SIGPIPE, SIG_IGN);

	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
	unlink(sock);
	int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 4)) { perror("listen"); return 1; }

	static struct st_ctx c;
	const struct st_io io = { f_print, f_abort, f_send, f_delay };
	st_init(&c, &io);
	t0 = mono_ns();
	f_print("[fake hart 4] listening on %s", sock);

	while (!g_stop) {
		struct pollfd p[2] = { { .fd = ls, .events = POLLIN }, { .fd = conn, .events = POLLIN } };
		int to = (int)st_timeout(&c, 200);          /* as the supervisor: deadline while armed */
		if (to > 200 && c.state != ST_S_ARMED) to = 200;
		int n = poll(p, conn >= 0 ? 2 : 1, to);
		if (n == 0 && c.state == ST_S_ARMED) {
			usleep(1000);                           /* the supervisor's 1 ms fabric snapshot */
			st_on_timeout(&c, sh->next_seq, sh->frames_sent, now_ms());
			continue;
		}
		if (n > 0 && (p[0].revents & POLLIN)) {
			if (conn >= 0) close(conn);
			conn = accept(ls, NULL, NULL);
		}
		if (n > 0 && conn >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
			struct st_msg m;
			ssize_t r = read(conn, &m, sizeof(m));
			if (r <= 0) { close(conn); conn = -1; continue; }
			if (r != (ssize_t)sizeof(m) || m.magic != ST_MSG_MAGIC) { f_print("[fake hart 4] ignored message, len %d", (int)r); continue; }
			usleep(1000);                           /* fabric snapshot delay */
			st_handle(&c, &m, 0, sh->next_seq, sh->frames_sent, now_ms());
		}
	}
	f_print("[fake hart 4] stopped: state %s, trips %u, events %u", st_state_name(c.state), c.trips, c.events);
	unlink(sock);
	return 0;
}
