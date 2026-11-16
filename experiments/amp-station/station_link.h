/* SPDX-License-Identifier: MIT
 *
 * station_link.h - the link from a Linux process to hart 4.
 *
 * On the board it is the project's RPMsg endpoint: sup_open() below is the one of pfdaq_cap.c
 * (yocto/meta-pfdaq/recipes-pfdaq/pfdaq-tools/files/pfdaq_cap.c), shortened to the flow that
 * works on Linux 6.18 (bind the channel to rpmsg_chrdev, open /dev/rpmsgN). On the PC the same
 * messages travel over a Unix SOCK_SEQPACKET socket to the stand-in for hart 4 (fake_hart4).
 * The chrdev allows one opener: only the analysis process (and the roll-call commands, which run
 * before it) use it.
 */
#ifndef STATION_LINK_H
#define STATION_LINK_H

#include "station_common.h"
#include "pfdaq_station.h"
#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#define LINK_CHANNEL "pfdaq-supervisor"

static inline int link_write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY), r;
	if (fd < 0)
		return -1;
	r = write(fd, val, strlen(val) + 1) < 0 ? -1 : 0;
	close(fd);
	return r;
}

/* from pfdaq_cap.c sup_open(), kernel 6.18 branch: find the channel, bind rpmsg_chrdev, open /dev/rpmsgN */
static inline int link_open_rpmsg(void)
{
	char dev[300] = "", path[700], node[300] = "";
	struct dirent *e;
	DIR *d = opendir("/sys/bus/rpmsg/devices");

	if (!d)
		return -1;
	while ((e = readdir(d)))
		if (strstr(e->d_name, LINK_CHANNEL))
			snprintf(dev, sizeof(dev), "%s", e->d_name);
	closedir(d);
	if (!dev[0]) { errno = ENODEV; return -1; }
	snprintf(path, sizeof(path), "/sys/bus/rpmsg/devices/%s/driver_override", dev);
	link_write_str(path, "rpmsg_chrdev");
	link_write_str("/sys/bus/rpmsg/drivers/rpmsg_chrdev/bind", dev);   /* fails harmlessly if already bound */
	snprintf(path, sizeof(path), "/sys/bus/rpmsg/devices/%s/rpmsg", dev);
	for (int tries = 0; tries < 50 && !(d = opendir(path)); tries++)
		usleep(20000);                      /* the chrdev appears shortly after the bind */
	if (!d) { errno = ENOENT; return -1; }
	while ((e = readdir(d)))
		if (!strncmp(e->d_name, "rpmsg", 5) && strncmp(e->d_name, "rpmsg_ctrl", 10))
			snprintf(node, sizeof(node), "/dev/%s", e->d_name);
	closedir(d);
	if (!node[0]) { errno = ENOENT; return -1; }
	return open(node, O_RDWR);
}

static inline int link_open_unix(const char *path)
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

/* "rpmsg" or "unix:PATH" */
static inline int link_open(const char *spec)
{
	if (!strcmp(spec, "rpmsg"))
		return link_open_rpmsg();
	if (!strncmp(spec, "unix:", 5))
		return link_open_unix(spec + 5);
	errno = EINVAL;
	return -1;
}

typedef void (*link_notice_fn)(const struct st_ack *a, void *arg);

static inline int link_is_notice(const struct st_ack *a)
{
	return a->magic == ST_ACK_MAGIC && (a->type == ST_T_TRIP_NOTICE || a->type == ST_T_SPEAK);
}

/* read everything that is waiting, without blocking, and pass unsolicited notices on */
static inline void link_drain(int fd, link_notice_fn cb, void *arg)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	struct st_ack a;
	while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
		if (read(fd, &a, sizeof(a)) != (ssize_t)sizeof(a))
			return;
		if (link_is_notice(&a) && cb)
			cb(&a, arg);
	}
}

/*
 * Send one message and wait for the acknowledgement of the same type (at most timeout_ms).
 * Unsolicited notices that arrive meanwhile go to cb. Returns 0 on success, -1 on timeout or error.
 */
static inline int link_xfer(int fd, const struct st_msg *m, struct st_ack *ack, int timeout_ms,
			    link_notice_fn cb, void *arg)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	uint64_t t0 = mono_ns(), now;

	if (write(fd, m, sizeof(*m)) != (ssize_t)sizeof(*m))
		return -1;
	for (;;) {
		now = mono_ns();
		int left = timeout_ms - (int)((now - t0) / 1000000ull);
		if (left <= 0 || poll(&p, 1, left) <= 0)
			return -1;
		if (read(fd, ack, sizeof(*ack)) != (ssize_t)sizeof(*ack))
			return -1;
		if (link_is_notice(ack)) {
			if (cb)
				cb(ack, arg);
			if (ack->type == m->type)       /* a notice type that was itself requested: not used */
				return 0;
			continue;
		}
		if (ack->magic == ST_ACK_MAGIC && ack->type == m->type)
			return 0;
		/* an acknowledgement of an earlier, timed-out message: skip it */
	}
}

static inline void st_msg_init(struct st_msg *m, uint32_t type)
{
	memset(m, 0, sizeof(*m));
	m->magic = ST_MSG_MAGIC;
	m->type = type;
}

#endif /* STATION_LINK_H */
