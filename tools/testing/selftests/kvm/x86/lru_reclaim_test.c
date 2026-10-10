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
 * Run with kvm.tdp_mmu=0, npt/ept=0, transparent_hugepage=never.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/swap.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall_common.h"

#define TEST_SLOT	1
#define TEST_GPA	0x100000000ull
#define TEST_GVA	0xc0000000ull
#define REGION_SIZE	(2ull << 20)
#define REGION_PAGES	(REGION_SIZE / PAGE_SIZE)
#define MAX_USED	(1 << 18)

#define PAGEMAP_PRESENT		(1ULL << 63)
#define PAGEMAP_SWAPPED		(1ULL << 62)

static uint64_t nr_regions = 1024;
static uint64_t hot_regions = 16;
static uint64_t hot_pct = 90;
static uint64_t pages_per_region = 8;
static uint64_t nr_warmup = 50000;
static uint64_t nr_intervals = 20;
static uint64_t interval_len = 5000;
static uint64_t seed = 1;
static uint64_t avail_mib;		/* 0: no hog, no reclaim (control) */
static uint64_t squeeze_mib;		/* swapped out during the intervals */
static const char *swap_dev = "/dev/vda";

static uint8_t touched[MAX_USED];

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
	uint64_t page = (idx % pages_per_region) * (REGION_PAGES / pages_per_region);

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
			touch((r >> 8) % hot_regions, (r >> 32) % pages_per_region);
		else
			touch(hot_regions + (r >> 8) % cold, (r >> 32) % pages_per_region);
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
	hdr[256] = 1;					/* version */
	hdr[257] = size / getpagesize() - 1;		/* last_page */
	memcpy((char *)hdr + getpagesize() - 10, "SWAPSPACE2", 10);
	TEST_ASSERT(pwrite(fd, hdr, getpagesize(), 0) == getpagesize(), "write swap header");
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
		if (!strncmp(name, key, strlen(key)) && name[strlen(key)] == ':')
			r = v;
	fclose(f);
	return r;
}

/* Lock enough memory that only avail_mib stays available. */
static void hog(void)
{
	uint64_t target = avail_mib << 20, avail, len, total = 0;
	void *p;

	TEST_ASSERT(meminfo_kib("MemAvailable") * 1024 > target, "MemAvailable <= target");
	/*
	 * Lock half the remaining gap at a time and re-read MemAvailable, since
	 * page tables and dropped caches make one big mlock overshoot.
	 */
	while ((avail = meminfo_kib("MemAvailable") * 1024) > target + (1 << 20)) {
		len = (avail - target) / 2 & ~(uint64_t)(getpagesize() - 1);
		if (len < (1 << 20))
			len = 1 << 20;
		p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
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
		p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
		TEST_ASSERT(p != MAP_FAILED, "mmap squeeze");
		TEST_ASSERT(!mlock(p, 1 << 20), "mlock squeeze");
	}
}

static const char * const vmstat_keys[] = { "pswpin", "pswpout", "pgmajfault", "pgmigrate_success" };
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

static const char * const vm_stats[] = {
	"mmu_cache_miss", "mmu_shadow_zapped", "mmu_recycled",
	"lru_hand_steps", "lru_ref_skips", "lru_age_sptes", "lru_age_skips",
	"lru_age_flushes",
};
static const char * const vcpu_stats[] = { "pf_taken", "pf_fixed" };
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

	for (i = 0; i < ARRAY_SIZE(vm_stats); i++)
		out[j++] = vm_get_stat(vm, vm_stats[i]);
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
	uint64_t out[2];		/* used pages swapped out at a boundary: [0] cold, [1] hot */
	uint64_t touched_out[2];	/* ... that the guest touched in the interval just ended */
	uint64_t checked[2];		/* page-boundary pairs checked */
};

static void help(char *name)
{
	puts("");
	printf("usage: %s [-r regions] [-H hot_regions] [-p hot_pct] [-k pages_per_region]\n"
	       "          [-w warmup] [-t intervals] [-l interval_len] [-s seed]\n"
	       "          [-M avail_mib] [-S squeeze_mib] [-d swap_dev]\n"
	       "  -M: after warmup, swap on and lock memory so only avail_mib MiB stays\n"
	       "      available (0 = no pressure, control)\n"
	       "  -S: with -M, lock memory during the intervals until squeeze_mib MiB in\n"
	       "      total has been swapped out (reclaim of resident guest pages)\n", name);
	puts("");
	exit(0);
}

