// SPDX-License-Identifier: MIT
/*
 * cached_dma - what a processor sees when the fabric DMA writes memory that it has cached.
 *
 * One trial per run. The test area is 16 KiB to 1 MiB at offset OFF of the 32 MiB region that the
 * kernel reserved for the cached window (/dev/udmabuf-ddr-c0, physical 0x88000000). The same bytes of
 * DDR are reachable through the non-cached window (/dev/udmabuf-ddr-nc0, physical 0xC8000000), which
 * this program uses (a) to put a known pattern into DDR without involving the caches and (b) to look at
 * what DDR really holds. The DMA is pointed at the CACHED address.
 *
 *   state cold   DDR holds OLD, nothing of the area is in any cache
 *   state clean  DDR holds OLD, the processor has read it (lines valid and unmodified)
 *   state dirty  DDR holds PRE, the processor has written OLD (lines modified, not yet written back)
 *
 * Then one burst of counter frames goes into the area by DMA, and the area is read back:
 *   1. through the cached mapping, with no cache maintenance        ("c1")
 *   2. through the non-cached mapping: what is in DDR              ("nc1")
 *   3. (-M sync) after the kernel's own u-dma-buf sync_for_cpu       ("c_sync")
 *   4. after the processor has read 8 MiB elsewhere, which pushes the area's lines out ("c2", "nc2")
 * Each word is classified: NEW (what the DMA wrote), OLD, PRE or other. Words 2 (timestamp) and
 * 255 (CRC) of every 256-word frame are not counted.
 *
 * Also: -B prints the read speed of the two mappings, to show that the c mapping really is cached.
 *       -S control    no DMA: the processor reads the area (so its lines hold PRE), then the processor writes
 *                     OLD through the NON-cached mapping. Reading through the cached mapping then shows whether
 *                     the cached copy is stale. It shows that this experiment can see stale cache lines at all.
 *       -S partial    dirty lines of OLD, then a 24-byte DMA write into the middle of a line (byte offset 8).
 *       -S bench      -n bursts of 1 MiB, each verified completely (magic, sequence, counter payload, CRC-32):
 *                     with -D c straight from the cached view of the buffer, with -D nc the way pfdaq_cap does
 *                     it (copy out of the non-cached view with memcpy, then verify the copy).
 * Options: -D c|nc  window the DMA writes through (default c);  -M sync|evict  maintenance before reading.
 *
 * Register offsets are those of pfdaq_cap.c (docs/modules/I04, I05). Do not run it while another
 * program uses the stream source or the DMA controller.
 */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define SRC_OFF      0xF00u
#define SRC_ID       0x00
#define SRC_CTRL     0x08
#define SRC_PAYLOAD  0x0C
#define SRC_FRAMES   0x10
#define SRC_GAP      0x14
#define SRC_FAULT    0x18
#define SRC_STATUS   0x1C
#define SRC_SNAP_SEQ 0x30
#define SRC_MODE     0x34
#define SRC_ID_VALUE 0x50464451u
#define DMA_STAT     0x10
#define DMA_MASK     0x14
#define DMA_CLEAR    0x18
#define DMA_STREAM0  0x460
#define DESC_ARMED   0xDu
#define PAYLOAD      252u
#define FLEN         256u           /* words per frame: 252 payload + 4 */
#define AREA_OFF     0x400000u      /* test area inside the 32 MiB regions */
#define THRASH_OFF   0x1000000u     /* 8 MiB read to push lines out of the caches */
#define THRASH_LEN   0x800000u

static volatile uint32_t *src, *dma;
static volatile uint32_t *cmap, *ncmap;        /* the same DDR bytes, cached and non-cached */
static int dma_fd = -1;
static uint64_t c_phys, nc_phys;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int read_str(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f || !fgets(buf, len, f)) { if (f) fclose(f); return -1; }
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int write_str(const char *path, const char *v)
{
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	int r = fputs(v, f) < 0 ? -1 : 0;
	if (fclose(f)) r = -1;
	return r;
}

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
		if (m == MAP_FAILED) return -1;
		*map = m;
		return fd;
	}
	if (d) closedir(d);
	return -1;
}

