/* SPDX-License-Identifier: GPL-2.0-only */
/* Build with gcc -O2 -static -pthread cross-codec.c -o init for a 4K ARM64
 * guest. Boot as rdinit=/init with 4 vCPUs. Default: no-swap integrity.
 * For matched 512 MiB pressure runs, append "-- lz4", "-- zstd", or
 * "-- switch". Optional "-- writeback" enables the disposable guest ram0.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEMCOMP "/sys/kernel/mm/memcomp/"
enum { PAGE = 4096, PAGES = 512, SIZE = PAGE * PAGES, WORKERS = 3 };
static const char *const codecs[] = { "lz4", "zstd", "lz4" };
static int pm;
static pthread_barrier_t phase;

struct batch {
	unsigned char *data;
	unsigned int id;
	uint64_t start[2], end[2];
};

struct usage {
	long stored, compressed, raw, samefill;
};

static void fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("CROSS_CODEC_FAIL ", stdout);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	reboot(RB_POWER_OFF);
	_exit(1);
}

static void syscheck(int failed, const char *what)
{
	if (failed)
		fail("%s: %s", what, strerror(errno));
}

static void sync_phase(void)
{
	int rc = pthread_barrier_wait(&phase);
	if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD)
		fail("barrier: %s", strerror(rc));
}

static void expect_codec(const char *name)
{
	char current[32];
	FILE *f = fopen(MEMCOMP "compressor", "r");
	syscheck(!f, "read compressor");
	if (fscanf(f, "%31s", current) != 1 || strcmp(current, name))
		fail("expected compressor %s", name);
	fclose(f);
}

static void select_codec(const char *value, const char *expected, int invalid)
{
	int fd = open(MEMCOMP "compressor", O_WRONLY);
	syscheck(fd < 0, "open writable compressor");
	size_t len = strlen(value);
	ssize_t n = write(fd, value, len);
	int err = errno;
	close(fd);
	if ((invalid && (n != -1 || err != EINVAL)) || (!invalid && n != (ssize_t)len))
		fail("write %s returned %zd, errno=%d", value, n, err);
	expect_codec(expected);
	if (invalid)
		printf("INVALID rejected errno=%d retained=%s\n", err, expected);
}

static struct usage usage(void)
{
	struct usage u = { .raw = -1, .samefill = -1 };
	FILE *f = fopen(MEMCOMP "stat", "r");
	syscheck(!f, "memcomp/stat");
	if (fscanf(f, "%ld%ld", &u.stored, &u.compressed) != 2)
		fail("parse memcomp/stat");
	fclose(f);
	f = fopen(MEMCOMP "events", "r");
	syscheck(!f, "memcomp/events (DEBUG_STATS)");
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "raw_pages %ld", &u.raw) == 1)
			continue;
		sscanf(line, "samefill_pages %ld", &u.samefill);
	}
	fclose(f);
	if (u.raw < 0 || u.samefill < 0)
		fail("missing raw_pages/samefill_pages");
	return u;
}

static void expect_usage(struct usage base, int batches)
{
	struct usage u = usage();
	long stored = u.stored - base.stored;
	long payload = u.compressed - base.compressed;

	printf("STORE batch_count=%d stored_delta=%ld compressed_delta=%ld raw_delta=%ld samefill_delta=%ld\n",
	       batches, stored, payload, u.raw - base.raw, u.samefill - base.samefill);
	if (stored != (long)batches * SIZE || u.raw != base.raw || u.samefill != base.samefill ||
	    (batches ? payload <= 0 || payload >= stored : payload != 0))
		fail("unexpected memcomp accounting");
}

/* Each page repeats a short dictionary. Both its page and batch choose the bytes. */
static unsigned char pattern(unsigned int id, unsigned int pg, unsigned int off)
{
	uint32_t key = (id * PAGES + pg + 1) * UINT32_C(2654435761);
	return (key >> (8 * (off % 4))) ^ (off % 64) ^ (off / 512);
}

