/* SPDX-License-Identifier: GPL-2.0-only */
/* ARM64 QEMU init. Run against the Android 6.1 memcomp kernel, without swap. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/userfaultfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum { PAGES = 256, PAGE = 4096, BYTES = PAGE * PAGES };
#define SWAPPED (UINT64_C(1) << 62)
#define PRESENT (UINT64_C(1) << 63)

static void require(_Bool condition, const char *what)
{
	if (condition)
		return;
	printf("REGRESSION FAIL: %s (errno %d)\n", what, errno);
	if (getpid() == 1)
		reboot(RB_POWER_OFF);
	_exit(1);
}

static unsigned char *area(void)
{
	unsigned char *p = mmap(NULL, BYTES, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	require(p != MAP_FAILED, "mmap");
	require(!madvise(p, BYTES, MADV_NOHUGEPAGE), "MADV_NOHUGEPAGE");
	return p;
}

static long metric(const char *path, const char *key)
{
	FILE *file = fopen(path, "r");
	char line[512], name[100];
	long value;

	require(file != NULL, path);
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%99s %ld", name, &value) == 2 && !strcmp(name, key)) {
			fclose(file);
			return value;
		}
	}
	require(0, key);
	return -1;
}

static long stored_bytes(void)
{
	FILE *file = fopen("/sys/kernel/mm/memcomp/stat", "r");
	long stored = -1;
	require(file && fscanf(file, "%ld", &stored) == 1, "memcomp/stat");
	fclose(file);
	return stored;
}

static long count(const char *key)
{
	return metric("/sys/kernel/mm/memcomp/execution_events", key);
}

static uint64_t pagemap(const void *address)
{
	static int fd = -1;
	uint64_t entry = 0;
	if (fd < 0)
		fd = open("/proc/self/pagemap", O_RDONLY);
	require(fd >= 0 && pread(fd, &entry, sizeof(entry),
		(off_t)((uintptr_t)address / PAGE) * sizeof(entry)) == sizeof(entry),
		"read pagemap");
	return entry;
}

static unsigned char pattern(size_t i)
{
	return i / PAGE == 1 ? 0 : 1 + (i / PAGE + i % 16) % 251;
}

static void check_compressed_pageout(unsigned char *page)
{
	/* Read the recent kernel log without consuming it (SYSLOG_ACTION_READ_ALL). */
	enum { READ_ALL = 3 };
	char log[8192];
	uint64_t entry = pagemap(page);
	int len;

	require(!madvise(page, PAGE, MADV_PAGEOUT), "repeat pageout");
	require(!madvise(page, PAGE, MADV_COLD), "cold on compressed page");
	require(pagemap(page) == entry, "repeated advice preserves token");
	len = klogctl(READ_ALL, log, sizeof(log) - 1);
	require(len >= 0, "read kernel log after repeated advice");
	log[len] = '\0';
	require(!strstr(log, "Bad swap file entry"), "compressed token reached swapfile");
	puts("PASS repeated PAGEOUT/COLD on a compressed page");
}
static void compressed_move_and_stats(void)
{
	unsigned char *src = area(), *dst = area(), vec[PAGES];
	struct uffdio_api api = { .api = UFFD_API, .features = UFFD_FEATURE_MOVE };
	struct uffdio_register reg = {
		.range = { .start = (uintptr_t)dst, .len = BYTES },
		.mode = UFFDIO_REGISTER_MODE_MISSING,
	};
	int fd = syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	require(fd >= 0 && !ioctl(fd, UFFDIO_API, &api), "userfaultfd API");
	require(!ioctl(fd, UFFDIO_REGISTER, &reg), "UFFDIO_REGISTER");
	require(reg.ioctls & (1ULL << _UFFDIO_MOVE), "advertised UFFDIO_MOVE");
	for (size_t i = 0; i < BYTES; i++)
		src[i] = pattern(i);
	require(!madvise(src, BYTES, MADV_PAGEOUT), "pageout");
	require(!mincore(src, BYTES, vec), "mincore");
	for (int i = 0; i < PAGES; i++) {
		uint64_t entry = pagemap(src + i * PAGE);
		require(entry & SWAPPED, "source compressed before MOVE");
		require(vec[i] & 1, "compressed page reported resident");
	}
	require(((pagemap(src + PAGE) >> 5) & ((UINT64_C(1) << 50) - 1)) == 0,
		"zero page uses token zero");
	check_compressed_pageout(src);
	require(!mincore(dst, BYTES, vec) && !(vec[0] & 1), "unmapped hole");
	long vm = metric("/proc/self/status", "VmSwap:");
	long swap = metric("/proc/self/smaps_rollup", "Swap:");
	long pss = metric("/proc/self/smaps_rollup", "SwapPss:");
	printf("accounting kB: VmSwap=%ld Swap=%ld SwapPss=%ld\n", vm, swap, pss);
	require(vm == BYTES / 1024 && vm == swap && vm == pss, "private swap totals");
	for (int i = 0; i < 2; i++) {
		struct uffdio_move mv = {
			.src = (uintptr_t)(src + i * PAGE), .dst = (uintptr_t)(dst + i * PAGE),
			.len = PAGE, .mode = UFFDIO_MOVE_MODE_DONTWAKE,
		};
		uint64_t old = pagemap(src + i * PAGE);
		require(!ioctl(fd, UFFDIO_MOVE, &mv) && mv.move == PAGE, "UFFDIO_MOVE");
		require(!(pagemap(src + i * PAGE) & (SWAPPED | PRESENT)), "source vacated");
		require(pagemap(dst + i * PAGE) == old, "destination retains token");
		for (size_t j = 0; j < PAGE; j++)
			require(dst[i * PAGE + j] == pattern(i * PAGE + j), "moved bytes");
	}
	int gate[2];
	require(!pipe(gate), "child barrier");
	pid_t child = fork();
	require(child >= 0, "fork");
	if (!child) {
		long logical = metric("/proc/self/smaps_rollup", "Swap:");
		long share = metric("/proc/self/smaps_rollup", "SwapPss:");
		printf("shared accounting kB: Swap=%ld SwapPss=%ld\n", logical, share);
		require(logical == (PAGES - 2) * PAGE / 1024 && 2 * share == logical,
			"fork proportional swap");
		close(gate[1]);
		char done;
		require(read(gate[0], &done, 1) == 1, "parent checked shared MOVE");
		_exit(0);
	}
	close(gate[0]);
	struct uffdio_move shared = {
		.src = (uintptr_t)(src + 2 * PAGE), .dst = (uintptr_t)(dst + 2 * PAGE),
		.len = PAGE,
	};
	require(ioctl(fd, UFFDIO_MOVE, &shared) == -1 && errno == EBUSY,
		"shared token retains exclusivity protection");
	require(pagemap(src + 2 * PAGE) & SWAPPED, "rejected MOVE retains source");
	require(write(gate[1], "x", 1) == 1, "release child");
	close(gate[1]);
	int status;
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
		"child accounting");
	for (size_t i = 2 * PAGE; i < BYTES; i++)
		require(src[i] == pattern(i), "all remaining bytes");
	close(fd);
	munmap(src, BYTES);
	munmap(dst, BYTES);
	puts("PASS compressed MOVE (positive and zero tokens), smaps, mincore and data");
}

