# Redis Benchmark: FIFO vs LRU Page Replacement Policies

This document compares the performance of Redis under two different page replacement algorithms in our KVM environment:
1. FIFO (First-In-First-Out) - The default policy
2. Approximate LRU (Least Recently Used) - Our implementation

## Benchmark Configuration
- 1 million requests (`-n 1000000`)
- 100,000 distinct keys (`-r 100000`) 
- Pipeline of 16 commands (`-P 16`)
- Quiet output format (`-q`)

## Benchmark Results

Raw redis-benchmark output and per-run eviction counters are in [raw_results.txt](raw_results.txt).

## Performance Analysis

### Key Observations

| Operation   | FIFO Performance | LRU Performance | % Difference |
| ----------- | ---------------- | --------------- | ------------ |
| PING_INLINE | 488,520 req/s    | 313,676 req/s   | -35.8%       |
| PING_MBULK  | 343,879 req/s    | 470,588 req/s   | +36.8%       |
| GET         | 244,858 req/s    | 362,188 req/s   | +47.9%       |
| SET         | 286,451 req/s    | 330,469 req/s   | +15.4%       |
| ZADD        | 158,228 req/s    | 144,697 req/s   | -8.6%        |
| LRANGE_300  | 18,265 req/s     | 16,548 req/s    | -9.4%        |
| LRANGE_600  | 9,560 req/s      | 9,668 req/s     | +1.1%        |

### How to read these numbers

These are single runs of each policy, and they don't separate the eviction policy from run-to-run noise:

* **PING moved as much as GET.** PING never touches the eviction code, yet PING_INLINE was 35.8% slower under LRU and PING_MBULK 36.8% faster. A command the policy can't affect swinging by ±36% means the noise is at least that large, so the +47.9% on GET and +15.4% on SET are within it.
* **No repeats or variance.** Each policy ran once, so there is no way to tell a real difference from noise.
* **EPT was on.** The legacy MMU was managing EPT tables rather than shadows of guest page tables, so this measured eviction of direct-map pages, not the shadow paging the project targets.
* **CLOCK without hardware accessed bits barely differs from FIFO.** The eviction counters below show why: with `lru_ref` alone, a hot page's bit is set only when it's created, so after one lap the sweep evicts in FIFO order.

The Redis numbers are kept as a record of what we ran. They don't show that either policy is faster.

## Eviction Counters with Reuse

`tools/testing/selftests/kvm/x86/lru_reuse_test.c` replays a seeded trace with a hot set (16 of 256 regions of 2 MiB, 90% of accesses) under true shadow paging (`kvm_amd.npt=0`, `kvm.tdp_mmu=0`) in QEMU TCG. Being emulated, it gives KVM's counters, not timings. Page faults (`pf_taken`), mean of 3 seeds, 200,000 measured accesses after 100,000 warm-up accesses:

| Workload | Pool pages | FIFO    | CLOCK, marked on creation | CLOCK, marked on fill too | + leaf A bits (`lru_age=1`) | + parent A bits (`lru_age=2`) |
| -------- | ---------- | ------- | ------------------------- | ------------------------- | --------------------------- | ----------------------------- |
| hot      | 32         | 212,854 | 212,854 (0%)              | 212,854 (0%)              | 204,270 (−4%)               | 202,913 (−5%)                 |
| hot      | 64         | 175,223 | 156,379 (−11%)            | 68,112 (−61%)             | 78,218 (−55%)               | 31,104 (−82%)                 |
| hot      | 128        | 119,108 | 122,539 (+3%)             | 42,421 (−64%)             | 32,795 (−72%)               | 40,520 (−66%)                 |
| scan     | 32         | 213,098 | 213,098 (0%)              | 213,098 (0%)              | 204,550 (−4%)               | 203,455 (−5%)                 |
| scan     | 64         | 179,150 | 161,438 (−10%)            | 36,200 (−80%)             | 31,957 (−82%)               | 36,431 (−80%)                 |
| scan     | 128        | 143,191 | 110,865 (−23%)            | 31,371 (−78%)             | 28,686 (−80%)               | 24,027 (−83%)                 |

* "Marked on creation": `lru_ref` is set only when a shadow page is looked up or created. "Marked on fill too": `mmu_set_spte()` also sets it on every non-prefetch SPTE fill. The two A-bit columns add `lru_age` on top of marking on fill: leaf mode checks the page's own SPTEs (up to 512), parent mode checks the parent SPTE(s) through `sp->parent_ptes`.
* FIFO and "marked on creation" come from the kernel before marking on fill; the other columns from the current kernel. They are comparable: CLOCK at 32 pages on the current kernel reproduces the earlier FIFO counts seed for seed.
* Earlier `lru_age` numbers in this section were replaced. They came from a run where `lru_reuse_test.c` overflowed its stats arrays (`before[5]`/`after[5]` holding 10 stats).
* Marking on fill does most of the work: 61–80% fewer faults and 11–30% fewer zaps (`mmu_shadow_zapped`) than FIFO at 64 and 128 pages. At 32 pages it matches FIFO; both A-bit modes zap 23–24% fewer pages there.
* A bits add mixed gains on top. Parent mode is best in 4 of 6 rows and never much worse. Leaf mode is best at hot/128 but worse than plain CLOCK at hot/64.
* Seed noise: plain CLOCK hot/64 ranged 65,270–71,759; parent hot/128 36,234–48,134; leaf scan/128 23,853–38,077. Differences of a few points between CLOCK variants are within noise; the gap to FIFO is not.
* Cost (new VM stats): `lru_age_sptes` per zapped page is 1,145–1,352 for leaf and 1.2–2.3 for parent. `lru_age_flushes` (TLB flush when a sweep clears A bits but zaps nothing) is ~255–288 per run at 32 pages and ~0 at 64/128.
* With the default pool (`min_alloc_pages=0`, upstream sizing), the test zaps nothing: 34,728 faults under both FIFO and CLOCK + parent.
* A workload that cycles through every region in order (earlier kernel, one seed) gained at most 7% from any policy, and `lru_age` was about 1% worse at 32 pages.

## Conclusions

* The Redis runs above are too noisy to rank the policies, and they came from a build where `lru_ref` was never set. `redis_repeat.sh` re-runs them with EPT off, repeats, interleaving and pinning.
* CLOCK needs a signal for accesses that don't fault. Setting `lru_ref` on every SPTE fill provides most of it and cuts faults sharply on workloads with reuse; with the bit set only on creation, CLOCK behaves close to FIFO.
* The hardware Accessed bits add mixed gains on top. Parent mode (`lru_age=2`) is best in 4 of 6 configurations, never much worse, reads ~2 SPTEs per zap, and doesn't touch the leaf A bits that host reclaim uses (`kvm_age_gfn`/`kvm_test_age_gfn`), so it is the recommended setting. Leaf mode (`lru_age=1`) reads ~1,300 SPTEs per zap, is mixed (worse than plain CLOCK at hot/64), and shares bits with host reclaim.
* Reclaim check: `access_tracking_perf_test -v 4 -b 64M` (`min_alloc_pages=64`) reported "0 of 16384 pages still idle" on every vCPU under FIFO, CLOCK, leaf, and parent, so no pages were falsely idle. The test isn't designed to catch interference from aging.
* The default stays `lru_age=0` because all measurements are emulated (TCG); there are no native timings. Open questions: timings on real hardware and the cost of scanning SPTEs under `mmu_lock`.