static struct batch prepare(unsigned int id)
{
	struct batch b = { .id = id };
	b.data = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	syscheck(b.data == MAP_FAILED, "mmap");
	syscheck(madvise(b.data, SIZE, MADV_NOHUGEPAGE), "MADV_NOHUGEPAGE");
	for (unsigned int pg = 0; pg < PAGES; pg++)
		for (unsigned int off = 0; off < PAGE; off++)
			b.data[pg * PAGE + off] = pattern(b.id, pg, off);
	/* Drain the producer CPU's pending LRU additions before handing pages
	 * to PAGEOUT workers on other vCPUs. */
	syscheck(madvise(b.data, SIZE, MADV_COLD), "MADV_COLD after fill");
	return b;
}

static unsigned int inspect_pages(const struct batch *b, int retry_present)
{
	uint64_t entries[PAGES];
	unsigned int remaining = 0;
	off_t where = ((uintptr_t)b->data / PAGE) * sizeof(entries[0]);
	if (pread(pm, entries, sizeof(entries), where) != sizeof(entries))
		fail("pagemap batch=%u", b->id);
	for (unsigned int pg = 0; pg < PAGES; pg++) {
		if ((entries[pg] >> 62) == 1)
			continue;
		if (!retry_present || !(entries[pg] >> 63))
			fail("not swapped batch=%u page=%u entry=%016" PRIx64, b->id, pg, entries[pg]);
		remaining++;
		syscheck(madvise(b->data + pg * PAGE, PAGE, MADV_PAGEOUT), "retry present page");
	}
	return remaining;
}

static void check_swapped(const struct batch *b)
{
	inspect_pages(b, 0);
}

/* PAGEOUT is advisory. Count misses and retry only still-present pages. */
static void finish_pageout(const struct batch *b)
{
	for (int pass = 0; pass < 8; pass++) {
		unsigned int remaining = inspect_pages(b, 1);
		if (!remaining)
			return;
		printf("PAGEOUT_RETRY batch=%u pass=%d present=%u\n", b->id, pass + 1, remaining);
		usleep(1000);
	}
	check_swapped(b);
}

static void pageout(const struct batch *b)
{
	syscheck(madvise(b->data, SIZE, MADV_PAGEOUT), "MADV_PAGEOUT");
	finish_pageout(b);
}

static void verify_page(const struct batch *b, unsigned int pg)
{
	for (unsigned int off = 0; off < PAGE; off++) {
		unsigned char got = b->data[pg * PAGE + off], want = pattern(b->id, pg, off);
		if (got != want)
			fail("batch=%u page=%u byte=%u got=%02x expected=%02x",
			     b->id, pg, off, got, want);
	}
}

static void mixed_batches(void)
{
	struct batch batches[3];
	struct usage base = usage();
	for (int i = 0; i < 3; i++) {
		select_codec(codecs[i], codecs[i], 0);
		batches[i] = prepare(i);
		pageout(&batches[i]);
		select_codec("invalid-codec", codecs[i], 1);
		for (int j = 0; j <= i; j++)
			check_swapped(&batches[j]);
		expect_usage(base, i + 1);
	}
	/* Fault each codec under the other selection with all generations retained. */
	select_codec("zstd", "zstd", 0);
	for (int i = 0; i < 3; i += 2)
		verify_page(&batches[i], 0);
	select_codec("lz4", "lz4", 0);
	for (unsigned int pg = 0; pg < PAGES; pg++)
		for (int i = 0; i < 3; i++)
			verify_page(&batches[i], pg);
	for (int i = 0; i < 3; i++)
		syscheck(munmap(batches[i].data, SIZE), "unmap mixed batch");
	expect_usage(base, 0);
	printf("PASS mixed lz4/zstd/lz4 pages=%d verified_bytes=%d\n", 3 * PAGES, 3 * SIZE);
}