static volatile uint32_t *map_udma(const char *dev, int sync, uint64_t *phys)
{
	char path[200], buf[64];
	snprintf(path, sizeof(path), "/sys/class/u-dma-buf/%s/phys_addr", dev);
	if (read_str(path, buf, sizeof(buf))) return NULL;
	*phys = strtoull(buf, NULL, 0);
	snprintf(path, sizeof(path), "/dev/%s", dev);
	int fd = open(path, sync ? O_RDWR | O_SYNC : O_RDWR);
	if (fd < 0) return NULL;
	void *m = mmap(NULL, 0x2000000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	return m == MAP_FAILED ? NULL : m;
}

/* patterns: pseudo-random so a coincidence with the counter payload is practically impossible */
static inline uint32_t old_w(uint32_t i) { return 0x5EC0DE00u ^ (i * 0x9E3779B1u); }
static inline uint32_t pre_w(uint32_t i) { return 0x0D0D0000u ^ (i * 0x85EBCA6Bu); }

/* the word the fabric writes at word i of the burst (counter payload), without timestamp and CRC */
static inline int new_w(uint32_t i, uint32_t seq0, uint32_t *w)
{
	uint32_t f = i / FLEN, j = i % FLEN;
	if (j == 0) { *w = (0xA55Au << 16) | PAYLOAD; return 1; }
	if (j == 1) { *w = seq0 + f; return 1; }
	if (j == 2 || j == FLEN - 1) return 0;
	*w = (((seq0 + f) & 0xFFFFu) << 16) | (j - 3);
	return 1;
}

struct cnt { uint32_t n_new, n_old, n_pre, n_other, counted; };

static void classify(volatile uint32_t *view, uint32_t words, uint32_t seq0, struct cnt *c)
{
	memset(c, 0, sizeof(*c));
	for (uint32_t i = 0; i < words; i++) {
		uint32_t nw;
		if (!new_w(i, seq0, &nw)) continue;
		uint32_t v = view[i];
		c->counted++;
		if (v == nw) c->n_new++;
		else if (v == old_w(i)) c->n_old++;
		else if (v == pre_w(i)) c->n_pre++;
		else c->n_other++;
	}
}

static void json_cnt(const char *name, const struct cnt *c, int last)
{
	printf("\"%s\": {\"new\": %u, \"old\": %u, \"pre\": %u, \"other\": %u}%s", name, c->n_new, c->n_old, c->n_pre,
	       c->n_other, last ? "" : ", ");
}

static void fill(volatile uint32_t *view, uint32_t words, uint32_t (*fn)(uint32_t))
{
	for (uint32_t i = 0; i < words; i++) view[i] = fn(i);
	__sync_synchronize();
}

static uint32_t thrash(void)
{
	volatile uint32_t *t = cmap + THRASH_OFF / 4;
	uint32_t s = 0;
	for (uint32_t i = 0; i < THRASH_LEN / 4; i += 16) s += t[i];
	for (uint32_t i = 0; i < THRASH_LEN / 4; i += 16) s += t[i];
	return s;
}

static int wait_done(int timeout_ms, uint32_t *stat)
{
	struct pollfd p = { .fd = dma_fd, .events = POLLIN };
	uint32_t cnt;
	if (poll(&p, 1, timeout_ms) <= 0 || read(dma_fd, &cnt, 4) != 4) return -1;
	*stat = dma[DMA_STAT / 4];
	dma[DMA_CLEAR / 4] = 0xF;
	if (*stat & 0xE) return -1;
	if (((volatile uint32_t *)ncmap)[0] & 0x4) return -1;       /* descriptor error flag */
	return 0;
}

static void bandwidth(void)
{
	const uint32_t words = 0x100000u / 4;                       /* 1 MiB */
	volatile uint32_t *c = cmap + AREA_OFF / 4, *nc = ncmap + AREA_OFF / 4;
	uint32_t s = 0;
	double t0, t1, t2, t3, t4;
	thrash();
	t0 = now_s();
	for (uint32_t i = 0; i < words; i++) s += c[i];
	t1 = now_s();
	for (uint32_t i = 0; i < words; i++) s += c[i];
	t2 = now_s();
	for (uint32_t i = 0; i < words; i++) s += nc[i];
	t3 = now_s();
	for (uint32_t i = 0; i < words; i++) s += nc[i];
	t4 = now_s();
	printf("{\"mode\": \"read_speed\", \"bytes\": 1048576, \"cached_first_MBps\": %.1f, \"cached_second_MBps\": %.1f, "
	       "\"noncached_first_MBps\": %.1f, \"noncached_second_MBps\": %.1f, \"sink\": %u}\n",
	       1.048576 / (t1 - t0), 1.048576 / (t2 - t1), 1.048576 / (t3 - t2), 1.048576 / (t4 - t3), s & 1);
}


/* positive control: a writer that the caches do not see (the processor through the non-cached window) */
static int run_control(uint32_t frames)
{
	uint32_t words = frames * FLEN, s = 0;
	volatile uint32_t *carea = cmap + AREA_OFF / 4, *ncarea = ncmap + AREA_OFF / 4;
	uint32_t n_c_pre = 0, n_c_old = 0, n_c_oth = 0, n_nc_old = 0, n_nc_oth = 0, n_c2_old = 0;
	thrash();
	fill(ncarea, words, pre_w);
	thrash();
	for (uint32_t i = 0; i < words; i++) s += carea[i];     /* lines now hold PRE */
	fill(ncarea, words, old_w);                              /* DDR now holds OLD; the cached copies are not told */
	for (uint32_t i = 0; i < words; i++) {
		uint32_t v = carea[i];
		if (v == pre_w(i)) n_c_pre++; else if (v == old_w(i)) n_c_old++; else n_c_oth++;
		v = ncarea[i];
		if (v == old_w(i)) n_nc_old++; else n_nc_oth++;
	}
	thrash();
	for (uint32_t i = 0; i < words; i++) if (carea[i] == old_w(i)) n_c2_old++;
	printf("{\"state\": \"control\", \"frames\": %u, \"words\": %u, \"cached_view\": {\"stale_PRE\": %u, \"fresh_OLD\": %u, \"other\": %u}, "
	       "\"ddr_view\": {\"OLD\": %u, \"other\": %u}, \"cached_view_after_8MiB_read\": {\"OLD\": %u}, \"sink\": %u}\n",
	       frames, words, n_c_pre, n_c_old, n_c_oth, n_nc_old, n_nc_oth, n_c2_old, s & 1);
	return 0;
}

/* a 24-byte DMA write into the middle of a line of which the processor holds a modified copy */
static int run_partial(uint32_t seq0, uint32_t *stat_out, int dest_nc)
{
	const uint32_t words = 256, nbytes = 24, boff = 8;       /* 1 KiB of OLD; the DMA writes bytes 8..31 */
	volatile uint32_t *carea = cmap + AREA_OFF / 4, *ncarea = ncmap + AREA_OFF / 4;
	src[SRC_PAYLOAD / 4] = 2;
	src[SRC_FRAMES / 4] = 1;
	thrash();
	fill(ncarea, words, pre_w);
	thrash();
	fill(carea, words, old_w);                                /* modified lines */
	volatile uint32_t *desc = ncmap;
	desc[1] = nbytes;
	desc[2] = (uint32_t)((dest_nc ? nc_phys : c_phys) + AREA_OFF + boff);
	desc[0] = DESC_ARMED;
	__sync_synchronize();
	dma[DMA_STREAM0 / 4] = (uint32_t)(nc_phys + 0);
	__sync_synchronize();
	src[SRC_CTRL / 4] = 0x1;
	int bad = wait_done(2000, stat_out);
	/* the frame: w0 magic|2, w1 seq, w2 timestamp, w3 {seq,0}, w4 {seq,1}, w5 crc; words 2..7 of the area */
	uint32_t exp[6] = { (0xA55Au << 16) | 2u, seq0, 0, ((seq0 & 0xFFFFu) << 16) | 0u, ((seq0 & 0xFFFFu) << 16) | 1u, 0 };
	uint32_t out_old[2] = {0,0}, out_pre[2] = {0,0}, out_oth[2] = {0,0}, in_new[2] = {0,0}, in_old[2] = {0,0}, in_oth[2] = {0,0};
	for (int pass = 0; pass < 2; pass++) {
		volatile uint32_t *v = pass ? ncarea : carea;
		if (pass == 1) thrash();
		for (uint32_t i = 0; i < words; i++) {
			uint32_t w = v[i];
			if (i >= 2 && i < 8) {
				uint32_t k = i - 2;
				if (k == 2 || k == 5) { if (w == old_w(i)) in_old[pass]++; else in_new[pass]++; }   /* timestamp, CRC: not compared */
				else if (w == exp[k]) in_new[pass]++; else if (w == old_w(i)) in_old[pass]++; else in_oth[pass]++;
			} else {
				if (w == old_w(i)) out_old[pass]++; else if (w == pre_w(i)) out_pre[pass]++; else out_oth[pass]++;
			}
		}
	}
	printf("{\"state\": \"partial\", \"dma_window\": \"%s\", \"dma_ok\": %s, \"dma_stat\": \"%#x\", \"burst_bytes\": %u, \"at_byte_offset\": %u, "
	       "\"cached_view\": {\"burst_words_new\": %u, \"burst_words_old\": %u, \"burst_words_other\": %u, \"neighbours_old\": %u, \"neighbours_pre\": %u, \"neighbours_other\": %u}, "
	       "\"ddr_view_after_8MiB_read\": {\"burst_words_new\": %u, \"burst_words_old\": %u, \"burst_words_other\": %u, \"neighbours_old\": %u, \"neighbours_pre\": %u, \"neighbours_other\": %u}}\n",
	       dest_nc ? "non-cached" : "cached", bad ? "false" : "true", *stat_out, nbytes, boff,
	       in_new[0], in_old[0], in_oth[0], out_old[0], out_pre[0], out_oth[0],
	       in_new[1], in_old[1], in_oth[1], out_old[1], out_pre[1], out_oth[1]);
	return bad;
}

static uint32_t crc_tab[4][256];

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
		crc_tab[0][i] = c;
	}
	for (uint32_t i = 0; i < 256; i++)
		for (int t = 1; t < 4; t++)
			crc_tab[t][i] = (crc_tab[t - 1][i] >> 8) ^ crc_tab[0][crc_tab[t - 1][i] & 0xFF];
}

