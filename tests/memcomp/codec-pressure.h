/* SPDX-License-Identifier: GPL-2.0-only */
/* Matched workload for a fresh 512 MiB guest per arm. See pressure_run(). */
#include <sys/swap.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <stdatomic.h>

enum { EPOCHS = 24, REQUESTS = 5, RING = 4, HOT_MIB = 640, STEP_MIB = 48 };
enum { HOT_PAGES = HOT_MIB * 256, STEP_PAGES = STEP_MIB * 256 };
static volatile unsigned int *hot;
static struct batch ring[RING];
static uint64_t delay_ns[EPOCHS * PAGES], access_at[EPOCHS * PAGES];
static long scan_delta[EPOCHS];
struct request {
	uint64_t start, end;
	const char *to;
};
static uint64_t heat_start[EPOCHS], heat_end[EPOCHS], read_start[EPOCHS], read_end[EPOCHS];
static uint64_t request_max, direction_max[2];
static unsigned int timeouts, writes, directions[2], direction_timeouts[2];
static atomic_int pending_request;

static long counter(const char *path, const char *name)
{
	FILE *f = fopen(path, "r");
	char line[256], key[100];
	long value;
	syscheck(!f, path);
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%99s%ld", key, &value) == 2 && !strcmp(key, name)) {
			fclose(f);
			return value;
		}
	fail("missing %s in %s", name, path);
	return -1;
}

static struct counter_spec {
	const char *path, *key;
	long before;
} counters[] = {
	{ "/proc/vmstat", "pgscan_kswapd", 0 }, { "/proc/vmstat", "pgscan_direct", 0 },
	{ "/proc/vmstat", "pgsteal_kswapd", 0 }, { "/proc/vmstat", "pgsteal_direct", 0 },
	{ "/proc/vmstat", "pgmajfault", 0 }, { "/proc/vmstat", "oom_kill", 0 },
	{ MEMCOMP "events", "bg_store_ok", 0 }, { MEMCOMP "events", "direct_store_ok", 0 },
	{ MEMCOMP "events", "pageout_store_ok", 0 }, { MEMCOMP "events", "swapped_read_io", 0 },
	{ MEMCOMP "packed_swap_events", "writeback_pressure_events", 0 },
	{ MEMCOMP "packed_swap_events", "writeback_gate_opens", 0 },
	{ MEMCOMP "packed_swap_events", "writeback_payloads", 0 },
	{ MEMCOMP "packed_swap_events", "writeback_packed_slots", 0 },
	{ MEMCOMP "packed_swap_events", "writeback_packed_payloads", 0 },
};

static void pressure_counters(int done)
{
	for (size_t i = 0; i < sizeof(counters) / sizeof(counters[0]); i++) {
		struct counter_spec *c = &counters[i];
		long value = counter(c->path, c->key);
		if (done)
			printf("COUNTER %s delta=%ld\n", c->key, value - c->before);
		else
			c->before = value;
	}
}

static void *churn(void *unused)
{
	(void)unused;
	unsigned int page = 0;
	for (int epoch = 0; epoch < EPOCHS; epoch++) {
		sync_phase();
		heat_start[epoch] = now_ns();
		for (unsigned int i = 0; i < STEP_PAGES; i++) {
			hot[page * (PAGE / sizeof(*hot))]++;
			page = (page + 1) % HOT_PAGES;
		}
		heat_end[epoch] = now_ns();
		sync_phase();
	}
	return NULL;
}

static void *timed_refault(void *unused)
{
	(void)unused;
	for (int epoch = 0; epoch < EPOCHS; epoch++) {
		struct batch *b = &ring[epoch % RING];
		sync_phase();
		long scan = counter("/proc/vmstat", "pgscan_kswapd");
		read_start[epoch] = now_ns();
		for (unsigned int n = 0; n < PAGES; n++) {
			unsigned int pg = (n * 73) % PAGES;
			uint64_t t = now_ns();
			unsigned char first = *(volatile unsigned char *)(b->data + pg * PAGE);
			delay_ns[epoch * PAGES + n] = now_ns() - t;
			access_at[epoch * PAGES + n] = t;
			if (first != pattern(b->id, pg, 0))
				fail("first byte batch=%u page=%u", b->id, pg);
			verify_page(b, pg);
		}
		read_end[epoch] = now_ns();
		scan_delta[epoch] = counter("/proc/vmstat", "pgscan_kswapd") - scan;
		sync_phase();
	}
	return NULL;
}