static uint64_t now_ns(void)
{
	struct timespec t;
	syscheck(clock_gettime(CLOCK_MONOTONIC, &t), "clock_gettime");
	return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

static void *worker(void *arg)
{
	struct batch *b = arg;
	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	CPU_SET(b->id % WORKERS, &cpus);
	int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
	if (rc)
		fail("worker affinity: %s", strerror(rc));
	*b = prepare(b->id);
	sync_phase();
	b->start[0] = now_ns();
	syscheck(madvise(b->data, SIZE, MADV_PAGEOUT), "concurrent MADV_PAGEOUT");
	b->end[0] = now_ns();
	finish_pageout(b);
	sync_phase();
	sync_phase();
	b->start[1] = now_ns();
	for (unsigned int pg = 0; pg < PAGES; pg++)
		verify_page(b, pg);
	b->end[1] = now_ns();
	return NULL;
}

enum { SWITCHES = 32 };

static void switch_repeatedly(uint64_t times[SWITCHES])
{
	for (int i = 0; i < SWITCHES; i++) {
		const char *name = codecs[(i + 1) % 2];
		select_codec(name, name, 0);
		times[i] = now_ns();
	}
}

static void concurrent_batches(void)
{
	struct usage base = usage();
	struct batch batches[WORKERS];
	pthread_t threads[WORKERS];
	uint64_t times[2][SWITCHES];
	unsigned int overlap[2] = { 0, 0 };
	int rc = pthread_barrier_init(&phase, NULL, WORKERS + 1);
	if (rc)
		fail("pthread_barrier_init: %s", strerror(rc));
	for (int round = 0; round < 4; round++) {
		for (int i = 0; i < WORKERS; i++) {
			batches[i].id = 3 + round * WORKERS + i;
			rc = pthread_create(&threads[i], NULL, worker, &batches[i]);
			if (rc)
				fail("pthread_create: %s", strerror(rc));
		}
		sync_phase();
		switch_repeatedly(times[0]);
		sync_phase();
		expect_usage(base, WORKERS);
		for (int i = 0; i < WORKERS; i++)
			check_swapped(&batches[i]);
		sync_phase();
		switch_repeatedly(times[1]);
		for (int i = 0; i < WORKERS; i++) {
			rc = pthread_join(threads[i], NULL);
			if (rc)
				fail("pthread_join: %s", strerror(rc));
			syscheck(munmap(batches[i].data, SIZE), "unmap concurrent batch");
		}
		for (int step = 0; step < 2; step++)
			for (int n = 0; n < SWITCHES; n++)
				for (int i = 0; i < WORKERS; i++)
					if (times[step][n] >= batches[i].start[step] &&
					    times[step][n] <= batches[i].end[step]) {
						overlap[step]++;
						break;
					}
		expect_usage(base, 0);
		printf("PASS concurrent round=%d workers=%d switches=64 verified_bytes=%d\n",
		       round + 1, WORKERS, WORKERS * SIZE);
	}
	printf("OVERLAP switch_readbacks_during_pageout=%u during_refault_verify=%u\n",
	       overlap[0], overlap[1]);
	if (!overlap[0] || !overlap[1])
		fail("concurrent switching was not observed in both phases");
	pthread_barrier_destroy(&phase);
}

#include "codec-pressure.h"

int main(int argc, char **argv)
{
	if (getpid() != 1)
		return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	syscheck(mount("proc", "/proc", "proc", 0, NULL), "mount proc");
	syscheck(mount("sysfs", "/sys", "sysfs", 0, NULL), "mount sysfs");
	if (sysconf(_SC_PAGESIZE) != PAGE || sysconf(_SC_NPROCESSORS_ONLN) < WORKERS)
		fail("expected 4K pages and three or more vCPUs");
	FILE *f = fopen("/proc/swaps", "r");
	char line[256];
	syscheck(!f, "/proc/swaps");
	if (!fgets(line, sizeof(line), f) || fgets(line, sizeof(line), f))
		fail("expected no active swap");
	fclose(f);
	pm = open("/proc/self/pagemap", O_RDONLY);
	syscheck(pm < 0, "open pagemap");
	if (argc == 2) {
		pressure_run(argv[1]);
		reboot(RB_POWER_OFF);
		return 1;
	}
	mixed_batches();
	concurrent_batches();
	close(pm);
	puts("ALL CROSS-CODEC REGRESSIONS PASSED");
	reboot(RB_POWER_OFF);
	return 1;
}
