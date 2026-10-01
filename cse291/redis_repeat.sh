#!/bin/bash
#
# Repeated Redis benchmark for FIFO vs CLOCK shadow-page eviction.
# Run inside L1 (the VM running our KVM), after run_bench.sh has prepared the
# L2 cloud image and seed image.
#
# Method, and why (the first single-run results swung +-36% on PING, which
# never touches eviction):
#   - One L2 boot for all runs. The policy is switched between runs through
#     /sys/module/kvm/parameters/lru_mmu, so both policies share the guest,
#     the host state and the time window.
#   - Policies interleaved, alternating which one goes first each round
#     (ABBA...), so drift over time hits both equally.
#   - One discarded warm-up run before measuring.
#   - L2's vCPUs pinned to L1 CPUs 1..N (CPU 0 left for L1 housekeeping).
#     Inside L2, redis-server on CPU 0 and redis-benchmark on the others.
#   - No perf recording during measured runs.
#   - Median and range per test and policy printed at the end; the raw runs
#     go to a CSV.
#
# Requires ept=0 and tdp_mmu=0 on L1's kernel command line (run_bench.sh does
# this), otherwise L2 runs on EPT and the shadow MMU is not exercised.

set -e

REPS=5
OUT=redis_runs.csv
POLICIES="0 1"	# lru_mmu values: 0 = FIFO, 1 = CLOCK
LRU_AGE=2	# used when lru_mmu=1: 0 = off, 1 = leaf, 2 = parent
L2_CPUS=1-3
L2_SMP=3
L2_MEM=4G
SSH_PORT=2223
TESTS=ping_inline,ping_mbulk,set,get,zadd,lrange_300,lrange_600
BENCH_ARGS="-n 1000000 -r 100000 -P 16"
USER_HOME=$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)
CLOUD_IMG=$USER_HOME/ubuntu_cloud.img
SEED_IMG=$USER_HOME/seed.img

usage() {
    echo "Usage: $0 [-r REPS] [-o OUT.csv] [-a LRU_AGE] [-t TESTS] [-i CLOUD_IMG] [-s SEED_IMG]"
    exit 1
}

while getopts "r:o:a:t:i:s:h" opt; do
    case $opt in
    r) REPS=$OPTARG ;;
    o) OUT=$OPTARG ;;
    a) LRU_AGE=$OPTARG ;;
    t) TESTS=$OPTARG ;;
    i) CLOUD_IMG=$OPTARG ;;
    s) SEED_IMG=$OPTARG ;;
    *) usage ;;
    esac
done

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root"
    exit 1
fi
for f in "$CLOUD_IMG" "$SEED_IMG"; do
    [ -f "$f" ] || { echo "ERROR: $f not found (see run_bench.sh)"; exit 1; }
done
command -v sshpass >/dev/null || apt install -y sshpass

P=/sys/module/kvm/parameters
if [ "$(cat /sys/module/kvm_intel/parameters/ept 2>/dev/null)" != N ] ||
   [ "$(cat $P/tdp_mmu)" != N ]; then
    echo "ERROR: boot L1 with kvm_intel.ept=0 kvm.tdp_mmu=0"
    exit 1
fi

l2() {
    sshpass -p password ssh -q -p $SSH_PORT -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null ubuntu@localhost "$@"
}

set_policy() {
    echo "$1" >$P/lru_mmu
    if [ "$1" = 1 ]; then echo "$LRU_AGE" >$P/lru_age; else echo 0 >$P/lru_age; fi
}

policy_name() {
    if [ "$1" = 0 ]; then echo FIFO; else echo "CLOCK(lru_age=$LRU_AGE)"; fi
}

# One run: prints "test,rps,p50" lines for each test.
bench() {
    l2 "redis-cli flushall >/dev/null &&
        taskset -c 1-$((L2_SMP - 1)) redis-benchmark $BENCH_ARGS -t $TESTS --csv" |
        tr -d '"' | awk -F, 'NR > 1 { print $1 "," $2 "," $5 }'
}

echo 3 >/proc/sys/vm/drop_caches
taskset -c "$L2_CPUS" qemu-system-x86_64 \
    -drive if=virtio,id=root,media=disk,file="$CLOUD_IMG" \
    -drive if=virtio,file="$SEED_IMG",format=raw \
    -cpu host -smp $L2_SMP -enable-kvm -m $L2_MEM \
    -nic user,model=virtio-net-pci,hostfwd=tcp::$SSH_PORT-:22 \
    -display none -daemonize -pidfile /tmp/redis_l2.pid
trap 'l2 sudo poweroff -f >/dev/null 2>&1 || kill "$(cat /tmp/redis_l2.pid)" 2>/dev/null' EXIT

echo "Waiting for L2 and redis-server..."
for _ in $(seq 180); do
    l2 'redis-cli ping' 2>/dev/null | grep -q PONG && break
    sleep 5
done
l2 'redis-cli ping' | grep -q PONG || { echo "ERROR: redis in L2 not reachable"; exit 1; }
l2 'sudo taskset -acp 0 $(pidof redis-server)' >/dev/null

echo "Warm-up run (discarded)"
set_policy 1
bench >/dev/null

echo "policy,lru_age,round,test,rps,p50_ms" >"$OUT"
order=$POLICIES
for r in $(seq "$REPS"); do
    for pol in $order; do
        set_policy "$pol"
        echo "round $r/$REPS: $(policy_name "$pol")"
        bench | sed "s/^/$(policy_name "$pol" | cut -d'(' -f1),$(cat $P/lru_age),$r,/" >>"$OUT"
    done
    order=$(echo "$order" | tr ' ' '\n' | tac | tr '\n' ' ')
done
set_policy 0

python3 - "$OUT" <<'EOF'
import csv, statistics, sys
runs = {}
for row in csv.DictReader(open(sys.argv[1])):
    runs.setdefault((row['test'], row['policy']), []).append(float(row['rps']))
tests = list(dict.fromkeys(t for t, _ in runs))
print(f"\n{'test':<34}{'FIFO median':>14}{'CLOCK median':>14}{'diff':>8}   FIFO range / CLOCK range")
for t in tests:
    f, c = runs.get((t, 'FIFO'), []), runs.get((t, 'CLOCK'), [])
    if not f or not c:
        continue
    mf, mc = statistics.median(f), statistics.median(c)
    print(f"{t:<34}{mf:>14,.0f}{mc:>14,.0f}{(mc / mf - 1) * 100:>+7.1f}%"
          f"   {min(f):,.0f}-{max(f):,.0f} / {min(c):,.0f}-{max(c):,.0f}")
print("\nA difference is meaningful only if the two ranges don't overlap.")
EOF