int main(int argc, char *argv[])
{
	uint64_t before[NR_STATS], after[NR_STATS], vs0[NR_VMSTAT], vs1[NR_VMSTAT];
	struct swap_counts c = {};
	uint64_t nr_used, i, b;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	int opt, pagemap_fd;

	while ((opt = getopt(argc, argv, "hr:H:p:k:w:t:l:s:M:S:d:")) != -1) {
		switch (opt) {
		case 'r': nr_regions = atoi_positive("regions", optarg); break;
		case 'H': hot_regions = atoi_positive("hot regions", optarg); break;
		case 'p': hot_pct = atoi_non_negative("hot percent", optarg); break;
		case 'k': pages_per_region = atoi_positive("pages per region", optarg); break;
		case 'w': nr_warmup = atoi_non_negative("warmup", optarg); break;
		case 't': nr_intervals = atoi_positive("intervals", optarg); break;
		case 'l': interval_len = atoi_positive("interval length", optarg); break;
		case 's': seed = atoi_positive("seed", optarg); break;
		case 'M': avail_mib = atoi_non_negative("available MiB", optarg); break;
		case 'S': squeeze_mib = atoi_non_negative("squeeze MiB", optarg); break;
		case 'd': swap_dev = optarg; break;
		default: help(argv[0]);
		}
	}
	nr_used = nr_regions * pages_per_region;
	TEST_ASSERT(hot_regions < nr_regions && hot_pct <= 100, "bad hot set");
	TEST_ASSERT(REGION_PAGES % pages_per_region == 0 && nr_used <= MAX_USED, "bad pages per region");

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	TEST_ASSERT(pagemap_fd >= 0, "open pagemap");

	vm = __vm_create_with_one_vcpu(&vcpu, nr_regions * REGION_PAGES, guest_code);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, TEST_GPA, TEST_SLOT,
				    nr_regions * REGION_PAGES, 0);
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
				if (avail_mib) {
					swap_on(swap_dev);
					hog();
				}
				read_stats(vm, vcpu, before);
				read_vmstat(vs0);
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
				uint64_t e = pagemap_entry(pagemap_fd, vm, used_gva(i));
				bool hot = i / pages_per_region < hot_regions;
				bool out = !(e & PAGEMAP_PRESENT);

				c.checked[hot]++;
				if (out) {
					c.out[hot]++;
					if (touched[i])
						c.touched_out[hot]++;
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
		TEST_ASSERT(after[i] >= before[i], "stat %lu went down: %lu -> %lu",
			    i, before[i], after[i]);

	printf("RESULT squeeze_mib=%lu avail_mib=%lu regions=%lu hot=%lu hot_pct=%lu k=%lu w=%lu t=%lu l=%lu seed=%lu",
	       squeeze_mib, avail_mib, nr_regions, hot_regions, hot_pct, pages_per_region, nr_warmup,
	       nr_intervals, interval_len, seed);
	print_param("lru_mmu");
	print_param("lru_age");
	print_param("min_alloc_pages");
	for (i = 0; i < ARRAY_SIZE(vm_stats); i++)
		printf(" %s=%lu", vm_stats[i], after[i] - before[i]);
	for (i = 0; i < ARRAY_SIZE(vcpu_stats); i++)
		printf(" %s=%lu", vcpu_stats[i],
		       after[ARRAY_SIZE(vm_stats) + i] - before[ARRAY_SIZE(vm_stats) + i]);
	for (i = 0; i < NR_VMSTAT; i++)
		printf(" %s=%lu", vmstat_keys[i], vs1[i] - vs0[i]);
	printf(" out_hot=%lu out_cold=%lu touched_out_hot=%lu touched_out_cold=%lu"
	       " checked_hot=%lu checked_cold=%lu\n",
	       c.out[1], c.out[0], c.touched_out[1], c.touched_out[0], c.checked[1], c.checked[0]);

	kvm_vm_free(vm);
	return 0;
}
