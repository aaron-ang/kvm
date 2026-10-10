// SPDX-License-Identifier: GPL-2.0
/*
 * Measure how shadow MMU eviction and host page aging (page_idle) interact
 * through the hardware Accessed bits of leaf SPTEs.
 *
 * Same trace as lru_reuse_test (MODE_HOT), but each 2MiB region only uses
 * K pages so the host can track every used page.  The measured phase is
 * split into intervals.  At each interval boundary the vCPU is stopped and
 * the host
 *   1. reads the page_idle bit of every used page and compares it with the
 *      pages the guest touched during the interval (aging only), then
 *   2. marks every used page idle (aging only), which clears the Accessed
 *      bits of the leaf SPTEs through KVM's clear_young notifier.
 * The guest then reloads CR3 (flushing its TLB, so cleared Accessed bits are
 * set again on the next walk), clears its touched map and runs the next
 * interval.  With one vCPU and all host work done at boundaries, a run is
 * deterministic for a given seed and policy.
 *
 * Run with kvm.tdp_mmu=0, npt/ept=0, transparent_hugepage=never.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall_common.h"

#define TEST_SLOT	1
#define TEST_GPA	0x100000000ull
#define TEST_GVA	0xc0000000ull
#define REGION_SIZE	(2ull << 20)
#define REGION_PAGES	(REGION_SIZE / PAGE_SIZE)
#define MAX_USED	(1 << 16)

#define PAGEMAP_PRESENT		(1ULL << 63)
#define PAGEMAP_PFN_MASK	((1ULL << 55) - 1)

static uint64_t nr_regions = 256;
static uint64_t hot_regions = 16;
static uint64_t hot_pct = 90;
static uint64_t pages_per_region = 8;
static uint64_t nr_warmup = 50000;
static uint64_t nr_intervals = 40;
static uint64_t interval_len = 5000;
static uint64_t seed = 1;
static bool aging;

/* Guest-written, host-read: 1 if the used page was touched this interval. */
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

static uint64_t lookup_pfn(int pagemap_fd, struct kvm_vm *vm, uint64_t gva)
{
	uint64_t hva = (uint64_t)addr_gva2hva(vm, gva);
	uint64_t entry = pread_u64(pagemap_fd, 8 * (hva / getpagesize()));

	TEST_ASSERT(entry & PAGEMAP_PRESENT, "gva 0x%lx not present", gva);
	TEST_ASSERT(entry & PAGEMAP_PFN_MASK, "PFNs need CAP_SYS_ADMIN");
	return entry & PAGEMAP_PFN_MASK;
}

static bool page_idle_get(int fd, uint64_t pfn)
{
	return (pread_u64(fd, 8 * (pfn / 64)) >> (pfn % 64)) & 1;
}

