#!/bin/bash

set -e # Exit on error

# Check for root privileges
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root"
    exit 1
fi

# Default paths
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
IMG_DIR="$SCRIPT_DIR/arch/x86_64/boot"
KERNEL="$IMG_DIR/bzImage"
CLOUD_IMG="$IMG_DIR/ubuntu_cloud.img"
USER_CONFIG="$IMG_DIR/user-data"
SEED_IMG="$IMG_DIR/seed.img"
SHARED_DIR="$IMG_DIR/qemu_shared"

# Display usage information
usage() {
    echo "Usage: $0 [-k KERNEL_PATH]"
    echo "  -k KERNEL_PATH  Path to the kernel image (default: $KERNEL)"
    echo "  -h              Display this help message"
    exit 1
}

# Parse command-line options
while getopts "k:h" opt; do
    case ${opt} in
    k)
        CUSTOM_KERNEL=$OPTARG
        if [ -f "$CUSTOM_KERNEL" ]; then
            KERNEL=$CUSTOM_KERNEL
        else
            echo "ERROR: Specified kernel not found: $CUSTOM_KERNEL"
            exit 1
        fi
        ;;
    h)
        usage
        ;;
    \?)
        usage
        ;;
    esac
done
shift $((OPTIND - 1))

install_dependencies() {
    apt update -q
    apt install -y wget cloud-utils qemu-system
}

check_files() {
    if [ ! -f "$KERNEL" ]; then
        echo "ERROR: bzImage not found at $KERNEL. Please compile the kernel first with build.sh."
        exit 1
    fi

    # Download cloud image
    if [ ! -f "$CLOUD_IMG" ]; then
        wget -4 -O "$CLOUD_IMG" https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
        qemu-img resize "$CLOUD_IMG" 10G
    fi

    # Create user-data config
    cat >"$USER_CONFIG" <<EOF
#cloud-config
password: password
chpasswd: { expire: False }
ssh_pwauth: True
package_update: true
package_upgrade: true
packages:
    - stress-ng
runcmd:
    - mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt
    - echo 'hostshare /mnt 9p trans=virtio,version=9p2000.L 0 0' | tee -a /etc/fstab
    - fallocate -l 4G /swapfile
    - chmod 600 /swapfile
    - mkswap /swapfile
    - swapon /swapfile
    - echo '/swapfile none swap sw 0 0' | tee -a /etc/fstab
    - |
      stress-ng --class vm --all 1 --vm-bytes 100% --page-in \
--perf --verify --times --log-brief --metrics-brief --timeout 1m \
| tee /mnt/stress-ng_\$(uname -r)_\$(date +%Y-%m-%d_%H-%M).log
EOF

    # Create seed image
    cloud-localds "$SEED_IMG" "$USER_CONFIG"

    # Create shared directory
    mkdir -p "$SHARED_DIR"
}

run_vm() {
    # If not first boot, run stress-ng command in $USER_CONFIG as root
    qemu-system-x86_64 \
        -kernel "$KERNEL" \
        -drive id=root,media=disk,file="$CLOUD_IMG" \
        -drive file="$SEED_IMG",format=raw \
        -cpu host \
        -smp $(nproc) \
        -enable-kvm \
        -m 2G \
        -device virtio-net,netdev=vmnic -netdev user,id=vmnic \
        -virtfs local,path="$SHARED_DIR",mount_tag=hostshare,security_model=none \
        -append "console=ttyS0 root=/dev/sda1" \
        -nographic
}

# Main execution
install_dependencies
check_files
run_vm

# Notes:
# - Ctrl-A x to exit QEMU session
# - Ctrl-A h for help
