// SPDX-License-Identifier: MIT
/*
 * dma_one_burst - the smallest complete use of the fabric DMA on the pfdaq baseline.
 *
 * Writes one stream descriptor, starts one burst on the fabric stream source, sleeps until the
 * DMA interrupt arrives, then prints the first frame and checks every frame's CRC-32.
 * Register offsets and values are the ones pfdaq_cap.c uses (docs/modules/I04).
 */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SRC_OFF      0xF00u       /* stream source registers inside its UIO page */
#define SRC_ID       0x00         /* reads 0x50464451 */
#define SRC_CTRL     0x08         /* bit0 START */
#define SRC_PAYLOAD  0x0C         /* payload words per frame */
#define SRC_FRAMES   0x10         /* frames per START */
#define SRC_GAP      0x14
#define SRC_FAULT    0x18
#define SRC_STATUS   0x1C         /* bit0 busy */
#define SRC_MODE     0x34         /* 0 = counter payload, burst mode */
#define DMA_STAT     0x10         /* INTR_0_STAT_REG */
#define DMA_MASK     0x14         /* INTR_0_MASK_REG */
#define DMA_CLEAR    0x18         /* INTR_0_CLEAR_REG */
#define DMA_STREAM0  0x460        /* STREAM_0_ADDR_REG: descriptor for TDEST 0 */
#define DESC_ARMED   0xDu         /* valid | destination data ready | incrementing address */
#define DESC_OFF     0x0u         /* descriptor at the start of the non-cached window */
#define BUF_OFF      0x1000u      /* data buffer one page later */
#define PAYLOAD      252u         /* + 4 header/CRC words = 1 KiB frames (even word count) */
#define FRAMES       16u

static int read_str(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f || !fgets(buf, len, f)) { if (f) fclose(f); return -1; }
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

/* find a UIO device by its name in sysfs, open it and map its first page */
static int open_uio(const char *name, volatile uint32_t **map)
{
	char path[300], val[128];
	struct dirent *e;
	DIR *d = opendir("/sys/class/uio");
	while (d && (e = readdir(d))) {
		snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
		if (strncmp(e->d_name, "uio", 3) || read_str(path, val, sizeof(val)) || strcmp(val, name))
			continue;
		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		closedir(d);
		int fd = open(path, O_RDWR | O_SYNC);
		void *m = fd < 0 ? MAP_FAILED : mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m == MAP_FAILED) break;
		*map = m;
		return fd;
	}
	fprintf(stderr, "UIO device %s not found or not mappable\n", name);
	exit(1);
}

/* CRC-32 (IEEE 802.3) over n little-endian 32-bit words, one bit at a time */
static uint32_t crc32_words(const uint32_t *w, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		c ^= w[i];
		for (int k = 0; k < 32; k++)
			c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
	}
	return ~c;
}

int main(void)
{
	volatile uint32_t *page, *dma;
	char buf[64];
	uint32_t cnt, bytes = FRAMES * (PAYLOAD + 4) * 4;
	open_uio("pfdaq_stream_src", &page);
	int dma_fd = open_uio("dma-controller@60010000", &dma);
	volatile uint32_t *src = page + SRC_OFF / 4;

	/* the non-cached DDR window: its physical address is what the DMA must be given */
	if (read_str("/sys/class/u-dma-buf/udmabuf-ddr-nc0/phys_addr", buf, sizeof(buf))) { perror("udmabuf"); return 1; }
	uint64_t phys = strtoull(buf, NULL, 0);
	int ddr_fd = open("/dev/udmabuf-ddr-nc0", O_RDWR | O_SYNC);
	uint8_t *ddr = ddr_fd < 0 ? MAP_FAILED : mmap(NULL, BUF_OFF + bytes, PROT_READ | PROT_WRITE, MAP_SHARED, ddr_fd, 0);
	if (ddr == MAP_FAILED) { perror("/dev/udmabuf-ddr-nc0"); return 1; }
	if (src[SRC_ID / 4] != 0x50464451u || (src[SRC_STATUS / 4] & 1)) { fprintf(stderr, "stream source missing or busy\n"); return 1; }

	/* enable the DMA interrupt sources, clear old events, drop completions left by earlier runs */
	struct pollfd p = { .fd = dma_fd, .events = POLLIN };
	dma[DMA_MASK / 4] = 0xF;
	dma[DMA_CLEAR / 4] = 0xF;
	while (poll(&p, 1, 10) > 0 && read(dma_fd, &cnt, 4) == 4)
		;

	/* 1. arm: byte count and destination first, the configuration word last, then tell the DMA where it is */
	volatile uint32_t *desc = (volatile uint32_t *)(ddr + DESC_OFF);
	memset(ddr + BUF_OFF, 0xEE, bytes);
	desc[1] = bytes;
	desc[2] = (uint32_t)(phys + BUF_OFF);
	desc[0] = DESC_ARMED;
	dma[DMA_STREAM0 / 4] = (uint32_t)(phys + DESC_OFF);

	/* 2. configure the source and START: the whole burst is one packet */
	src[SRC_MODE / 4] = 0;
	src[SRC_FAULT / 4] = 0;
	src[SRC_GAP / 4] = 0;
	src[SRC_PAYLOAD / 4] = PAYLOAD;
	src[SRC_FRAMES / 4] = FRAMES;
	src[SRC_CTRL / 4] = 0x1;

	/* 3. sleep until the DMA interrupt: the read returns the UIO event count */
	if (poll(&p, 1, 2000) <= 0 || read(dma_fd, &cnt, 4) != 4) { fprintf(stderr, "no DMA interrupt within 2 s\n"); return 1; }
	uint32_t stat = dma[DMA_STAT / 4], cfg = desc[0];
	dma[DMA_CLEAR / 4] = 0xF;
	printf("DMA interrupt: event count %u, INTR_0_STAT %#x, descriptor config %#x (%s)\n", cnt, stat, cfg,
	       (stat & 0xE) ? "DMA ERROR" : (cfg & 0x4) ? "NOT COMPLETE" : "complete, ready bit cleared by the DMA");

	/* 4. look at what arrived */
	const uint32_t *w = (const uint32_t *)(ddr + BUF_OFF);
	uint32_t seq0 = w[1];
	unsigned bad = 0;
	printf("frame 0: header %08x  seq %u  timestamp %u  payload %08x %08x %08x ...  crc %08x\n",
	       w[0], w[1], w[2], w[3], w[4], w[5], w[PAYLOAD + 3]);
	for (uint32_t f = 0; f < FRAMES; f++, w += PAYLOAD + 4)
		bad += w[0] != (0xA55A0000u | PAYLOAD) || w[1] != seq0 + f || w[PAYLOAD + 3] != crc32_words(w, PAYLOAD + 3);
	printf("%u frames, %u bytes: %u bad (header, sequence or CRC-32)\n", FRAMES, bytes, bad);
	return (stat & 0xE) || (cfg & 0x4) || bad;
}
