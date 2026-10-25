// SPDX-License-Identifier: GPL-2.0
/*
 * Stress shadow MMU eviction against every other path that zaps or ages
 * shadow pages, and check that guest translations stay correct.
 *
 * Several vCPUs read and write a hot/cold mix of pages.  Every page in the
 * "checked" area holds a tag derived from its own address, so a vCPU that
 * reads a page through a stale or wrong translation sees the wrong tag and
 * fails a guest assert.  While the vCPUs run, host threads
 *   - delete, re-add and move an extra memslot (zaps all shadow pages),
 *   - toggle dirty logging on the checked memslot and read the dirty log,
 *   - madvise(MADV_DONTNEED) random pages of a separate "scratch" area
 *     (MMU notifier invalidations),
 *   - mark checked pages idle through page_idle (clear_young notifier),
 *   - resize the shadow page pool with KVM_SET_NR_MMU_PAGES.
 * The plan can also flip kvm.lru_mmu and kvm.lru_age while the test runs.
 * Kernel debugging (KASAN, lockdep, list debugging) catches memory and
 * locking errors; the test itself catches wrong translations and hangs.
 *
 * Run with kvm.tdp_mmu=0, npt/ept=0, transparent_hugepage=never.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall_common.h"

#define CHECK_SLOT	1
#define SCRATCH_SLOT	2
#define EXTRA_SLOT	10	/* outside the library's slots */
#define CHECK_GPA	0x100000000ull
#define SCRATCH_GPA	0x200000000ull
#define EXTRA_GPA	0x300000000ull
#define EXTRA_GPA2	0x380000000ull
#define CHECK_GVA	0xc0000000ull
#define SCRATCH_GVA	0x180000000ull
#define REGION_SIZE	(2ull << 20)
#define REGION_PAGES	(REGION_SIZE / PAGE_SIZE)
#define EXTRA_PAGES	512
#define MAX_VCPUS	8
#define TAG_MAGIC	0x5a5a1234deadbeefull

#define PAGEMAP_PRESENT		(1ULL << 63)
#define PAGEMAP_PFN_MASK	((1ULL << 55) - 1)

static uint64_t nr_regions = 256;	/* per area */
static uint64_t hot_regions = 16;
static uint64_t hot_pct = 90;
static uint64_t pages_per_region = 8;
static uint64_t sync_every = 2000;
static uint64_t seed = 1;
static int nr_vcpus = 4;
static int duration = 300;
static int window = 30;		/* each vCPU must report progress within this many seconds */

static volatile bool stop;
static uint64_t vcpu_iters[MAX_VCPUS];
static uint64_t ops[5];
static const char * const op_names[] = { "slot_ops", "dirty_ops", "madvise_ops", "idle_ops", "pool_ops" };

static uint64_t next_rand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return *s = x;
}

static uint64_t page_off(uint64_t idx)
{
	uint64_t region = idx / pages_per_region;
	uint64_t page = (idx % pages_per_region) * (REGION_PAGES / pages_per_region);

	return region * REGION_SIZE + page * PAGE_SIZE;
}

static uint64_t tag(uint64_t gva)
{
	return gva ^ TAG_MAGIC;
}

static uint64_t pick(uint64_t r)
{
	uint64_t cold = nr_regions - hot_regions;
	uint64_t region = r % 100 < hot_pct ? (r >> 8) % hot_regions :
					      hot_regions + (r >> 8) % cold;

	return region * pages_per_region + (r >> 32) % pages_per_region;
}

static void guest_code(uint64_t id)
{
	uint64_t s = seed * 1000003ull + id + 1, i, gva, r;

	for (i = 0;; i++) {
		r = next_rand(&s);
		gva = CHECK_GVA + page_off(pick(r));
		GUEST_ASSERT_EQ(*(volatile uint64_t *)gva, tag(gva));
		*(volatile uint64_t *)gva = tag(gva);
		*(volatile uint64_t *)(gva + 8) = i;

		r = next_rand(&s);
		*(volatile uint8_t *)(SCRATCH_GVA + page_off(pick(r)) + 16) = 1;

		if (i % 256 == 0)
			asm volatile("mov %0, %%cr3" : : "r"(get_cr3()) : "memory");
		if (i % sync_every == 0)
			GUEST_SYNC(i);
	}
}

struct vcpu_info {
	struct kvm_vcpu *vcpu;
	int id;
};

