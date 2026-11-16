// SPDX-License-Identifier: MIT
/*
 * m09_ctl - the Linux side of experiment M09: ask hart 4 for its PMP report, ask it to run the user-mode
 * test, and ping it to see that the supervisor still answers.
 *
 *   m09_ctl [--link rpmsg|unix:PATH] ping [n]      n RPMsg QUERY round trips (default 1), prints each result
 *   m09_ctl [--link ...] report                    hart 4 prints the PMP report on its console; the answer says what it found
 *   m09_ctl [--link ...] run [--timeout-ms N]      hart 4 installs its entries, runs the user-mode test once, restores
 *
 * The evidence of `run` is on hart 4's console (UART); this program prints only the answer (a result code and
 * the counts). Exit status: 0 for an answer that is as expected (ping answered; report answered; run answered
 * OK), 1 for no answer or a result other than OK, 2 for a usage error. "No answer to run" is the hang case of the
 * risk list: the program says what to do.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "pfdaq_pmp.h"

#define SUP_CHANNEL   "pfdaq-supervisor"
#define SUP_MSG_MAGIC 0x53444650u
#define SUP_ACK_MAGIC 0x41444650u
#define SUP_QUERY     4u

struct sup_msg { uint32_t magic, type, burst, next_seq, errors, deadline_ms, max_lag; };
struct sup_ack { uint32_t magic, type, state, status_rx, deadline_miss, seq_mismatch, fabric_seq, fabric_frames, uptime_ms; };

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

/* the Linux 6.18 branch of sup_open() in pfdaq_cap.c: bind the channel to rpmsg_chrdev, open /dev/rpmsgN */
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
	write_str("/sys/bus/rpmsg/drivers/rpmsg_chrdev/bind", dev);
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

static int open_unix(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0)
		return -1;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* send, wait for the answer of the same type; returns the round trip in us, or -1 */
static int64_t xfer(int fd, uint32_t type, int timeout_ms, struct sup_ack *a)
{
	struct sup_msg m = { SUP_MSG_MAGIC, type, 0, 0, 0, 0, 0 };
	struct pollfd p = { .fd = fd, .events = POLLIN };
	uint64_t t0 = mono_ns();

	if (write(fd, &m, sizeof(m)) != (ssize_t)sizeof(m))
		return -1;
	for (;;) {
		int left = timeout_ms - (int)((mono_ns() - t0) / 1000000ull);
		if (left <= 0 || poll(&p, 1, left) <= 0)
			return -1;
		if (read(fd, a, sizeof(*a)) != (ssize_t)sizeof(*a))
			return -1;
		if (a->magic == SUP_ACK_MAGIC && a->type == type)
			return (int64_t)((mono_ns() - t0) / 1000u);
	}
}

static void usage(void)
{
	fprintf(stderr, "usage: m09_ctl [--link rpmsg|unix:PATH] ping [n] | report | run [--timeout-ms N]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *link = "rpmsg";
	int i = 1, fd;
	struct sup_ack a;

	if (i + 1 < argc && !strcmp(argv[i], "--link")) {
		link = argv[i + 1];
		i += 2;
	}
	if (i >= argc)
		usage();
	const char *cmd = argv[i++];
	fd = !strcmp(link, "rpmsg") ? open_rpmsg() : !strncmp(link, "unix:", 5) ? open_unix(link + 5) : -1;
	if (fd < 0) {
		fprintf(stderr, "m09_ctl: cannot open the link to hart 4 (%s): %s\n", link, strerror(errno));
		return 1;
	}
	if (!strcmp(cmd, "ping")) {
		int n = i < argc ? atoi(argv[i]) : 1, ok = 0;
		if (n < 1 || n > 100000)
			usage();
		for (int k = 0; k < n; k++) {
			int64_t us = xfer(fd, SUP_QUERY, 1000, &a);
			if (us < 0) {
				printf("ping %d: no answer within 1000 ms\n", k + 1);
				continue;
			}
			ok++;
			if (n <= 20 || k == 0)
				printf("ping %d: answered after %lld us, supervisor state %u, hart 4 uptime %u ms\n", k + 1, (long long)us, a.state,
				       a.uptime_ms & 0x7FFFFFFFu);
			usleep(2000);
		}
		printf("{\"experiment\": \"m09\", \"command\": \"ping\", \"sent\": %d, \"answered\": %d}\n", n, ok);
		return ok == n ? 0 : 1;
	}
	if (!strcmp(cmd, "report")) {
		if (xfer(fd, M09_T_REPORT, 3000, &a) < 0) {
			printf("no answer to REPORT: is the M09 payload running on hart 4?\n");
			return 1;
		}
		printf("report: %u entries active, %u locked; %s\n", a.fabric_frames >> 16, a.fabric_frames & 0xFFFFu,
		       a.status_rx == 2 ? "a locked entry that allows everything (the PMP cannot be tightened until reset)" :
		       a.status_rx == 1 ? "a locked entry exists" : "no locked entry");
		printf("{\"experiment\": \"m09\", \"command\": \"report\", \"active\": %u, \"locked\": %u, \"locked_open_to_all\": %s}\n",
		       a.fabric_frames >> 16, a.fabric_frames & 0xFFFFu, a.status_rx == 2 ? "true" : "false");
		return 0;
	}
	if (!strcmp(cmd, "run")) {
		int timeout_ms = 10000;
		if (i + 1 < argc && !strcmp(argv[i], "--timeout-ms"))
			timeout_ms = atoi(argv[i + 1]);
		if (timeout_ms < 100 || timeout_ms > 600000)
			usage();
		int64_t us = xfer(fd, M09_T_RUN, timeout_ms, &a);
		if (us < 0) {
			printf("NO ANSWER to RUN within %d ms.\n"
			       "Hart 4 may be stuck in the test. Nothing on the SD card was written by this program. Reboot the board (ssh root@board reboot;\n"
			       "if Linux does not answer either, power-cycle it). If hart 4 still does not come up, restore the proven payload from the PC\n"
			       "(experiments/amp-m09/deploy_payload.sh restore, or by hand with the SD card in the PC: see the page's risk list).\n",
			       timeout_ms);
			printf("{\"experiment\": \"m09\", \"command\": \"run\", \"answered\": false}\n");
			return 1;
		}
		const char *name = m09_result_name((enum m09_result)a.status_rx);
		printf("run: result %s (%u), %u access faults caught, ended %u (1 = the test's own ecall), after %lld us\n", name, a.status_rx,
		       a.fabric_seq, a.deadline_miss, (long long)us);
		printf("{\"experiment\": \"m09\", \"command\": \"run\", \"answered\": true, \"result\": \"%s\", \"faults\": %u, \"ended\": %u, "
		       "\"active\": %u, \"locked\": %u, \"round_trip_us\": %lld}\n",
		       name, a.fabric_seq, a.deadline_miss, a.fabric_frames >> 16, a.fabric_frames & 0xFFFFu, (long long)us);
		return a.status_rx == M09_RES_OK ? 0 : 1;
	}
	usage();
	return 2;
}