static void rollback(int pin_failure)
{
	uint64_t *buf = (void *)area();
	uint64_t state = 1234567;
	long before = stored_bytes(), restored = count("restore_ptes");
	long missed = count("restore_missed_pages");

	if (pin_failure)
		require(!prctl(PR_SET_NAME, "v9-pin-check"), "test comm");
	for (int pass = 0; pass < 2; pass++) {
		state = 1234567;
		for (size_t i = 0; i < BYTES / sizeof(*buf); i++) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			uint64_t word = pin_failure ? pattern(i) : state;
			if (!pass)
				buf[i] = word;
			else
				require(buf[i] == word, "rollback bytes");
		}
		if (!pass) {
			require(!madvise(buf, BYTES, MADV_PAGEOUT), "rollback pageout");
			for (int i = 0; i < PAGES; i++)
				require(pagemap((char *)buf + i * PAGE) & PRESENT,
					"original mapping restored");
		}
	}
	munmap(buf, BYTES);
	long after = stored_bytes();
	long mapped_back = count("restore_ptes") - restored;
	printf("rollback pin=%d stored=%ld->%ld restored=%ld\n",
	       pin_failure, before, after, mapped_back);
	require(after == before && count("restore_missed_pages") == missed, "balanced rollback");
	if (!pin_failure)
		require(mapped_back >= PAGES, "rmap restored PTEs");
	puts(pin_failure ? "PASS busy-pin cleanup" : "PASS PMD/PTE rollback");
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	require(getpid() == 1 && sysconf(_SC_PAGESIZE) == PAGE, "4K QEMU init");
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	require(!mount("proc", "/proc", "proc", 0, NULL), "mount proc");
	require(!mount("sysfs", "/sys", "sysfs", 0, NULL), "mount sysfs");
	compressed_move_and_stats();
	rollback(0);
	if (argc > 1 && !strcmp(argv[1], "pin-return"))
		rollback(1);
	puts("ALL MEMCOMP REGRESSIONS PASSED");
	reboot(RB_POWER_OFF);
	return 1;
}
