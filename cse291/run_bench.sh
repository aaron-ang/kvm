#!/bin/bash

set -e # Exit on error

# Check for root privileges
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root"
    exit 1
fi

# Default paths
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
KVM_DIR=$(dirname "$SCRIPT_DIR")
IMG_DIR="$KVM_DIR/arch/x86_64/boot"
KERNEL="$IMG_DIR/bzImage"
CLOUD_IMG="$IMG_DIR/ubuntu_cloud.img"
USER_CONFIG="$IMG_DIR/user-data"
SEED_IMG="$IMG_DIR/seed.img"
SHARED_DIR=$(dirname "$KVM_DIR")
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
        qemu-img resize "$CLOUD_IMG" 12G
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
    - cloud-image-utils
    - qemu-system-x86
    - lsb-release
    - curl
    - gpg
runcmd:
    - curl -fsSL https://packages.redis.io/gpg | gpg --dearmor -o /usr/share/keyrings/redis-archive-keyring.gpg
    - chmod 644 /usr/share/keyrings/redis-archive-keyring.gpg
    - echo "deb [signed-by=/usr/share/keyrings/redis-archive-keyring.gpg] https://packages.redis.io/deb \$(lsb_release -cs) main" | tee /etc/apt/sources.list.d/redis.list
    - apt update
    - apt install -y redis
    - systemctl enable redis-server
    - systemctl start redis-server
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
    cd $KVM_DIR
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
echo 0 | sudo tee /sys/module/kvm/parameters/tdp_mmu
# echo 0 | sudo tee /sys/module/kvm_intel/parameters/ept
echo 32 | sudo tee /sys/module/kvm/parameters/min_alloc_pages
echo 1 | sudo tee /sys/module/kvm/parameters/lru_mmu
echo 0 | sudo tee /sys/module/kvm/parameters/lru_mmu

cat /sys/module/kvm/parameters/lru_mmu
cat /sys/module/kvm/parameters/min_alloc_pages

### Run tests

sudo /mnt/kvm/tools/testing/selftests/kvm/mmu_stress_test &
test_pid=$!
sleep 15
echo "Running perf-kvm on mmu stress test..."
sudo perf kvm --host --guest record --call-graph dwarf --all-cpus -g -o /mnt/kvm/mmu_stress.data -- sleep 10
kill $test_pid
sudo perf script -i /mnt/kvm/mmu_stress.data | sudo tee /mnt/kvm/mmu_stress.perf > /dev/null
sudo /mnt/FlameGraph/stackcollapse-perf.pl /mnt/kvm/mmu_stress.perf | \
    sudo /mnt/FlameGraph/flamegraph.pl --colors java --title "MMU Stress: $(uname -r)" | \
    sudo tee /mnt/kvm/mmu_stress.svg > /dev/null

# mv /mnt/kvm/mmu_stress.svg /mnt/kvm/cse291/mmu_stress_{fifo | lru}.svg

sudo /mnt/kvm/tools/testing/selftests/kvm/demand_paging_test -v $(nproc) -b $(( ( 9 << 30 ) / $(nproc) )) &
test_pid=$!
sleep 5
echo "Running perf-kvm on demand paging test..."
sudo perf kvm --host --guest record --call-graph dwarf --all-cpus -g -o /mnt/kvm/dd_paging.data -- sleep 5
kill $test_pid
sudo perf script -i /mnt/kvm/dd_paging.data | sudo tee /mnt/kvm/dd_paging.perf > /dev/null
sudo /mnt/FlameGraph/stackcollapse-perf.pl /mnt/kvm/dd_paging.perf | \
    sudo /mnt/FlameGraph/flamegraph.pl --colors java --title "Demand Paging: $(uname -r)" | \
    sudo tee /mnt/kvm/dd_paging.svg > /dev/null

# mv /mnt/kvm/dd_paging.svg /mnt/kvm/cse291/dd_paging_{fifo | lru}.svg

### Run Redis benchmark

CLOUD_IMG=~/ubuntu_cloud.img
SEED_IMG=~/seed.img
if [ ! -f "$CLOUD_IMG" ]; then
    wget -4 -O "$CLOUD_IMG" https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
    qemu-img resize "$CLOUD_IMG" 8G
fi
cp /mnt/kvm/arch/x86_64/boot/user-data ~/user-data
echo 'Replacing virtiofs with 9p in user-data...'
sed -i '/mount -t virtiofs/s/mount -t virtiofs hostshare \/mnt/mount -t 9p -o trans=virtio hostshare \/mnt -oversion=9p2000.L/' ~/user-data
sed -i '/echo.*hostshare.*fstab/s/virtiofs/9p/' ~/user-data
cloud-localds "$SEED_IMG" ~/user-data

echo 3 | sudo tee /proc/sys/vm/drop_caches
sudo qemu-system-x86_64 \
    -drive if=virtio,id=root,media=disk,file="$CLOUD_IMG" \
    -drive if=virtio,file="$SEED_IMG",format=raw \
    -cpu host -smp 4 \
    -enable-kvm -m 4G \
    -virtfs local,path=/mnt,mount_tag=hostshare,security_model=none \
    -nic user,model=virtio-net-pci \
    -nographic

# In nested VM:
redis-cli flushall && redis-benchmark -n 1000000 -r 100000 -P 16 -q

redis-cli flushall && redis-benchmark -n 2000000 -d 1024 -r 1000000 --threads 4 -t lrange_600 -P 16 -q

# In host VM (ssh -p 2222 ubuntu@localhost):
sudo perf kvm --host record --call-graph dwarf --all-cpus -g -o /mnt/kvm/redis_bench.data -- sleep 10
sudo perf script -i /mnt/kvm/redis_bench.data | sudo tee /mnt/kvm/redis_bench.perf > /dev/null
sudo /mnt/FlameGraph/stackcollapse-perf.pl /mnt/kvm/redis_bench.perf | \
    sudo /mnt/FlameGraph/flamegraph.pl --colors java --title "Redis Benchmark: $(uname -r)" | \
    sudo tee /mnt/kvm/redis_bench.svg > /dev/null

# Once perf is complete, Ctrl-C in nested VM.

# Go back to host VM:
sudo poweroff -f
COMMANDS
    qemu-system-x86_64 \
        -kernel "$KERNEL" \
        -drive if=virtio,id=root,media=disk,file="$CLOUD_IMG" \
        -drive if=virtio,file="$SEED_IMG",format=raw \
        -cpu host -smp 4 \
        -enable-kvm -m 8G \
        -object memory-backend-memfd,id=mem,size=8G,share=on \
        -numa node,memdev=mem \
        -chardev socket,id=char0,path="$SOCKET_PATH" \
        -device vhost-user-fs-pci,queue-size=1024,chardev=char0,tag=hostshare \
        -netdev user,id=net0,hostfwd=tcp::2222-:22 \
        -device virtio-net-pci,netdev=net0 \
        -append "console=ttyS0 root=/dev/vda1" \
        -nographic

    # wait for user to exit QEMU session
    read -p "Press Enter to continue..."
}

cleanup() {
    cd $KVM_DIR
    rm -f *.svg *.data *.perf *.old
}

# Main execution
install_dependencies
check_files
create_config
build_tests
start_virtiofsd
run_vm
cleanup