static void *vcpu_thread(void *arg)
{
	struct vcpu_info *info = arg;
	struct ucall uc;

	while (!READ_ONCE(stop)) {
		vcpu_run(info->vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(info->vcpu, KVM_EXIT_IO);
		switch (get_ucall(info->vcpu, &uc)) {
		case UCALL_SYNC:
			WRITE_ONCE(vcpu_iters[info->id], uc.args[1]);
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		default:
			TEST_FAIL("unexpected ucall");
		}
	}
	return NULL;
}

/*
 * Raw ioctls on a private buffer: the selftest library's memslot bookkeeping
 * is not safe against the vCPU threads looking up regions concurrently.
 */
static void *slot_thread(void *arg)
{
	struct kvm_vm *vm = arg;
	uint64_t size = EXTRA_PAGES * PAGE_SIZE;
	void *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct kvm_userspace_memory_region r = {
		.slot = EXTRA_SLOT, .memory_size = size, .userspace_addr = (uint64_t)buf,
	};

	TEST_ASSERT(buf != MAP_FAILED, "mmap extra slot");
	while (!READ_ONCE(stop)) {
		r.guest_phys_addr = EXTRA_GPA;
		r.memory_size = size;
		vm_ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r);	/* add */
		usleep(10000);
		r.guest_phys_addr = EXTRA_GPA2;
		vm_ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r);	/* move */
		usleep(10000);
		r.memory_size = 0;
		vm_ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r);	/* delete */
		ops[0]++;
	}
	return NULL;
}

static void *dirty_thread(void *arg)
{
	struct kvm_vm *vm = arg;
	uint64_t npages = nr_regions * REGION_PAGES;
	unsigned long *bitmap = calloc(npages / 64 + 1, sizeof(unsigned long));

	while (!READ_ONCE(stop)) {
		vm_mem_region_set_flags(vm, CHECK_SLOT, KVM_MEM_LOG_DIRTY_PAGES);
		usleep(5000);
		kvm_vm_get_dirty_log(vm, CHECK_SLOT, bitmap);
		vm_mem_region_set_flags(vm, CHECK_SLOT, 0);
		ops[1]++;
		usleep(15000);
	}
	free(bitmap);
	return NULL;
}

static void *madvise_thread(void *arg)
{
	struct kvm_vm *vm = arg;
	uint64_t s = seed ^ 0xabcdef, r;
	char *base = addr_gpa2hva(vm, SCRATCH_GPA);

	while (!READ_ONCE(stop)) {
		r = next_rand(&s);
		TEST_ASSERT(!madvise(base + page_off(pick(r)), PAGE_SIZE, MADV_DONTNEED), "madvise");
		ops[2]++;
		if (ops[2] % 64 == 0)
			usleep(1000);
	}
	return NULL;
}

static void *idle_thread(void *arg)
{
	struct kvm_vm *vm = arg;
	int idle_fd = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR);
	int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	uint64_t s = seed ^ 0x123457, hva, entry, pfn, bits;

	if (idle_fd < 0 || pagemap_fd < 0)
		return NULL;
	while (!READ_ONCE(stop)) {
		hva = (uint64_t)addr_gpa2hva(vm, CHECK_GPA + page_off(pick(next_rand(&s))));
		if (pread(pagemap_fd, &entry, 8, 8 * (hva / getpagesize())) != 8 ||
		    !(entry & PAGEMAP_PRESENT))
			continue;
		pfn = entry & PAGEMAP_PFN_MASK;
		bits = 1ULL << (pfn % 64);
		if (pwrite(idle_fd, &bits, 8, 8 * (pfn / 64)) == 8)
			ops[3]++;
		if (ops[3] % 64 == 0)
			usleep(1000);
	}
	close(idle_fd);
	close(pagemap_fd);
	return NULL;
}

static void *pool_thread(void *arg)
{
	static const unsigned long sizes[] = { 64, 512, 96, 256, 128, 1024 };
	struct kvm_vm *vm = arg;
	int i = 0;

	while (!READ_ONCE(stop)) {
		if (!__vm_ioctl(vm, KVM_SET_NR_MMU_PAGES, (void *)sizes[i++ % ARRAY_SIZE(sizes)]))
			ops[4]++;
		usleep(10000);
	}
	return NULL;
}

static void help(char *name)
{
	puts("");
	printf("usage: %s [-v vcpus] [-d seconds] [-r regions] [-H hot_regions] [-p hot_pct]\n"
	       "          [-k pages_per_region] [-s seed] [-w progress_window_seconds]\n", name);
	puts("");
	exit(0);
}

