/* SPDX-License-Identifier: GPL-2.0-only */
/* ARM64 initramfs /init for the test-only codec-alloc-fail.h kernel. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BASE "/sys/kernel/mm/memcomp/"
enum { PAGE = 4096, OLD = 2048, CHURN = 256 };
static atomic_ulong faults;
static atomic_int stop;
static int pm;

static void die(const char *msg)
{
	printf("FAIL fault-test: %s (errno=%d)\n", msg, errno);
	reboot(RB_POWER_OFF);
	_exit(1);
}

static uint64_t nanos(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		die("clock_gettime");
	return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static unsigned char byte_at(unsigned int page, unsigned int off)
{
	uint32_t key = (page + 1U) * 2654435761U;
	return (key >> (8 * (off % 4))) ^ (off % 97) ^ (off / 512);
}

static unsigned char *region(unsigned int pages, unsigned int id)
{
	size_t len = (size_t)pages * PAGE;
	unsigned char *buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED || madvise(buf, len, MADV_NOHUGEPAGE))
		die("allocating anonymous pages");
	for (unsigned int p = 0; p < pages; p++)
		for (unsigned int i = 0; i < PAGE; i++)
			buf[(size_t)p * PAGE + i] = byte_at(id + p, i);
	return buf;
}

static void verify(const unsigned char *buf, unsigned int pages, unsigned int id)
{
	for (unsigned int p = 0; p < pages; p++)
		for (unsigned int i = 0; i < PAGE; i++)
			if (buf[(size_t)p * PAGE + i] != byte_at(id + p, i))
				die("refault data mismatch");
}

static unsigned int swapped(const unsigned char *buf, unsigned int pages)
{
	uint64_t entries[OLD];
	unsigned int total = 0;
	size_t count = pages * sizeof(*entries);
	off_t where = ((uintptr_t)buf / PAGE) * sizeof(*entries);
	if (pages > OLD || pread(pm, entries, count, where) != (ssize_t)count)
		die("reading pagemap");
	for (unsigned int p = 0; p < pages; p++)
		total += entries[p] >> 62 == 1;
	return total;
}

static void *churn(void *data)
{
	unsigned char *buf = data;
	while (!atomic_load(&stop)) {
		if (madvise(buf, CHURN * PAGE, MADV_PAGEOUT))
			die("concurrent madvise");
		unsigned int reclaimed = swapped(buf, CHURN);
		verify(buf, CHURN, OLD);
		atomic_fetch_add(&faults, reclaimed);
	}
	return NULL;
}

static unsigned long counter(const char *filename, const char *key)
{
	char line[256], name[80];
	unsigned long n, out = 0;
	int found = 0;
	FILE *fp = fopen(filename, "r");
	if (!fp)
		die(filename);
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%79s %lu", name, &n) == 2 && !strcmp(name, key)) {
			out = n;
			found = 1;
			break;
		}
	}
	fclose(fp);
	if (!found)
		die(key);
	return out;
}

static unsigned long stores(void)
{
	return counter(BASE "events", "pageout_store_ok");
}

static void expect_codec(const char *codec)
{
	char s[32];
	FILE *fp = fopen(BASE "compressor", "r");
	if (!fp || fscanf(fp, "%31s", s) != 1 || strcmp(s, codec))
		die("unexpected active codec");
	fclose(fp);
}

static void set_codec(const char *codec, int expected)
{
	int fd = open(BASE "compressor", O_WRONLY);
	if (fd < 0)
		die("opening compressor");
	size_t n = strlen(codec);
	uint64_t start = nanos();
	ssize_t rc = write(fd, codec, n);
	int err = rc < 0 ? errno : 0;
	uint64_t duration = nanos() - start;
	printf("CODEC request=%s rc=%zd errno=%d usec=%.3f\n",
	       codec, rc, err, duration / 1000.);
	if (err != expected || (!err && rc != (ssize_t)n))
		die("unexpected sysfs response");
	close(fd);
}

static void wait_progress(unsigned long prev_stores, unsigned long prev_faults)
{
	uint64_t start = nanos();
	while (stores() <= prev_stores || atomic_load(&faults) <= prev_faults) {
		if (nanos() - start > 5000000000ULL)
			die("five seconds without reclaim/refault progress");
		usleep(2000);
	}
}

int main(void)
{
	if (getpid() != 1)
		return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL))
		die("mount");
	if (sysconf(_SC_PAGESIZE) != PAGE)
		die("page size");
	FILE *sw = fopen("/proc/swaps", "r");
	char line[180];
	if (!sw || !fgets(line, sizeof(line), sw) || fgets(line, sizeof(line), sw))
		die("guest requires no active disk swap");
	fclose(sw);
	pm = open("/proc/self/pagemap", O_RDONLY);
	if (pm < 0)
		die("pagemap");
	expect_codec("lz4");
	unsigned char *old = region(OLD, 0);
	unsigned char *working = region(CHURN, OLD);
	unsigned long raw = counter(BASE "events", "raw_pages");
	unsigned long same = counter(BASE "events", "samefill_pages");
	if (madvise(old, OLD * PAGE, MADV_PAGEOUT) || swapped(old, OLD) != OLD ||
	    raw != counter(BASE "events", "raw_pages") ||
	    same != counter(BASE "events", "samefill_pages"))
		die("expected old pages to be in compressed storage");
	/* Touch more anonymous memory than the available headroom. kswapd
	 * can reclaim these compressible pages while independent PAGEOUT and
	 * fault operations continue around the failing request. */
	unsigned long available = counter("/proc/meminfo", "MemAvailable:");
	unsigned int pressure_pages = (available * 105 / 100) / (PAGE / 1024);
	unsigned long scans0 = counter("/proc/vmstat", "pgscan_kswapd");
	unsigned long bg0 = counter(BASE "events", "bg_store_ok");
	unsigned char *pressure = region(pressure_pages, OLD + CHURN);
	pthread_t t;
	if (pthread_create(&t, NULL, churn, working))
		die("starting reclaim worker");
	wait_progress(stores(), atomic_load(&faults));
	if (swapped(old, OLD) != OLD)
		die("old pages lost before failure");
	unsigned long pre_s = stores(), pre_f = atomic_load(&faults);
	set_codec("zstd", ENOMEM);
	expect_codec("lz4");
	unsigned long fail_s = stores(), fail_f = atomic_load(&faults);
	uint64_t start = nanos();
	verify(old, OLD, 0);
	printf("OLD_REFAULT pages=%u verified_bytes=%u usec=%.3f\n",
	       OLD, OLD * PAGE, (nanos() - start) / 1000.);
	/* Sample a full second of post-failure progress rather than counting
	 * completions that were already in flight at the time of failure. */
	usleep(1000000);
	wait_progress(fail_s, fail_f);
	printf("PROGRESS before=%lu/%lu failure=%lu/%lu after=%lu/%lu (PAGEOUT-stores/verified-refaults)\n",
	       pre_s, pre_f, fail_s, fail_f, stores(), atomic_load(&faults));
	unsigned long scans = counter("/proc/vmstat", "pgscan_kswapd") - scans0;
	unsigned long bg = counter(BASE "events", "bg_store_ok") - bg0;
	printf("PRESSURE allocated_MiB=%u kswapd_scans=%lu bg_store_ok_delta=%lu\n",
	       pressure_pages / 256, scans, bg);
	if (!scans || !bg)
		die("no background pressure evidence");
	set_codec("zstd", 0);
	expect_codec("zstd");
	atomic_store(&stop, 1);
	pthread_join(t, NULL);
	if (madvise(old, OLD * PAGE, MADV_PAGEOUT) || swapped(old, OLD) != OLD)
		die("storing the retry batch under Zstd");
	set_codec("lz4", 0);
	expect_codec("lz4");
	verify(old, OLD, 0);
	printf("RETRY_REFAULT zstd_to_lz4 pages=%u verified_bytes=%u\n", OLD, OLD * PAGE);
	munmap(pressure, (size_t)pressure_pages * PAGE);
	puts("PASS ALLOC FAILURE AND RECOVERY");
	reboot(RB_POWER_OFF);
	return 1;
}
