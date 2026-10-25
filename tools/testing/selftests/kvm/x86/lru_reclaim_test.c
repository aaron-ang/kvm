// SPDX-License-Identifier: GPL-2.0
/*
 * Measure whether shadow MMU eviction policies change which guest pages host
 * reclaim swaps out.
 *
 * Same trace as lru_idle_test, but instead of page_idle the host is put
 * under real memory pressure: after warmup the test formats a swap device,
 * turns it on and locks a "hog" allocation that leaves only -M MiB of
 * available memory, so reclaim keeps swapping guest pages out while the guest
 * runs.  Reclaim decides what to swap from the Accessed bits it reads (and
 * clears) through KVM's clear_young notifier, the same leaf SPTE bits that
 * leaf aging clears.
 *
 * At every interval boundary the host reads /proc/self/pagemap for every
 * used page and counts the pages that are swapped out, split hot/cold.  A
 * hot page that is out at a boundary was picked by reclaim even though the
 * guest uses it all the time.
 *
 * With -Z the test also traces kvmmmu:kvm_mmu_prepare_zap_page for the guest
 * page tables of the hot regions, so each hot region's last-level shadow
 * page, and kvmmmu:kvm_mmu_set_spte for the hot pages.  It then splits the hot
 * counts two ways: by whether the region's shadow page was zapped during the
 * interval just ended, and by whether the page's SPTE was present at the
 * boundary (installed after the last zap of its shadow page), the state host
 * reclaim sees.  SPTEs removed by reclaim itself are not traced; a hot page
 * refaults, and so gets a new SPTE, long before the next boundary.
 *
 * Run with kvm.tdp_mmu=0, npt/ept=0, transparent_hugepage=never.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/swap.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall_common.h"

#define TEST_SLOT 1
#define TEST_GPA 0x100000000ull
#define TEST_GVA 0xc0000000ull
#define REGION_SIZE (2ull << 20)
#define REGION_PAGES (REGION_SIZE / PAGE_SIZE)
#define MAX_USED (1 << 18)

#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_SWAPPED (1ULL << 62)

static uint64_t nr_regions = 1024;
static uint64_t hot_regions = 16;
static uint64_t hot_pct = 90;
static uint64_t pages_per_region = 8;
static uint64_t nr_warmup = 50000;
static uint64_t nr_intervals = 20;
static uint64_t interval_len = 5000;
static uint64_t seed = 1;
static uint64_t avail_mib; /* 0: no hog, no reclaim (control) */
static uint64_t squeeze_mib; /* swapped out during the intervals */
static const char *swap_dev = "/dev/vda";
static bool trace_zaps;

static uint8_t touched[MAX_USED];

static char trace_inst[128]; /* the test's own tracefs instance */
static int trace_fd = -1;
static uint64_t *pt_gfn; /* gfn of each hot region's guest page table */
static uint64_t *region_zaps; /* zaps of its shadow page this interval */
static uint64_t hot_zaps, hot_installs;
static uint64_t trace_seq = 2; /* event order; 1 = before tracing */
static uint64_t *zap_seq; /* per hot region: last zap of its shadow page */
static uint64_t *install_seq; /* per hot used page: last SPTE install */

static uint64_t next_rand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return *s = x;
}

static uint64_t used_gva(uint64_t idx)
{
	uint64_t region = idx / pages_per_region;
	uint64_t page =
		(idx % pages_per_region) * (REGION_PAGES / pages_per_region);

	return TEST_GVA + region * REGION_SIZE + page * PAGE_SIZE;
}

static void touch(uint64_t region, uint64_t slot)
{
	uint64_t idx = region * pages_per_region + slot;

	*(volatile uint8_t *)used_gva(idx) = 1;
	touched[idx] = 1;
}

