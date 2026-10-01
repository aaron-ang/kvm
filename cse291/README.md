# CSE 291: CLOCK eviction for the KVM x86 shadow MMU

This fork (based on upstream `abab683b97`, Linux 6.14-rc7) adds an
approximate-LRU (CLOCK / second-chance) policy for recycling shadow MMU
pages in the legacy (non-TDP-MMU) x86 KVM MMU. Upstream recycles pages
FIFO from the tail of `kvm->arch.active_mmu_pages`.

## What changed

- `arch/x86/kvm/mmu/mmu.c`
  - `kvm_mmu_zap_oldest_mmu_pages()` calls `kvm_mmu_clock_zap_pages()` when
    `lru_mmu=1`. A hand (`kvm->arch.clock_hand`) walks `active_mmu_pages`
    from oldest to newest and wraps. A page with `sp->lru_ref` set gets
    its bit cleared and is skipped. Unreferenced non-root pages are zapped.
    The sweep stops after two full passes.
  - `kvm_mmu_clock_hand_unlink()`, called from
    `__kvm_mmu_prepare_zap_page()`, moves the hand forward when the page
    it points at leaves the list. Every zap path goes through that
    function, so the hand never dangles.
  - `mark_kvm_page_accessed()` sets `lru_ref` when a shadow page is looked
    up or created (`__kvm_mmu_get_shadow_page()`), when
    `fast_page_fault()` fixes a fault (or finds it spurious), and when
    `mmu_set_spte()` fills an SPTE outside prefetch ("marking on fill").
  - With `lru_age=1` or `2`, the sweep also skips a page whose hardware
    Accessed bits are set, clearing them: leaf mode checks the page's own
    SPTEs (up to 512), parent mode checks the parent SPTE(s) via
    `sp->parent_ptes`. A sweep that clears bits but zaps nothing flushes
    the TLB so the bits can be set again.
  - The list is never reordered, so the FIFO assumption in
    `kvm_zap_obsolete_pages()` still holds.
- `arch/x86/kvm/mmu/mmu_internal.h`: `sp->lru_ref`, `mark_kvm_page_accessed()`.
- `arch/x86/include/asm/kvm_host.h`: `kvm->arch.clock_hand`.
- `arch/x86/kvm/x86.c`: with `min_alloc_pages=0` (default) the MMU page
  limit is sized as upstream. A nonzero value pins every VM's pool to that
  many pages, to force eviction in experiments, and becomes the minimum
  for `KVM_SET_NR_MMU_PAGES`. Adds the `lru_*` VM stats below.

## Module parameters

| Parameter | Default | Perm | Meaning |
|---|---|---|---|
| `kvm.lru_mmu` | 0 | 0644 | 1 = CLOCK eviction, 0 = upstream FIFO |
| `kvm.lru_age` | 0 | 0644 | with CLOCK, also use hardware Accessed bits: 1 = leaf, 2 = parent (recommended) |
| `kvm.min_alloc_pages` | 0 | 0644 | 0 = upstream sizing; N = pin each VM's pool to N pages |
| `kvm.tdp_mmu` | 1 | 0444 | set 0 on the command line to use the legacy MMU |
| `kvm_intel.ept` | 1 | 0444 | set 0 on the command line for real guest-page-table shadowing |

`tdp_mmu` and `ept` are read-only, as upstream: KVM latches them at module
load, so set them on the kernel command line. `run_bench.sh` boots L1 with
`kvm_intel.ept=0 kvm.tdp_mmu=0 kvm.min_alloc_pages=32`. The policy can be
switched at runtime (inside L1):

```sh
echo 1 | sudo tee /sys/module/kvm/parameters/lru_mmu
echo 2 | sudo tee /sys/module/kvm/parameters/lru_age
```

New VM stats (`/sys/kernel/debug/kvm/*/` or the binary stats fd):
`lru_hand_steps`, `lru_ref_skips`, `lru_age_sptes`, `lru_age_skips`,
`lru_age_flushes`. All only increase.

## Scripts

- `sudo cse291/build.sh`: installs build dependencies with apt, builds a
  config from `/boot/config-$(uname -r)` plus `defconfig`, turns on KVM,
  KVM_INTEL, KVM_PROVE_MMU and the other options the tests need, and
  builds `arch/x86_64/boot/bzImage`.
- `sudo cse291/run_bench.sh [-k KERNEL] [-a APPEND] [-c CPUS]`: installs
  dependencies, fetches an Ubuntu cloud image, builds the kselftests and
  perf, starts virtiofsd, and boots the kernel in QEMU as an L1 guest with
  EPT and the TDP MMU off (`-a` overrides the command line, `-c` pins L1 to
  host CPUs). The heredoc inside `run_vm()` lists the commands to run in
  the guest: `mmu_stress_test`, `demand_paging_test` (recorded with
  `perf kvm` into flame graphs), and the nested (L2) Redis benchmark.
- `sudo cse291/redis_repeat.sh [-r REPS] [-o OUT.csv] [-a LRU_AGE]`: run
  inside L1. Boots L2 once, pins it, and runs redis-benchmark REPS times per
  policy, interleaved, after one discarded warm-up run, with no perf
  recording. Writes each run to a CSV and prints median and range per test.
- `tools/testing/selftests/kvm/x86/lru_reuse_test.c`: seeded hot-set/scan
  trace that prints eviction counters over the measured phase. Results in
  `redis_bench.md` ("Eviction Counters with Reuse").

Earlier `run_bench.sh` runs left EPT on (the `ept=0` line was commented
out), so they tested the legacy MMU managing direct-mapped EPT tables, not
shadowing of guest page tables.

## Results caveat

`redis_bench.md` came from a build with **every** `mark_kvm_page_accessed()`
call commented out. Commit `9879770e` disabled them, `1b9d1922` committed
`redis_bench.md`, and `ff563476` restored two of them. In that build
`lru_ref` was never set, so "LRU" behaved like FIFO with a rotating start
point. It was not approximate LRU. It also ran with EPT on and one run per
policy. The code is fixed; the Redis numbers have not been re-measured
(use `redis_repeat.sh`). Raw output is in `raw_results.txt`.

## Report

`report/report.tex` (built as `report/report.pdf` with `pdflatex report.tex`,
run twice for cross-references) is the final report, revised to describe the
fixed CLOCK code above and its KASAN/lockdep validation under QEMU TCG
(`kvm_amd.npt=0`). Its performance numbers come from the original `c5n.metal`
runs and have not been re-measured with the fix.