static void page_idle_set(int fd, uint64_t pfn)
{
	uint64_t bits = 1ULL << (pfn % 64);

	TEST_ASSERT(pwrite(fd, &bits, 8, 8 * (pfn / 64)) == 8, "mark pfn 0x%lx idle", pfn);
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

/* Counts over intervals 2..T (interval 1 starts right after warmup). */
struct idle_counts {
	uint64_t touched_idle[2], touched_busy[2];	/* [0] cold, [1] hot */
	uint64_t untouched_idle, untouched_busy;
	uint64_t untracked;				/* not idle right after marking */
	uint64_t pfn_changes;				/* page migrated between intervals */
};

static uint64_t vmstat_sum(void)
{
	static const char * const keys[] = { "pgmigrate_success", "pgscan_kswapd", "pgscan_direct",
					     "pgsteal_kswapd", "pgsteal_direct", "compact_migrate_scanned" };
	char name[64];
	uint64_t v, sum = 0;
	FILE *f = fopen("/proc/vmstat", "r");
	int i;

	if (!f)
		return 0;
	while (fscanf(f, "%63s %lu", name, &v) == 2)
		for (i = 0; i < ARRAY_SIZE(keys); i++)
			if (!strcmp(name, keys[i]))
				sum += v;
	fclose(f);
	return sum;
}

static void help(char *name)
{
	puts("");
	printf("usage: %s [-a] [-r regions] [-H hot_regions] [-p hot_pct] [-k pages_per_region]\n"
	       "          [-w warmup] [-t intervals] [-l interval_len] [-s seed]\n"
	       "  -a: age pages with page_idle at every interval boundary\n", name);
	puts("");
	exit(0);
}

int main(int argc, char *argv[])
{
	uint64_t before[NR_STATS], after[NR_STATS];
	struct idle_counts c = {};
	int idle_fd = -1, pagemap_fd = -1;
	uint64_t vm0 = 0;
	uint64_t *pfns, nr_used, i, b;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	int opt;

	while ((opt = getopt(argc, argv, "har:H:p:k:w:t:l:s:")) != -1) {
		switch (opt) {
		case 'a': aging = true; break;
		case 'r': nr_regions = atoi_positive("regions", optarg); break;
		case 'H': hot_regions = atoi_positive("hot regions", optarg); break;
		case 'p': hot_pct = atoi_non_negative("hot percent", optarg); break;
		case 'k': pages_per_region = atoi_positive("pages per region", optarg); break;
		case 'w': nr_warmup = atoi_non_negative("warmup", optarg); break;
		case 't': nr_intervals = atoi_positive("intervals", optarg); break;
		case 'l': interval_len = atoi_positive("interval length", optarg); break;
		case 's': seed = atoi_positive("seed", optarg); break;
		default: help(argv[0]);
		}
	}
	nr_used = nr_regions * pages_per_region;
	TEST_ASSERT(hot_regions < nr_regions && hot_pct <= 100, "bad hot set");
	TEST_ASSERT(REGION_PAGES % pages_per_region == 0 && nr_used <= MAX_USED, "bad pages per region");

	if (aging) {
		idle_fd = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR);
		TEST_ASSERT(idle_fd >= 0, "open page_idle");
		pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
		TEST_ASSERT(pagemap_fd >= 0, "open pagemap");
	}
	pfns = calloc(nr_used, sizeof(*pfns));

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
				read_stats(vm, vcpu, before);
				vm0 = vmstat_sum();
			}
			if (b == nr_intervals)
				read_stats(vm, vcpu, after);
			if (!aging)
				continue;

			/* Score the interval that just ended (skip the first). */
			if (b >= 2) {
				sync_global_from_guest(vm, touched);
				for (i = 0; i < nr_used; i++) {
					bool idle = page_idle_get(idle_fd, pfns[i]);
					bool hot = i / pages_per_region < hot_regions;

					if (touched[i] && idle)
						c.touched_idle[hot]++;
					else if (touched[i])
						c.touched_busy[hot]++;
					else if (idle)
						c.untouched_idle++;
					else
						c.untouched_busy++;
				}
			}
			if (b == nr_intervals)
				continue;

			/* Start the next interval with every used page idle. */
			for (i = 0; i < nr_used; i++) {
				uint64_t pfn = lookup_pfn(pagemap_fd, vm, used_gva(i));

				if (b >= 1 && pfn != pfns[i])
					c.pfn_changes++;
				pfns[i] = pfn;
				page_idle_set(idle_fd, pfns[i]);
			}
			for (i = 0; i < nr_used; i++)
				if (b >= 1 && !page_idle_get(idle_fd, pfns[i]))
					c.untracked++;
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

	printf("RESULT aging=%d regions=%lu hot=%lu hot_pct=%lu k=%lu w=%lu t=%lu l=%lu seed=%lu",
	       aging, nr_regions, hot_regions, hot_pct, pages_per_region, nr_warmup,
	       nr_intervals, interval_len, seed);
	print_param("lru_mmu");
	print_param("lru_age");
	print_param("min_alloc_pages");
	for (i = 0; i < ARRAY_SIZE(vm_stats); i++)
		printf(" %s=%lu", vm_stats[i], after[i] - before[i]);
	for (i = 0; i < ARRAY_SIZE(vcpu_stats); i++)
		printf(" %s=%lu", vcpu_stats[i],
		       after[ARRAY_SIZE(vm_stats) + i] - before[ARRAY_SIZE(vm_stats) + i]);
	if (aging)
		printf(" touched_idle_hot=%lu touched_busy_hot=%lu touched_idle_cold=%lu"
		       " touched_busy_cold=%lu untouched_idle=%lu untouched_busy=%lu untracked=%lu"
		       " pfn_changes=%lu",
		       c.touched_idle[1], c.touched_busy[1], c.touched_idle[0],
		       c.touched_busy[0], c.untouched_idle, c.untouched_busy, c.untracked,
		       c.pfn_changes);
	printf(" vmstat_reclaim_migrate=%lu", vmstat_sum() - vm0);
	printf("\n");

	kvm_vm_free(vm);
	return 0;
}
