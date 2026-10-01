// SPDX-License-Identifier: GPL-2.0
/*
 * Count shadow MMU page recycling for access patterns with reuse.
 *
 * The guest touches one byte per access in a region of 2MiB "regions"
 * (one last-level shadow page each when EPT/NPT is off).  A small set of
 * regions is hot; the rest is cold.  The access sequence depends only on
 * the seed, so runs with different eviction policies see the same trace.
 *
 * The host prints KVM's VM/vCPU stats over the measured phase.  Set the
 * policy with the kvm.lru_mmu / kvm.lru_age / kvm.min_alloc_pages module
 * parameters before running; run with kvm.tdp_mmu=0 and npt/ept=0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

enum { MODE_HOT, MODE_SCAN, MODE_LOOP };

static uint64_t nr_regions = 256;
static uint64_t hot_regions = 16;
static uint64_t hot_pct = 90;
static uint64_t nr_warmup = 50000;
static uint64_t nr_measure = 200000;
static uint64_t mode = MODE_HOT;
static uint64_t seed = 1;

static uint64_t next_rand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return *s = x;
}

static void touch(uint64_t region, uint64_t page)
{
	*(volatile uint8_t *)(TEST_GVA + region * REGION_SIZE + page * PAGE_SIZE) = 1;
}

/*
 * MODE_HOT:  hot_pct% of accesses go to a random page of a random hot
 *            region, the rest to a random page of a random cold region.
 * MODE_SCAN: like MODE_HOT, but cold accesses walk the cold regions in
 *            order (a hot set plus a background scan).
 * MODE_LOOP: cycle through every region in order (no reuse inside the
 *            pool; the classic worst case for LRU).
 */
static void do_accesses(uint64_t n, uint64_t *s, uint64_t *scan_pos)
{
	uint64_t cold = nr_regions - hot_regions;
	uint64_t i, r;

	for (i = 0; i < n; i++) {
		r = next_rand(s);
		if (mode == MODE_LOOP) {
			touch(*scan_pos % nr_regions, (*scan_pos / nr_regions) % REGION_PAGES);
			(*scan_pos)++;
		} else if (r % 100 < hot_pct) {
			touch((r >> 8) % hot_regions, (r >> 32) % REGION_PAGES);
		} else if (mode == MODE_SCAN) {
			touch(hot_regions + *scan_pos % cold, (r >> 32) % REGION_PAGES);
			(*scan_pos)++;
		} else {
			touch(hot_regions + (r >> 8) % cold, (r >> 32) % REGION_PAGES);
		}
	}
}

static void guest_code(void)
{
	uint64_t s = seed, scan_pos = 0;

	do_accesses(nr_warmup, &s, &scan_pos);
	GUEST_SYNC(1);
	do_accesses(nr_measure, &s, &scan_pos);
	GUEST_DONE();
}

static const char * const vm_stats[] = {
	"mmu_cache_miss", "mmu_shadow_zapped", "mmu_recycled",
	"lru_hand_steps", "lru_ref_skips", "lru_age_sptes", "lru_age_skips",
	"lru_age_flushes",
};
static const char * const vcpu_stats[] = { "pf_taken", "pf_fixed" };

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

static void help(char *name)
{
	puts("");
	printf("usage: %s [-m hot|scan|loop] [-r regions] [-H hot_regions]\n"
	       "          [-p hot_pct] [-w warmup] [-n accesses] [-s seed]\n", name);
	puts("");
	exit(0);
}

int main(int argc, char *argv[])
{
	uint64_t before[ARRAY_SIZE(vm_stats) + ARRAY_SIZE(vcpu_stats)];
	uint64_t after[ARRAY_SIZE(vm_stats) + ARRAY_SIZE(vcpu_stats)];
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	int opt, i;

	while ((opt = getopt(argc, argv, "hm:r:H:p:w:n:s:")) != -1) {
		switch (opt) {
		case 'm':
			if (!strcmp(optarg, "hot"))
				mode = MODE_HOT;
			else if (!strcmp(optarg, "scan"))
				mode = MODE_SCAN;
			else if (!strcmp(optarg, "loop"))
				mode = MODE_LOOP;
			else
				help(argv[0]);
			break;
		case 'r': nr_regions = atoi_positive("regions", optarg); break;
		case 'H': hot_regions = atoi_positive("hot regions", optarg); break;
		case 'p': hot_pct = atoi_non_negative("hot percent", optarg); break;
		case 'w': nr_warmup = atoi_non_negative("warmup", optarg); break;
		case 'n': nr_measure = atoi_positive("accesses", optarg); break;
		case 's': seed = atoi_positive("seed", optarg); break;
		default: help(argv[0]);
		}
	}
	TEST_ASSERT(hot_regions < nr_regions && hot_pct <= 100, "bad hot set");

	/* Passing the test region's size reserves slot 0 space for its page tables. */
	vm = __vm_create_with_one_vcpu(&vcpu, nr_regions * REGION_PAGES, guest_code);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, TEST_GPA, TEST_SLOT,
				    nr_regions * REGION_PAGES, 0);
	virt_map(vm, TEST_GVA, TEST_GPA, nr_regions * REGION_PAGES);

	sync_global_to_guest(vm, nr_regions);
	sync_global_to_guest(vm, hot_regions);
	sync_global_to_guest(vm, hot_pct);
	sync_global_to_guest(vm, nr_warmup);
	sync_global_to_guest(vm, nr_measure);
	sync_global_to_guest(vm, mode);
	sync_global_to_guest(vm, seed);

	for (;;) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			read_stats(vm, vcpu, before);
			continue;
		case UCALL_DONE:
			read_stats(vm, vcpu, after);
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		default:
			TEST_FAIL("unexpected ucall");
		}
		break;
	}

	/* KVM stats only go up; a drop means the before/after reads are wrong. */
	for (i = 0; i < ARRAY_SIZE(before); i++)
		TEST_ASSERT(after[i] >= before[i], "stat %d went down: %lu -> %lu",
			    i, before[i], after[i]);

	printf("RESULT mode=%s regions=%lu hot=%lu hot_pct=%lu n=%lu seed=%lu",
	       mode == MODE_HOT ? "hot" : mode == MODE_SCAN ? "scan" : "loop",
	       nr_regions, hot_regions, hot_pct, nr_measure, seed);
	print_param("lru_mmu");
	print_param("lru_age");
	print_param("min_alloc_pages");
	for (i = 0; i < ARRAY_SIZE(vm_stats); i++)
		printf(" %s=%lu", vm_stats[i], after[i] - before[i]);
	for (i = 0; i < ARRAY_SIZE(vcpu_stats); i++)
		printf(" %s=%lu", vcpu_stats[i],
		       after[ARRAY_SIZE(vm_stats) + i] - before[ARRAY_SIZE(vm_stats) + i]);
	printf("\n");

	kvm_vm_free(vm);
	return 0;
}
