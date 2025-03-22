#!/bin/bash

set -e # Exit on error

# Check for root privileges
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root"
    exit 1
fi

GIT_BRANCH=$(git branch --show-current)
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
KVM_DIR=$(dirname "$SCRIPT_DIR")

install_dependencies() {
    apt update -q

    apt install -y \
        git fakeroot build-essential ncurses-dev xz-utils \
        libssl-dev bc flex libelf-dev bison binutils dwarves gcc gnupg2 \
        gzip make openssl pahole perl-base rsync
}

setup_config() {
    # Clean old configs
    rm -f .config .config.old

    # Copy current kernel config as base
    cp -v /boot/config-$(uname -r) .config

    # Use defaults for new options
    make defconfig
    # make kselftest-merge

    # Enable KVM-related options
    scripts/config --enable KVM
    scripts/config --enable KVM_X86
    scripts/config --enable KVM_INTEL
    scripts/config --enable EXPERT                 # for KVM_PROVE_MMU
    scripts/config --enable KVM_PROVE_MMU          # to verify MMU operations
    scripts/config --enable UNWINDER_FRAME_POINTER # for unwinding kernel stack traces
    scripts/config --enable USERFAULTFD
    scripts/config --enable BPF_SYSCALL
    scripts/config --enable BPF_JIT
    scripts/config --enable CGROUP_BPF
    scripts/config --enable TRANSPARENT_HUGEPAGE # for kvm/mmu_stress_test
    scripts/config --enable VHOST_NET            # for virtiofsd
    scripts/config --enable FUSE_FS              # for virtiofsd
    scripts/config --enable VIRTIO_FS

    # Disable signature verification
    scripts/config --disable SYSTEM_TRUSTED_KEYS
    scripts/config --disable SYSTEM_REVOCATION_KEYS

    # Set custom version
    scripts/config --set-str LOCALVERSION "-$GIT_BRANCH"
}

build_kernel() {
    make bzImage -j$(nproc)
}

# Main execution
cd "$KVM_DIR"
install_dependencies
setup_config
build_kernel