static uint32_t crc32_words(const uint32_t *w, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		c ^= w[i];
		c = crc_tab[3][c & 0xFF] ^ crc_tab[2][(c >> 8) & 0xFF] ^ crc_tab[1][(c >> 16) & 0xFF] ^ crc_tab[0][c >> 24];
	}
	return ~c;
}

/* counter payload: header, sequence, payload and CRC of every frame; returns the number of bad frames */
static uint32_t verify_counter(const volatile uint32_t *wv, uint32_t frames, uint32_t seq0)
{
	uint32_t bad = 0;
	const uint32_t *w = (const uint32_t *)wv;
	for (uint32_t f = 0; f < frames; f++, w += FLEN) {
		int b = 0;
		if (w[0] != ((0xA55Au << 16) | PAYLOAD)) b = 1;
		if (w[1] != seq0 + f) b = 1;
		uint32_t base = ((seq0 + f) & 0xFFFFu) << 16;
		for (uint32_t k = 0; k < PAYLOAD; k++) if (w[3 + k] != (base | k)) b = 1;
		if (crc32_words(w, FLEN - 1) != w[FLEN - 1]) b = 1;
		bad += b;
	}
	return bad;
}

static int run_bench(int bursts, int dest_nc, int evict)
{
	const uint32_t frames = 1024, nbytes = frames * FLEN * 4, words = frames * FLEN;
	volatile uint32_t *carea = cmap + AREA_OFF / 4, *ncarea = ncmap + AREA_OFF / 4;
	volatile uint32_t *desc = ncmap;
	uint32_t *copy = dest_nc ? malloc(nbytes) : NULL, stat = 0, bad_frames = 0, seq = 0;
	double t_verify = 0, t_copy = 0, t_wait = 0;
	int dma_fail = 0;

	crc_init();
	src[SRC_CTRL / 4] = 0x2;
	usleep(1000);
	seq = src[SRC_SNAP_SEQ / 4];
	src[SRC_PAYLOAD / 4] = PAYLOAD;
	src[SRC_FRAMES / 4] = frames;
	thrash();
	double t_start = now_s();
	for (int b = 0; b < bursts; b++) {
		desc[1] = nbytes;
		desc[2] = (uint32_t)((dest_nc ? nc_phys : c_phys) + AREA_OFF);
		desc[0] = DESC_ARMED;
		__sync_synchronize();
		dma[DMA_STREAM0 / 4] = (uint32_t)(nc_phys + 0);
		__sync_synchronize();
		double t0 = now_s();
		src[SRC_CTRL / 4] = 0x1;
		if (wait_done(2000, &stat)) { dma_fail++; break; }
		double t1 = now_s();
		t_wait += t1 - t0;
		if (evict) thrash();
		if (dest_nc) {
			memcpy(copy, (const void *)ncarea, nbytes);
			double t2 = now_s();
			t_copy += t2 - t1;
			bad_frames += verify_counter((volatile uint32_t *)copy, frames, seq);
			t_verify += now_s() - t2;
		} else {
			bad_frames += verify_counter(carea, frames, seq);
			t_verify += now_s() - t1;
		}
		seq += frames;
	}
	double el = now_s() - t_start;
	printf("{\"state\": \"bench\", \"dma_window\": \"%s\", \"read_path\": \"%s\", \"bursts\": %d, \"bytes\": %llu, \"elapsed_s\": %.4f, "
	       "\"mb_per_s_end_to_end\": %.2f, \"dma_wait_ms_per_burst\": %.3f, \"copy_ms_per_burst\": %.2f, \"verify_ms_per_burst\": %.2f, "
	       "\"bad_frames\": %u, \"dma_fail\": %d, \"words\": %u}\n",
	       dest_nc ? "non-cached" : "cached", dest_nc ? "memcpy then verify the copy" : "verify in place",
	       bursts, (unsigned long long)bursts * nbytes, el, (double)bursts * nbytes / el / 1e6, t_wait / bursts * 1e3,
	       t_copy / bursts * 1e3, t_verify / bursts * 1e3, bad_frames, dma_fail, words);
	src[SRC_CTRL / 4] = 0x2;
	free(copy);
	return bad_frames || dma_fail;
}