static void do_accesses(uint64_t n, uint64_t *s)
{
	uint64_t cold = nr_regions - hot_regions;
	uint64_t i, r;

	for (i = 0; i < n; i++) {
		r = next_rand(s);
		if (r % 100 < hot_pct)
			touch((r >> 8) % hot_regions,
			      (r >> 32) % pages_per_region);
		else
			touch(hot_regions + (r >> 8) % cold,
			      (r >> 32) % pages_per_region);
	}
}

static void guest_code(void)
{
	uint64_t nr_used = nr_regions * pages_per_region;
	uint64_t s = seed, i;

	for (i = 0; i < nr_used; i++)
		*(volatile uint8_t *)used_gva(i) = 1;
	do_accesses(nr_warmup, &s);

	for (i = 0; i <= nr_intervals; i++) {
		GUEST_SYNC(i);
		asm volatile("mov %0, %%cr3" : : "r"(get_cr3()) : "memory");
		memset(touched, 0, nr_used);
		if (i < nr_intervals)
			do_accesses(interval_len, &s);
	}
	GUEST_DONE();
}

static uint64_t pread_u64(int fd, uint64_t off)
{
	uint64_t v;

	TEST_ASSERT(pread(fd, &v, 8, off) == 8, "pread at 0x%lx", off);
	return v;
}

static uint64_t pagemap_entry(int fd, struct kvm_vm *vm, uint64_t gva)
{
	uint64_t hva = (uint64_t)addr_gva2hva(vm, gva);

	return pread_u64(fd, 8 * (hva / getpagesize()));
}

/* Write a version 1 swap header (what mkswap does) and turn the device on. */
static void swap_on(const char *dev)
{
	uint32_t *hdr = calloc(1, getpagesize());
	int fd = open(dev, O_RDWR);
	off_t size;

	TEST_ASSERT(fd >= 0 && hdr, "open %s", dev);
	size = lseek(fd, 0, SEEK_END);
	TEST_ASSERT(size > 16 * getpagesize(), "swap device too small");
	hdr[256] = 1; /* version */
	hdr[257] = size / getpagesize() - 1; /* last_page */
	memcpy((char *)hdr + getpagesize() - 10, "SWAPSPACE2", 10);
	TEST_ASSERT(pwrite(fd, hdr, getpagesize(), 0) == getpagesize(),
		    "write swap header");
	fsync(fd);
	close(fd);
	free(hdr);
	TEST_ASSERT(!swapon(dev, 0), "swapon %s", dev);
}

static uint64_t meminfo_kib(const char *key)
{
	char name[64];
	uint64_t v, r = 0;
	FILE *f = fopen("/proc/meminfo", "r");

	TEST_ASSERT(f, "open meminfo");
	while (fscanf(f, "%63s %lu kB", name, &v) == 2)
		if (!strncmp(name, key, strlen(key)) &&
		    name[strlen(key)] == ':')
			r = v;
	fclose(f);
	return r;
}

