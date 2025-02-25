#!/bin/bash

set -e # Exit on error

# Check for root privileges
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root"
    exit 1
fi

GIT_BRANCH=$(git branch --show-current)

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

    # Enable KVM-related options
    scripts/config --enable KVM
    scripts/config --enable KVM_X86
    scripts/config --enable KVM_INTEL
    scripts/config --enable EXPERT
    scripts/config --enable KVM_PROVE_MMU
    scripts/config --enable VHOST_NET

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
install_dependencies
setup_config
build_kernel
