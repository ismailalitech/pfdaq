/* Stand-in for hart 4 on the PC, for the tests of the board-day script: the real sequence of pfdaq_pmp.h (m09_report,
 * m09_run) behind a Unix socket, with a model of the PMP CSRs in place of the hardware. It answers QUERY like the supervisor.
 *   fake_hart4_m09 --sock PATH [--state clean|board]
 * "board" starts with pmp0 locked and open to all, the state the sources predict; "clean" with no entry set. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "pfdaq_pmp.h"

struct sup_msg { uint32_t magic, type, burst, next_seq, errors, deadline_ms, max_lag; };
struct sup_ack { uint32_t magic, type, state, status_rx, deadline_miss, seq_mismatch, fabric_seq, fabric_frames, uptime_ms; };

static struct pmp_state hw;
static void p_print(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap); fputc('\n', stdout); fflush(stdout); }
static void m_read(struct pmp_state *s) { *s = hw; }
static void m_write(const struct pmp_state *w)
{
	for (unsigned i = 0; i < PMP_ENTRIES; i++) {
		uint8_t c = pmp_cfg(&hw, i), n = pmp_cfg(w, i);
		if ((c & PMP_L) || (n & PMP_L))
			continue;
		if (c != n || hw.addr[i] != w->addr[i]) {
			hw.addr[i] = w->addr[i] & M09_PMPADDR_MASK;
			pmp_set_cfg(&hw, i, n);
		}
	}
}
static int m_run_user(struct m09_run *r)
{
	r->ended = M09_ENDED_ECALL;
	r->own_before = 1000; r->own_after = 1000; r->canary_before = r->canary_after = 0xCA7A57A1CA7A57A1ull;
	if (pmp_check(&hw, r->probe_addr[M09_P_OWN_DATA], 8, PMP_ACC_W, PMP_PRIV_U, NULL))
		r->own_after = 1001;
	for (int p = M09_P_LINUX_LOAD; p < M09_PROBES; p++)
		if (!pmp_check(&hw, r->probe_addr[p], 8, p == M09_P_CANARY_STORE ? PMP_ACC_W : PMP_ACC_R, PMP_PRIV_U, NULL)) {
			r->log[r->nlog].cause = m09_probe_cause(p);
			r->log[r->nlog].tval = r->probe_addr[p];
			r->log[r->nlog++].epc = 0x91C01300ull + 4u * (unsigned)p;
		}
	return 0;
}
static const struct m09_io io = { p_print, m_read, m_write, m_run_user };

int main(int argc, char **argv)
{
	const char *sock = NULL, *state = "clean";
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--sock")) sock = argv[i + 1];
		else if (!strcmp(argv[i], "--state")) state = argv[i + 1];
	}
	if (!sock) return 2;
	if (!strcmp(state, "board")) { hw.cfg0 = 0x9F; hw.addr[0] = M09_PMPADDR_MASK; }
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
	unlink(sock);
	int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 4) < 0) return 1;
	printf("fake hart 4 (M09) listening, PMP state %s\n", state);
	fflush(stdout);
	static const struct m09_geom g = { 0x91C13000ull, 0x91C00000ull, 0x100000ull, 0x91D00000ull, 0x91D51000ull, 0x91C11188ull, 0x80200000ull, 0x0A000000ull };
	p_print("[m09] AMP demo variant M09 (stand-in)");
	m09_report(&io, &hw);
	for (;;) {
		int c = accept(ls, NULL, NULL);
		struct sup_msg m;
		while (c >= 0 && read(c, &m, sizeof(m)) == (ssize_t)sizeof(m)) {
			struct sup_ack k = { 0x41444650u, m.type, 0, 0, 0, 0, 0, 0, 12345 };
			struct pmp_state st; unsigned act, lk, idx = 0; int open = 0;
			m_read(&st); pmp_count(&st, &act, &lk);
			if (m.type == M09_T_RUN) {
				struct m09_run run;
				p_print("[m09] RUN requested by Linux");
				k.status_rx = (uint32_t)m09_run(&io, &g, &run);
				k.fabric_seq = run.nlog; k.deadline_miss = (uint32_t)run.ended;
			} else if (m.type == M09_T_REPORT) {
				m09_report(&io, &st);
				if (pmp_first_locked(&st, &idx, &open)) k.status_rx = open ? 2 : 1;
			}
			k.fabric_frames = (act << 16) | lk;
			if (write(c, &k, sizeof(k)) < 0) break;
		}
		if (c >= 0) close(c);
	}
}