/* Lock enough memory that only avail_mib stays available. */
static void hog(void)
{
	uint64_t target = avail_mib << 20, avail, len, total = 0;
	void *p;

	TEST_ASSERT(meminfo_kib("MemAvailable") * 1024 > target,
		    "MemAvailable <= target");
	/*
	 * Lock half the remaining gap at a time and re-read MemAvailable, since
	 * page tables and dropped caches make one big mlock overshoot.
	 */
	while ((avail = meminfo_kib("MemAvailable") * 1024) >
	       target + (1 << 20)) {
		len = (avail - target) / 2 & ~(uint64_t)(getpagesize() - 1);
		if (len < (1 << 20))
			len = 1 << 20;
		p = mmap(NULL, len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
		TEST_ASSERT(p != MAP_FAILED, "mmap hog %lu MiB", len >> 20);
		TEST_ASSERT(!mlock(p, len), "mlock hog %lu MiB", len >> 20);
		total += len;
	}
	printf("hog %lu MiB locked, MemAvailable now %lu MiB\n", total >> 20,
	       meminfo_kib("MemAvailable") >> 10);
}

static uint64_t vmstat_val(const char *key)
{
	char name[64];
	uint64_t v, r = 0;
	FILE *f = fopen("/proc/vmstat", "r");

	TEST_ASSERT(f, "open vmstat");
	while (fscanf(f, "%63s %lu", name, &v) == 2)
		if (!strcmp(name, key))
			r = v;
	fclose(f);
	return r;
}

/*
 * Lock memory 1 MiB at a time until another @mib MiB has been swapped out.
 * MemAvailable leaves out the watermark reserves, so a fixed amount can
 * be absorbed without any reclaim.
 */
static void squeeze(uint64_t mib)
{
	uint64_t base = vmstat_val("pswpout"), want = mib << (20 - 12);
	void *p;

	while (vmstat_val("pswpout") - base < want) {
		p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
		TEST_ASSERT(p != MAP_FAILED, "mmap squeeze");
		TEST_ASSERT(!mlock(p, 1 << 20), "mlock squeeze");
	}
}

static void tracefs_write(const char *file, const char *val)
{
	char path[256];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", trace_inst, file);
	fd = open(path, O_WRONLY | O_TRUNC);
	TEST_ASSERT(fd >= 0, "open %s", path);
	TEST_ASSERT(write(fd, val, strlen(val)) == strlen(val),
		    "write '%s' to %s", val, path);
	close(fd);
}

/*
 * Removing the instance disables its events and frees its buffer.  This runs
 * from atexit(), so it must not assert.
 */
static void trace_stop(void)
{
	if (trace_fd >= 0)
		close(trace_fd);
	if (rmdir(trace_inst))
		fprintf(stderr, "rmdir %s: %s\n", trace_inst, strerror(errno));
}

/*
 * For shadow paging, a last-level shadow page's gfn is the gfn of the guest
 * page table it shadows, so filtering on the hot regions' page-table gfns
 * keeps only the zaps of their shadow pages.
 *
 * The events go to a private tracefs instance with its own buffer, enables
 * and filters: other tracing on the host cannot fill its buffer, and nothing
 * stays enabled after the test exits.
 */
static void trace_start(struct kvm_vm *vm)
{
	const char *root = "/sys/kernel/tracing";
	uint64_t lo = UINT64_MAX, hi = 0, r;
	char path[256], filter[128];

	if (access("/sys/kernel/tracing/instances", F_OK)) {
		if (!access("/sys/kernel/debug/tracing/instances", F_OK))
			root = "/sys/kernel/debug/tracing";
		else
			TEST_ASSERT(!mount("tracefs", root, "tracefs", 0, NULL),
				    "mount tracefs");
	}
	snprintf(trace_inst, sizeof(trace_inst), "%s/instances/lru_reclaim_test",
		 root);
	rmdir(trace_inst); /* left behind by a killed run */
	TEST_ASSERT(!mkdir(trace_inst, 0755), "mkdir %s", trace_inst);
	atexit(trace_stop);

	pt_gfn = calloc(hot_regions, sizeof(*pt_gfn));
	region_zaps = calloc(hot_regions, sizeof(*region_zaps));
	zap_seq = calloc(hot_regions, sizeof(*zap_seq));
	install_seq =
		calloc(hot_regions * pages_per_region, sizeof(*install_seq));
	TEST_ASSERT(pt_gfn && region_zaps && zap_seq && install_seq, "calloc");
	/* Warmup touched every used page: count them as installed before tracing. */
	for (r = 0; r < hot_regions * pages_per_region; r++)
		install_seq[r] = 1;
	for (r = 0; r < hot_regions; r++) {
		uint64_t *pte =
			vm_get_page_table_entry(vm, TEST_GVA + r * REGION_SIZE);

		pt_gfn[r] = addr_hva2gpa(vm, pte) >> vm->page_shift;
		lo = pt_gfn[r] < lo ? pt_gfn[r] : lo;
		hi = pt_gfn[r] > hi ? pt_gfn[r] : hi;
	}

	tracefs_write("buffer_size_kb", "4096");
	snprintf(filter, sizeof(filter), "gfn >= %lu && gfn <= %lu", lo, hi);
	tracefs_write("events/kvmmmu/kvm_mmu_prepare_zap_page/filter", filter);
	tracefs_write("events/kvmmmu/kvm_mmu_prepare_zap_page/enable", "1");
	snprintf(filter, sizeof(filter), "gfn >= %llu && gfn < %llu",
		 TEST_GPA / PAGE_SIZE,
		 TEST_GPA / PAGE_SIZE + hot_regions * REGION_PAGES);
	tracefs_write("events/kvmmmu/kvm_mmu_set_spte/filter", filter);
	tracefs_write("events/kvmmmu/kvm_mmu_set_spte/enable", "1");

	snprintf(path, sizeof(path), "%s/trace_pipe", trace_inst);
	trace_fd = open(path, O_RDONLY | O_NONBLOCK);
	TEST_ASSERT(trace_fd >= 0, "open %s", path);
	printf("tracing zaps of hot page tables, gfn 0x%lx..0x%lx\n", lo, hi);
}

static void count_event(const char *line)
{
	static const char zap_ev[] = "kvm_mmu_prepare_zap_page:";
	static const char zap_gfn[] = " gfn ";
	static const char spte_ev[] = "kvm_mmu_set_spte: gfn ";
	uint64_t gfn, r, off, step = REGION_PAGES / pages_per_region;
	const char *p;

	if ((p = strstr(line, zap_ev)) && (p = strstr(p, zap_gfn))) {
		gfn = strtoull(p + sizeof(zap_gfn) - 1, NULL, 16);
		for (r = 0; r < hot_regions; r++) {
			if (pt_gfn[r] == gfn) {
				region_zaps[r]++;
				zap_seq[r] = trace_seq++;
				hot_zaps++;
				return;
			}
		}
	} else if ((p = strstr(line, spte_ev))) {
		off = strtoull(p + sizeof(spte_ev) - 1, NULL, 16) -
		      TEST_GPA / PAGE_SIZE;
		if (off >= hot_regions * REGION_PAGES ||
		    off % REGION_PAGES % step)
			return;
		install_seq[off / REGION_PAGES * pages_per_region +
			    off % REGION_PAGES / step] = trace_seq++;
		hot_installs++;
	}
}

static bool spte_present(uint64_t idx)
{
	return install_seq[idx] > zap_seq[idx / pages_per_region];
}

/* Consume the events logged since the last call, while the vCPU is stopped. */
static void trace_drain(void)
{
	static char line[1024];
	static size_t len;
	char buf[8192];
	ssize_t n, i;

	for (;;) {
		n = read(trace_fd, buf, sizeof(buf));
		/* 0: tracing was turned off and the buffer is empty. */
		if (!n || (n < 0 && errno == EAGAIN))
			return;
		TEST_ASSERT(n > 0, "read trace_pipe");
		for (i = 0; i < n; i++) {
			if (buf[i] != '\n') {
				if (len < sizeof(line) - 1)
					line[len++] = buf[i];
				continue;
			}
			line[len] = 0;
			count_event(line);
			len = 0;
		}
	}
}

/*
 * Events the ring buffer lost: overwritten ("overrun") or, without the
 * overwrite option, refused ("dropped events").  Nonzero means the zap counts
 * are short.
 */
static uint64_t trace_overruns(void)
{
	char path[256], line[128];
	uint64_t v, total = 0;
	FILE *f;
	int cpu;

	for (cpu = 0;; cpu++) {
		snprintf(path, sizeof(path), "%s/per_cpu/cpu%d/stats",
			 trace_inst, cpu);
		f = fopen(path, "r");
		if (!f)
			break;
		while (fgets(line, sizeof(line), f))
			if (sscanf(line, "overrun: %lu", &v) == 1 ||
			    sscanf(line, "dropped events: %lu", &v) == 1)
				total += v;
		fclose(f);
	}
	return total;
}

static const char *const vmstat_keys[] = { "pswpin", "pswpout", "pgmajfault",
					   "pgmigrate_success", "pgactivate" };
#define NR_VMSTAT ARRAY_SIZE(vmstat_keys)

static void read_vmstat(uint64_t *out)
{
	char name[64];
	uint64_t v;
	FILE *f = fopen("/proc/vmstat", "r");
	int i;

	TEST_ASSERT(f, "open vmstat");
	memset(out, 0, NR_VMSTAT * sizeof(*out));
	while (fscanf(f, "%63s %lu", name, &v) == 2)
		for (i = 0; i < NR_VMSTAT; i++)
			if (!strcmp(name, vmstat_keys[i]))
				out[i] = v;
	fclose(f);
}

/* lru_zap_marks exists only with the lru_zap_accessed validation knob; absent reads 0. */
static const char *const vm_stats[] = {
	"mmu_cache_miss", "mmu_shadow_zapped", "mmu_recycled",
	"lru_hand_steps", "lru_ref_skips",     "lru_age_sptes",
	"lru_age_skips",  "lru_age_flushes",   "lru_fifo_fallbacks",
	"lru_zap_marks",
};
static const char *const vcpu_stats[] = { "pf_taken", "pf_fixed" };
#define NR_STATS (ARRAY_SIZE(vm_stats) + ARRAY_SIZE(vcpu_stats))

static uint64_t vcpu_get_stat(int fd, struct kvm_stats_header *header,
			      struct kvm_stats_desc *descs, const char *name)
{
	struct kvm_stats_desc *desc;
	uint64_t data = 0;
	int i;

	for (i = 0; i < header->num_desc; i++) {
		desc = get_stats_descriptor(descs, i, header);
		if (!strcmp(desc->name, name)) {
			read_stat_data(fd, header, desc, &data, 1);
			break;
		}
	}
	return data;
}

static void read_stats(struct kvm_vm *vm, struct kvm_vcpu *vcpu, uint64_t *out)
{
	static struct kvm_stats_header header;
	static struct kvm_stats_desc *descs;
	static int fd = -1;
	int i, j = 0;

	if (fd < 0) {
		fd = vcpu_get_stats_fd(vcpu);
		read_stats_header(fd, &header);
		descs = read_stats_descriptors(fd, &header);
	}

	for (i = 0; i < ARRAY_SIZE(vm_stats); i++) {
		uint64_t v = 0;

		__vm_get_stat(vm, vm_stats[i], &v, 1);
		out[j++] = v;
	}
	for (i = 0; i < ARRAY_SIZE(vcpu_stats); i++)
		out[j++] = vcpu_get_stat(fd, &header, descs, vcpu_stats[i]);
}

static void print_param(const char *name)
{
	char path[128], buf[32] = "?";
	FILE *f;

	snprintf(path, sizeof(path), "/sys/module/kvm/parameters/%s", name);
	f = fopen(path, "r");
	if (f) {
		if (fgets(buf, sizeof(buf), f))
			buf[strcspn(buf, "\n")] = 0;
		fclose(f);
	}
	printf(" %s=%s", name, buf);
}

/* Counts over boundaries 2..T. */
struct swap_counts {
	uint64_t out[2]; /* used pages swapped out at a boundary: [0] cold, [1] hot */
	/* ... that the guest touched in the interval just ended */
	uint64_t touched_out[2];
	uint64_t checked[2]; /* page-boundary pairs checked */
	/* -Z, hot pages only: [0] region not zapped this interval, [1] zapped */
	uint64_t hot_out_z[2];
	uint64_t hot_checked_z[2];
	/* -Z, hot pages only: [0] SPTE absent at the boundary, [1] present */
	uint64_t hot_out_p[2];
	uint64_t hot_checked_p[2];
};

static void help(char *name)
{
	puts("");
	printf("usage: %s [-r regions] [-H hot_regions] [-p hot_pct] [-k pages_per_region]\n"
	       "          [-w warmup] [-t intervals] [-l interval_len] [-s seed]\n"
	       "          [-M avail_mib] [-S squeeze_mib] [-d swap_dev] [-Z]\n"
	       "  -M: after warmup, swap on and lock memory so only avail_mib MiB stays\n"
	       "      available (0 = no pressure, control)\n"
	       "  -S: with -M, lock memory during the intervals until squeeze_mib MiB in\n"
	       "      total has been swapped out (reclaim of resident guest pages)\n"
	       "  -Z: trace zaps of the hot regions' shadow pages and SPTE installs of\n"
	       "      the hot pages; split the hot counts by whether the region's shadow\n"
	       "      page was zapped and by whether the page's SPTE was present\n",
	       name);
	puts("");
	exit(0);
}

int main(int argc, char *argv[])
{
	uint64_t before[NR_STATS], after[NR_STATS], vs0[NR_VMSTAT],
		vs1[NR_VMSTAT];
	struct swap_counts c = {};
	uint64_t nr_used, i, b;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	int opt, pagemap_fd;

	while ((opt = getopt(argc, argv, "hr:H:p:k:w:t:l:s:M:S:d:Z")) != -1) {
		switch (opt) {
		case 'r':
			nr_regions = atoi_positive("regions", optarg);
			break;
		case 'H':
			hot_regions = atoi_positive("hot regions", optarg);
			break;
		case 'p':
			hot_pct = atoi_non_negative("hot percent", optarg);
			break;
		case 'k':
			pages_per_region =
				atoi_positive("pages per region", optarg);
			break;
		case 'w':
			nr_warmup = atoi_non_negative("warmup", optarg);
			break;
		case 't':
			nr_intervals = atoi_positive("intervals", optarg);
			break;
		case 'l':
			interval_len = atoi_positive("interval length", optarg);
			break;
		case 's':
			seed = atoi_positive("seed", optarg);
			break;
		case 'M':
			avail_mib = atoi_non_negative("available MiB", optarg);
			break;
		case 'S':
			squeeze_mib = atoi_non_negative("squeeze MiB", optarg);
			break;
		case 'd':
			swap_dev = optarg;
			break;
		case 'Z':
			trace_zaps = true;
			break;
		default:
			help(argv[0]);
		}
	}
	nr_used = nr_regions * pages_per_region;
	TEST_ASSERT(hot_regions < nr_regions && hot_pct <= 100, "bad hot set");
	TEST_ASSERT(REGION_PAGES % pages_per_region == 0 && nr_used <= MAX_USED,
		    "bad pages per region");

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	TEST_ASSERT(pagemap_fd >= 0, "open pagemap");

	vm = __vm_create_with_one_vcpu(&vcpu, nr_regions * REGION_PAGES,
				       guest_code);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, TEST_GPA,
				    TEST_SLOT, nr_regions * REGION_PAGES, 0);
	virt_map(vm, TEST_GVA, TEST_GPA, nr_regions * REGION_PAGES);

	sync_global_to_guest(vm, nr_regions);
	sync_global_to_guest(vm, hot_regions);
	sync_global_to_guest(vm, hot_pct);
	sync_global_to_guest(vm, pages_per_region);
	sync_global_to_guest(vm, nr_warmup);
	sync_global_to_guest(vm, nr_intervals);
	sync_global_to_guest(vm, interval_len);
	sync_global_to_guest(vm, seed);

	for (;;) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			b = uc.args[1];
			if (b == 0) {
				/* Before hog(), so its target includes the trace buffer. */
				if (trace_zaps)
					trace_start(vm);
				if (avail_mib) {
					swap_on(swap_dev);
					hog();
				}
				read_stats(vm, vcpu, before);
				read_vmstat(vs0);
			}
			/* Zaps in the interval just ended, before this boundary's reclaim. */
			if (trace_zaps && b > 0) {
				memset(region_zaps, 0,
				       hot_regions * sizeof(*region_zaps));
				trace_drain();
			}
			if (avail_mib && b < nr_intervals)
				squeeze(squeeze_mib / nr_intervals);
			if (b == nr_intervals) {
				read_stats(vm, vcpu, after);
				read_vmstat(vs1);
			}
			if (b < 2)
				continue;

			/* Which used pages are swapped out right now? */
			sync_global_from_guest(vm, touched);
			for (i = 0; i < nr_used; i++) {
				uint64_t e = pagemap_entry(pagemap_fd, vm,
							   used_gva(i));
				bool hot = i / pages_per_region < hot_regions;
				bool out = !(e & PAGEMAP_PRESENT);

				c.checked[hot]++;
				if (out) {
					c.out[hot]++;
					if (touched[i])
						c.touched_out[hot]++;
				}
				if (trace_zaps && hot) {
					bool z = region_zaps[i / pages_per_region];
					bool pr = spte_present(i);

					c.hot_checked_z[z]++;
					c.hot_out_z[z] += out;
					c.hot_checked_p[pr]++;
					c.hot_out_p[pr] += out;
				}
			}
			continue;
		case UCALL_DONE:
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		default:
			TEST_FAIL("unexpected ucall");
		}
		break;
	}

	for (i = 0; i < NR_STATS; i++)
		TEST_ASSERT(after[i] >= before[i],
			    "stat %lu went down: %lu -> %lu", i, before[i],
			    after[i]);

	printf("RESULT squeeze_mib=%lu avail_mib=%lu regions=%lu hot=%lu hot_pct=%lu k=%lu w=%lu t=%lu l=%lu seed=%lu",
	       squeeze_mib, avail_mib, nr_regions, hot_regions, hot_pct,
	       pages_per_region, nr_warmup, nr_intervals, interval_len, seed);
	print_param("lru_mmu");
	print_param("lru_age");
	print_param("lru_zap_accessed");
	print_param("min_alloc_pages");
	for (i = 0; i < ARRAY_SIZE(vm_stats); i++)
		printf(" %s=%lu", vm_stats[i], after[i] - before[i]);
	for (i = 0; i < ARRAY_SIZE(vcpu_stats); i++)
		printf(" %s=%lu", vcpu_stats[i],
		       after[ARRAY_SIZE(vm_stats) + i] -
			       before[ARRAY_SIZE(vm_stats) + i]);
	for (i = 0; i < NR_VMSTAT; i++)
		printf(" %s=%lu", vmstat_keys[i], vs1[i] - vs0[i]);
	printf(" out_hot=%lu out_cold=%lu touched_out_hot=%lu touched_out_cold=%lu"
	       " checked_hot=%lu checked_cold=%lu",
	       c.out[1], c.out[0], c.touched_out[1], c.touched_out[0],
	       c.checked[1], c.checked[0]);
	if (trace_zaps)
		printf(" hot_zaps=%lu hot_installs=%lu out_hot_z=%lu checked_hot_z=%lu out_hot_nz=%lu"
		       " checked_hot_nz=%lu out_hot_present=%lu checked_hot_present=%lu"
		       " out_hot_absent=%lu checked_hot_absent=%lu trace_overruns=%lu",
		       hot_zaps, hot_installs, c.hot_out_z[1],
		       c.hot_checked_z[1], c.hot_out_z[0], c.hot_checked_z[0],
		       c.hot_out_p[1], c.hot_checked_p[1], c.hot_out_p[0],
		       c.hot_checked_p[0], trace_overruns());
	printf("\n");

	kvm_vm_free(vm);
	return 0;
}
