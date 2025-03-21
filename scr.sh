echo 0 | sudo tee /sys/module/kvm/parameters/tdp_mmu
echo 1 | sudo tee /sys/module/kvm/parameters/lru_mmu
echo 0 | sudo tee /sys/module/kvm_intel/parameters/ept
echo 128 | sudo tee /sys/module/kvm/parameters/min_alloc_pages
