echo 0 | sudo tee /sys/module/kvm/parameters/tdp_mmu
echo 0 | sudo tee /sys/module/kvm/parameters/lru_mmu
echo 0 | sudo tee /sys/module/kvm_intel/parameters/ept
echo 32 | sudo tee /sys/module/kvm/parameters/min_alloc_pages