static void *request_watchdog(void *unused)
{
	(void)unused;
	/* An outstanding syscall must count even if it never returns. */
	for (;;) {
		int id = atomic_load(&pending_request);
		sleep(2);
		if (id > 0 && id == atomic_load(&pending_request))
			fail("sysfs timeout request=%d elapsed_ns>=2000000000 timeouts=1", id);
	}
	return NULL;
}

static struct request timed_select(const char *from, const char *to)
{
	int id = ++writes;
	atomic_store(&pending_request, id);
	uint64_t begin = now_ns();
	int fd = open(MEMCOMP "compressor", O_WRONLY);
	syscheck(fd < 0, "open compressor");
	size_t len = strlen(to);
	ssize_t n = write(fd, to, len);
	uint64_t end = now_ns();
	atomic_store(&pending_request, 0);
	syscheck(n != (ssize_t)len, "write compressor");
	close(fd);
	expect_codec(to);
	uint64_t elapsed = end - begin;
	if (elapsed > request_max)
		request_max = elapsed;
	if (elapsed >= UINT64_C(2000000000))
		timeouts++;
	if (strcmp(from, to)) {
		int direction = !strcmp(to, "lz4");
		directions[direction]++;
		if (elapsed > direction_max[direction])
			direction_max[direction] = elapsed;
		if (elapsed >= UINT64_C(2000000000))
			direction_timeouts[direction]++;
	}
	printf("REQUEST id=%d from=%s to=%s latency_ns=%" PRIu64 "\n", id, from, to, elapsed);
	return (struct request){ begin, end, to };
}

static int order_ns(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

/* Enable only on this throwaway QEMU guest, with a built-in RAM disk. */
static void enable_guest_swap(void)
{
	mkdir("/dev", 0755);
	syscheck(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL), "mount devtmpfs");
	int fd = open("/dev/ram0", O_RDWR);
	syscheck(fd < 0, "open ram0");
	uint64_t bytes;
	syscheck(ioctl(fd, BLKGETSIZE64, &bytes), "brd size");
	uint32_t header[PAGE / 4] = { 0 };
	header[256] = 1;
	header[257] = bytes / PAGE - 1;
	memcpy((char *)header + PAGE - 10, "SWAPSPACE2", 10);
	syscheck(write(fd, header, PAGE) != PAGE, "brd swap header");
	close(fd);
	syscheck(swapon("/dev/ram0", 0), "swapon ram0");
	printf("SWAP ram0_bytes=%" PRIu64 "\n", bytes);
}

