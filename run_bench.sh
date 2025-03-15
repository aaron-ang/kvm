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
SHARED_DIR=$(dirname "$SCRIPT_DIR")
SOCKET_PATH="/tmp/virtiofs.sock"

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
    apt install -y \
        wget cloud-utils qemu-system qemu-utils virtiofsd \
        libzstd1 libzstd-dev zlib1g-dev liblzma-dev \
        libdwarf-dev libdw-dev libunwind-dev debuginfod \
        libpfm4-dev systemtap-sdt-dev libbabeltrace-dev \
        libcap-dev libnuma-dev libaio-dev libtraceevent-dev \
        libslang2-dev libperl-dev libiberty-dev clang llvm-dev \
        libcapstone-dev libtracefs-dev binutils-dev \
        linux-headers-generic python3 python3-dev
}

check_files() {
    if [ ! -f "$KERNEL" ]; then
        echo "ERROR: bzImage not found at $KERNEL. Please compile the kernel first with build.sh."
        exit 1
    fi

    # Download cloud image
    if [ ! -f "$CLOUD_IMG" ]; then
        wget -4 -O "$CLOUD_IMG" https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
        qemu-img resize "$CLOUD_IMG" 16G
    fi
}

create_config() {
    # Create user-data config
    cat >"$USER_CONFIG" <<EOF
#cloud-config
password: password
chpasswd: { expire: False }
ssh_pwauth: True
package_update: true
package_upgrade: true
packages:
    - libbabeltrace-dev
    - libcapstone-dev
    - libpfm4-dev
runcmd:
    - mount -t virtiofs hostshare /mnt
    - echo 'hostshare /mnt virtiofs defaults' | tee -a /etc/fstab
    - fallocate -l 2G /swapfile
    - chmod 600 /swapfile
    - mkswap /swapfile
    - swapon /swapfile
    - echo '/swapfile none swap sw 0 0' | tee -a /etc/fstab
    - echo 'alias perf=/mnt/kvm/tools/perf/perf' | tee -a /home/ubuntu/.bashrc
    - echo "alias sudo='sudo '" | tee -a /home/ubuntu/.bashrc
    - . /home/ubuntu/.bashrc
    - echo 'kernel.perf_event_paranoid=-1' | tee -a /etc/sysctl.conf
    - sysctl -p
EOF

    # Create seed image
    cloud-localds "$SEED_IMG" "$USER_CONFIG"
}

build_tests() {
    cd $SCRIPT_DIR
    make kselftest-install -j$(nproc)
    make -C tools/perf -j$(nproc)
    cd $SHARED_DIR
    if [ ! -d "FlameGraph" ]; then
        git clone https://github.com/brendangregg/FlameGraph.git
    fi
    cd -
}

start_virtiofsd() {
    /usr/libexec/virtiofsd --socket-path "$SOCKET_PATH" --shared-dir "$SHARED_DIR" &
}

run_vm() {
    # Notes:
    # - user: ubuntu
    # - password: password
    # - Ctrl-A x to exit QEMU session
    # - Ctrl-A h for help
    : <<'COMMANDS'
cd /mnt

echo 0 | sudo tee /sys/module/kvm/parameters/tdp_mmu
echo 0 | sudo tee /sys/module/kvm_intel/parameters/ept
echo 32 | sudo tee /sys/module/kvm/parameters/min_alloc_pages

sudo kvm/tools/testing/selftests/kvm/demand_paging_test -v $(nproc) -b $(( ( 4 << 30 ) / $(nproc) )) &
test_pid=$!
sleep 10
echo "Running perf-kvm on demand paging test..."
sudo perf kvm --host --guest record --call-graph dwarf --all-cpus -g -o kvm/dd_paging.data -- sleep 5
sudo kill $test_pid
sudo perf script -i kvm/dd_paging.data | sudo tee kvm/dd_paging.perf > /dev/null
sudo FlameGraph/stackcollapse-perf.pl kvm/dd_paging.perf | \
    sudo FlameGraph/flamegraph.pl --colors java --title "Demand Paging: $(uname -r)" | \
    sudo tee kvm/dd_paging.svg > /dev/null

sudo kvm/tools/testing/selftests/kvm/mmu_stress_test -m 4 &
test_pid=$!
sleep 30
echo "Running perf-kvm on mmu stress test..."
sudo perf kvm --host --guest record --call-graph dwarf --all-cpus -g -o kvm/mmu_stress.data -- sleep 10
sudo kill $test_pid
sudo perf script -i kvm/mmu_stress.data | sudo tee kvm/mmu_stress.perf > /dev/null
sudo FlameGraph/stackcollapse-perf.pl kvm/mmu_stress.perf | \
    sudo FlameGraph/flamegraph.pl --colors java --title "MMU Stress: $(uname -r)" | \
    sudo tee kvm/mmu_stress.svg > /dev/null
COMMANDS
    qemu-system-x86_64 \
        -kernel "$KERNEL" \
        -drive id=root,media=disk,file="$CLOUD_IMG" \
        -drive file="$SEED_IMG",format=raw \
        -cpu host -smp $(nproc) \
        -enable-kvm -m 4G \
        -object memory-backend-memfd,id=mem,size=4G,share=on \
        -numa node,memdev=mem \
        -chardev socket,id=char0,path="$SOCKET_PATH" \
        -device vhost-user-fs-pci,queue-size=1024,chardev=char0,tag=hostshare \
        -device virtio-net,netdev=vmnic -netdev user,id=vmnic \
        -append "console=ttyS0 root=/dev/sda1" \
        -nographic
}

# Main execution
install_dependencies
check_files
create_config
build_tests
start_virtiofsd
run_vm