int main(int argc, char **argv)
{
	const char *state = "cold";
	uint32_t frames = 16;
	int opt, sync_variant = 0, evict = 0, bw = 0, dest_nc = 0, nbursts = 32;

	while ((opt = getopt(argc, argv, "S:f:M:D:Bn:")) != -1) {
		switch (opt) {
		case 'n': nbursts = atoi(optarg); break;
		case 'S': state = optarg; break;
		case 'f': frames = strtoul(optarg, NULL, 0); break;
		case 'M': sync_variant = !strcmp(optarg, "sync"); evict = !strcmp(optarg, "evict"); break;
		case 'D': dest_nc = !strcmp(optarg, "nc"); break;
		case 'B': bw = 1; break;
		default:
			fprintf(stderr, "usage: %s [-S cold|clean|dirty|control|partial|bench] [-n bursts] [-f frames] [-D c|nc] [-M sync|evict] [-B]\n", argv[0]);
			return 2;
		}
	}
	if (strcmp(state, "cold") && strcmp(state, "clean") && strcmp(state, "dirty") && strcmp(state, "control") &&
	    strcmp(state, "partial") && strcmp(state, "bench")) return 2;
	uint32_t words = frames * FLEN, nbytes = words * 4;
	if (!frames || nbytes > 0x100000u) { fprintf(stderr, "1..1024 frames\n"); return 2; }

	cmap = map_udma("udmabuf-ddr-c0", 0, &c_phys);
	ncmap = map_udma("udmabuf-ddr-nc0", 1, &nc_phys);
	if (!cmap || !ncmap) { perror("udmabuf"); return 1; }
	if (bw) { bandwidth(); return 0; }
	if (!strcmp(state, "control")) return run_control(frames);

	int src_fd = open_uio("pfdaq_stream_src", &src), dfd = open_uio("dma-controller@60010000", &dma);
	if (src_fd < 0 || dfd < 0) { perror("uio"); return 1; }
	dma_fd = dfd;
	src = (volatile uint32_t *)((volatile uint8_t *)src + SRC_OFF);
	if (src[SRC_ID / 4] != SRC_ID_VALUE) { fprintf(stderr, "stream source ID %08x\n", src[SRC_ID / 4]); return 1; }
	if (src[SRC_STATUS / 4] & 1) { fprintf(stderr, "stream source busy\n"); return 1; }

	volatile uint32_t *carea = cmap + AREA_OFF / 4, *ncarea = ncmap + AREA_OFF / 4;
	uint32_t seq0, stat = 0;

	/* source and DMA set-up, as pfdaq_cap does it */
	src[SRC_MODE / 4] = 0;
	dma[DMA_MASK / 4] = 0xF;
	dma[DMA_CLEAR / 4] = 0xF;
	{ struct pollfd p = { .fd = dma_fd, .events = POLLIN }; uint32_t c; while (poll(&p, 1, 10) > 0 && read(dma_fd, &c, 4) == 4) ; }
	src[SRC_CTRL / 4] = 0x2;
	usleep(1000);
	seq0 = src[SRC_SNAP_SEQ / 4];
	src[SRC_PAYLOAD / 4] = PAYLOAD;
	src[SRC_FRAMES / 4] = frames;
	src[SRC_GAP / 4] = 0;
	src[SRC_FAULT / 4] = 0;

	if (!strcmp(state, "bench")) {
		int r = run_bench(nbursts, dest_nc, evict);
		return r;
	}
	if (!strcmp(state, "partial")) {
		int r = run_partial(seq0, &stat, dest_nc);
		src[SRC_CTRL / 4] = 0x2;
		return r;
	}

	/* put the area into the requested state; thrash first so nothing of an earlier run is left in a cache */
	thrash();
	if (!strcmp(state, "cold")) {
		fill(ncarea, words, old_w);
		thrash();
	} else if (!strcmp(state, "clean")) {
		fill(ncarea, words, old_w);
		thrash();
		uint32_t s = 0;
		for (uint32_t i = 0; i < words; i++) s += carea[i];
		if (s == 0x12345) putchar(' ');
	} else {                                         /* dirty */
		fill(ncarea, words, pre_w);
		thrash();
		fill(carea, words, old_w);                /* stores through the cached mapping */
	}

	/* descriptor in the non-cached region (offset 0), destination = the CACHED physical address */
	volatile uint32_t *desc = ncmap;
	desc[1] = nbytes;
	desc[2] = (uint32_t)((dest_nc ? nc_phys : c_phys) + AREA_OFF);
	desc[0] = DESC_ARMED;
	__sync_synchronize();
	if (evict) thrash();                                       /* maintenance before the DMA: write back and drop the area's lines */
	dma[DMA_STREAM0 / 4] = (uint32_t)(nc_phys + 0);
	__sync_synchronize();
	src[SRC_CTRL / 4] = 0x1;
	int dma_bad = wait_done(2000, &stat);
	__sync_synchronize();
	if (evict) thrash();                                       /* maintenance after the DMA: drop any copy of the area */

	struct cnt c1, nc1, cs, c2, nc2;
	double t0 = now_s();
	classify(carea, words, seq0, &c1);                         /* 1: cached view, no maintenance */
	double t_read_c = now_s() - t0;
	classify(ncarea, words, seq0, &nc1);                       /* 2: what DDR holds */
	double t_sync = 0;
	memset(&cs, 0, sizeof(cs));
	if (sync_variant) {
		char b[32];
		write_str("/sys/class/u-dma-buf/udmabuf-ddr-c0/sync_offset", "0x400000");
		snprintf(b, sizeof(b), "%u", nbytes);
		write_str("/sys/class/u-dma-buf/udmabuf-ddr-c0/sync_size", b);
		write_str("/sys/class/u-dma-buf/udmabuf-ddr-c0/sync_direction", "2");   /* DMA_FROM_DEVICE */
		double ts = now_s();
		int r = write_str("/sys/class/u-dma-buf/udmabuf-ddr-c0/sync_for_cpu", "1");
		t_sync = now_s() - ts;
		if (r) fprintf(stderr, "sync_for_cpu write failed\n");
		classify(carea, words, seq0, &cs);                 /* 3: after the kernel's sync call */
	}
	thrash();                                                  /* 4: push the area's lines out */
	classify(carea, words, seq0, &c2);
	classify(ncarea, words, seq0, &nc2);

	printf("{\"state\": \"%s\", \"dma_window\": \"%s\", \"maintenance\": \"%s\", \"frames\": %u, \"bytes\": %u, \"dma_ok\": %s, \"dma_stat\": \"%#x\", \"seq0\": %u, \"counted_words\": %u, ",
	       state, dest_nc ? "non-cached" : "cached", sync_variant ? "sync_for_cpu" : evict ? "evict" : "none", frames, nbytes,
	       dma_bad ? "false" : "true", stat, seq0, c1.counted);
	json_cnt("c1_cached_view", &c1, 0);
	json_cnt("nc1_ddr_view", &nc1, 0);
	if (sync_variant) json_cnt("c_after_sync_for_cpu", &cs, 0);
	json_cnt("c2_after_8MiB_read", &c2, 0);
	json_cnt("nc2_ddr_after_8MiB_read", &nc2, 1);
	printf(", \"first_read_us\": %.0f%s", t_read_c * 1e6, sync_variant ? "" : "");
	if (sync_variant) printf(", \"sync_for_cpu_us\": %.0f", t_sync * 1e6);
	printf("}\n");

	src[SRC_CTRL / 4] = 0x2;                                   /* leave the source as pfdaq_cap does */
	return dma_bad ? 1 : 0;
}