static void pressure_run(const char *mode)
{
	int dynamic = !strcmp(mode, "switch") || !strcmp(mode, "writeback");
	if (!dynamic && strcmp(mode, "lz4") && strcmp(mode, "zstd"))
		fail("unknown pressure mode %s", mode);
	const char *alg = !strcmp(mode, "zstd") ? "zstd" : "lz4";
	const char *generation[RING];
	pthread_t heat_thread, reader, watchdog;
	unsigned int overlap_epochs = 0, overlap_requests = 0, cross_generation_pages = 0;
	unsigned int cross_lz4 = 0, cross_zstd = 0;
	unsigned int scan_epochs = 0;
	long peak_raw = 0, peak_samefill = 0;
	struct request req[REQUESTS];

	if (!strcmp(mode, "writeback"))
		enable_guest_swap();
	select_codec(alg, alg, 0);
	hot = mmap(NULL, (size_t)HOT_MIB << 20, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	syscheck(hot == MAP_FAILED, "mmap pressure set");
	syscheck(madvise((void *)hot, (size_t)HOT_MIB << 20, MADV_NOHUGEPAGE), "no THP pressure");
	for (unsigned int page = 0; page < HOT_PAGES; page++)
		hot[page * (PAGE / sizeof(*hot))] = page + 1;
	for (int i = 0; i < RING; i++) {
		ring[i] = prepare(i);
		pageout(&ring[i]);
		generation[i] = alg;
	}
	printf("PRESSURE mode=%s hot_MiB=%d step_MiB=%d epochs=%d\n", mode, HOT_MIB, STEP_MIB, EPOCHS);
	if (pthread_barrier_init(&phase, NULL, 3) ||
	    pthread_create(&heat_thread, NULL, churn, NULL) ||
	    pthread_create(&reader, NULL, timed_refault, NULL) ||
	    pthread_create(&watchdog, NULL, request_watchdog, NULL))
		fail("pressure threads");
	pressure_counters(0);
	for (int epoch = 0; epoch < EPOCHS; epoch++) {
		int slot = epoch % RING;
		const char *initial = alg;
		check_swapped(&ring[slot]);
		struct usage u = usage();
		if (u.raw > peak_raw)
			peak_raw = u.raw;
		if (u.samefill > peak_samefill)
			peak_samefill = u.samefill;
		sync_phase();
		for (int n = 0; n < REQUESTS; n++) {
			const char *to = dynamic ? codecs[!strcmp(alg, "lz4")] : alg;
			req[n] = timed_select(alg, to);
			alg = to;
			usleep(250);
		}
		sync_phase();
		uint64_t begin = heat_start[epoch] > read_start[epoch] ? heat_start[epoch] : read_start[epoch];
		uint64_t end = heat_end[epoch] < read_end[epoch] ? heat_end[epoch] : read_end[epoch];
		overlap_epochs += begin < end;
		for (int n = 0; n < REQUESTS; n++)
			overlap_requests += req[n].end >= begin && req[n].end <= end;
		scan_epochs += scan_delta[epoch] > 0;
		/* Discard loads that intersect an in-flight selector write.
		 * All remaining first touches have an unambiguous active codec. */
		for (int n = 0; n < PAGES; n++) {
			int k = epoch * PAGES + n;
			const char *active = initial;
			for (int w = 0; w < REQUESTS; w++) {
				if (req[w].end < access_at[k])
					active = req[w].to;
				else if (req[w].start <= access_at[k] + delay_ns[k]) {
					active = NULL;
					break;
				} else
					break;
			}
			if (active && strcmp(active, generation[slot])) {
				cross_generation_pages++;
				if (!strcmp(generation[slot], "lz4"))
					cross_lz4++;
				else
					cross_zstd++;
			}
		}
		syscheck(munmap(ring[slot].data, SIZE), "unmap timed batch");
		if (epoch + RING < EPOCHS) {
			ring[slot] = prepare(RING + epoch);
			pageout(&ring[slot]);
			generation[slot] = alg;
		}
	}
	pthread_join(heat_thread, NULL);
	pthread_join(reader, NULL);
	pressure_counters(1);
	size_t samples = EPOCHS * PAGES;
	qsort(delay_ns, samples, sizeof(*delay_ns), order_ns);
	printf("RESULT mode=%s requests=%u lz4_to_zstd=%u zstd_to_lz4=%u max_request_ns=%" PRIu64
	       " lz4_to_zstd_max_ns=%" PRIu64 " zstd_to_lz4_max_ns=%" PRIu64
	       " lz4_to_zstd_timeouts=%u zstd_to_lz4_timeouts=%u timeouts=%u samples=%zu p99_ns=%" PRIu64 " verified_bytes=%zu overlap_epochs=%u overlap_requests=%u"
	       " kswapd_scan_epochs=%u verified_under_other_codec=%u source_lz4=%u source_zstd=%u peak_raw=%ld peak_samefill=%ld\n",
	       mode, writes, directions[0], directions[1], request_max, direction_max[0], direction_max[1],
	       direction_timeouts[0], direction_timeouts[1], timeouts, samples,
	       delay_ns[(samples * 99 + 99) / 100 - 1], samples * PAGE, overlap_epochs, overlap_requests,
	       scan_epochs, cross_generation_pages, cross_lz4, cross_zstd, peak_raw, peak_samefill);
	for (size_t i = 0; i < samples; ) {
		size_t next = i + 1;
		while (next < samples && delay_ns[next] == delay_ns[i])
			next++;
		printf("FAULT_HIST ns=%" PRIu64 " count=%zu\n", delay_ns[i], next - i);
		i = next;
	}
	if (!overlap_epochs || !overlap_requests || !scan_epochs || timeouts || (dynamic && (!cross_lz4 || !cross_zstd)))
		fail("pressure overlap/timeout");
	if (!strcmp(mode, "writeback"))
		printf("PACKED_PROBE packed_payloads=%ld read_io=%ld\n",
		       counter(MEMCOMP "packed_swap_events", "writeback_packed_payloads"),
		       counter(MEMCOMP "events", "swapped_read_io"));
	puts("PRESSURE PASS");
}