int main(int argc, char *argv[])
{
	void *(*host_fns[])(void *) = { slot_thread, dirty_thread, madvise_thread, idle_thread, pool_thread };
	pthread_t vcpu_threads[MAX_VCPUS], host_threads[ARRAY_SIZE(host_fns)];
	struct vcpu_info info[MAX_VCPUS];
	struct kvm_vcpu *vcpus[MAX_VCPUS];
	uint64_t area_pages, nr_used, i, before[MAX_VCPUS];
	struct kvm_vm *vm;
	int opt;

	while ((opt = getopt(argc, argv, "hv:d:r:H:p:k:s:w:")) != -1) {
		switch (opt) {
		case 'v': nr_vcpus = atoi_positive("vcpus", optarg); break;
		case 'd': duration = atoi_positive("seconds", optarg); break;
		case 'r': nr_regions = atoi_positive("regions", optarg); break;
		case 'H': hot_regions = atoi_positive("hot regions", optarg); break;
		case 'p': hot_pct = atoi_non_negative("hot percent", optarg); break;
		case 'k': pages_per_region = atoi_positive("pages per region", optarg); break;
		case 's': seed = atoi_positive("seed", optarg); break;
		case 'w': window = atoi_positive("window", optarg); break;
		default: help(argv[0]);
		}
	}
	TEST_ASSERT(nr_vcpus <= MAX_VCPUS, "too many vCPUs");
	TEST_ASSERT(hot_regions < nr_regions && hot_pct <= 100, "bad hot set");
	TEST_ASSERT(REGION_PAGES % pages_per_region == 0, "bad pages per region");
	area_pages = nr_regions * REGION_PAGES;
	nr_used = nr_regions * pages_per_region;

	vm = __vm_create_with_vcpus(VM_SHAPE_DEFAULT, nr_vcpus, 2 * area_pages, guest_code, vcpus);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, CHECK_GPA, CHECK_SLOT, area_pages, 0);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, SCRATCH_GPA, SCRATCH_SLOT, area_pages, 0);
	virt_map(vm, CHECK_GVA, CHECK_GPA, area_pages);
	virt_map(vm, SCRATCH_GVA, SCRATCH_GPA, area_pages);

	for (i = 0; i < nr_used; i++)
		*(uint64_t *)addr_gpa2hva(vm, CHECK_GPA + page_off(i)) = tag(CHECK_GVA + page_off(i));

	sync_global_to_guest(vm, nr_regions);
	sync_global_to_guest(vm, hot_regions);
	sync_global_to_guest(vm, hot_pct);
	sync_global_to_guest(vm, pages_per_region);
	sync_global_to_guest(vm, sync_every);
	sync_global_to_guest(vm, seed);

	for (i = 0; i < nr_vcpus; i++) {
		info[i] = (struct vcpu_info){ .vcpu = vcpus[i], .id = i };
		vcpu_args_set(vcpus[i], 1, i);
		pthread_create(&vcpu_threads[i], NULL, vcpu_thread, &info[i]);
	}
	for (i = 0; i < ARRAY_SIZE(host_fns); i++)
		pthread_create(&host_threads[i], NULL, host_fns[i], vm);

	/* Every vCPU must keep making progress: no hang, no livelock. */
	for (int t = 0; t < duration; t += window) {
		for (i = 0; i < nr_vcpus; i++)
			before[i] = READ_ONCE(vcpu_iters[i]);
		sleep(window);
		printf("t=%d iters", t + window);
		for (i = 0; i < nr_vcpus; i++)
			printf(" %lu", READ_ONCE(vcpu_iters[i]));
		printf(" ops %lu %lu %lu %lu %lu\n", ops[0], ops[1], ops[2], ops[3], ops[4]);
		for (i = 0; i < nr_vcpus; i++)
			TEST_ASSERT(READ_ONCE(vcpu_iters[i]) > before[i], "vCPU %lu made no progress in %d s", i, window);
	}

	WRITE_ONCE(stop, true);
	for (i = 0; i < ARRAY_SIZE(host_fns); i++)
		pthread_join(host_threads[i], NULL);
	/* vCPUs leave at their next sync. */
	for (i = 0; i < nr_vcpus; i++)
		pthread_join(vcpu_threads[i], NULL);

	printf("RESULT vcpus=%d duration=%d regions=%lu hot=%lu hot_pct=%lu k=%lu seed=%lu",
	       nr_vcpus, duration, nr_regions, hot_regions, hot_pct, pages_per_region, seed);
	for (i = 0; i < nr_vcpus; i++)
		printf(" iters%lu=%lu", i, vcpu_iters[i]);
	for (i = 0; i < ARRAY_SIZE(ops); i++)
		printf(" %s=%lu", op_names[i], ops[i]);
	printf(" mmu_recycled=%lu mmu_shadow_zapped=%lu lru_hand_steps=%lu lru_age_skips=%lu lru_fifo_fallbacks=%lu\n",
	       vm_get_stat(vm, "mmu_recycled"), vm_get_stat(vm, "mmu_shadow_zapped"),
	       vm_get_stat(vm, "lru_hand_steps"), vm_get_stat(vm, "lru_age_skips"),
	       vm_get_stat(vm, "lru_fifo_fallbacks"));

	kvm_vm_free(vm);
	return 0;
}
