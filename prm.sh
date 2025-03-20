echo 0 | tee /sys/module/kvm/parameters/tdp_mmu
echo 0 | tee /sys/module/kvm_intel/parameters/ept
echo 1 | tee /sys/module/kvm/parameters/lru_mmu
echo 400 | tee /sys/module/kvm/parameters/min_alloc_pages
