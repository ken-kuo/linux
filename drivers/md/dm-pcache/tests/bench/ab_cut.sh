#!/bin/bash
# A/B (or A/B/C/...) module builds against each other, interleaved, same session.
# Usage: ab_cut.sh <KO_A> <KO_B> [KO_C ...] [rounds]
#   KO_* = filenames under /root on the nodes (e.g. dm-pcache.ko dm-pcache-cut1.ko)
#   rounds = optional trailing number, default 3
# Reports qd=32 streams=2 128K-seq (size=800M, resync-settled), N rounds interleaved.
# Interleaving is the point: this host's cross-VM round-trip rate drifts, so
# only builds measured inside one session may be compared.
set -uo pipefail
cd "$(dirname "$0")"
source ./common.sh
KOS=(); ROUNDS=3
for a in "$@"; do
    case "$a" in
    *[!0-9]*) KOS+=("$a") ;;    # a .ko name
    *)        ROUNDS=$a ;;      # trailing all-digit arg is the round count
    esac
done
[ ${#KOS[@]} -ge 2 ] || { echo "usage: ab_cut.sh <KO_A> <KO_B> [KO_C ...] [rounds]" >&2; exit 1; }
rsh() { command ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@"$1" "$2"; }

# Verify what is ACTUALLY loaded before trusting a number.
#
# This used to be `rmmod ... || true; insmod ...`, so a device left open made
# rmmod fail, insmod printed "File exists", and the PREVIOUS module stayed
# resident -- the run then reported numbers labelled with the build that never
# loaded. Same trap assert_module_fresh() guards against in demo_features.sh.
verify_loaded() { # ko
    local want a b
    want=$(rsh $NODE1_IP "modinfo -F srcversion /root/$1")
    a=$(rsh $NODE1_IP 'cat /sys/module/dm_pcache/srcversion 2>/dev/null')
    b=$(rsh $NODE2_IP 'cat /sys/module/dm_pcache/srcversion 2>/dev/null')
    [ "$a" = "$want" ] && [ "$b" = "$want" ] && return 0
    echo "  !! wrong build loaded for $1: node1=$a node2=$b want=$want — aborting" >&2
    return 1
}

up() { # ko streams
    local ko=$1
    rsh $NODE2_IP "dmsetup remove pcache-bench 2>/dev/null||true; rmmod dm_pcache 2>/dev/null||true; modprobe -r null_blk 2>/dev/null||true; modprobe null_blk nr_devices=1 memory_backed=1 queue_mode=2 irqmode=0 completion_nsec=0 bs=4096 gb=2; dd if=/dev/zero of=/dev/pmem0 bs=1M count=32 oflag=direct status=none; insmod /root/$ko; SZ=\$(blockdev --getsz /dev/nullb0); dmsetup create pcache-bench --table \"0 \$SZ pcache-dist /dev/pmem0 /dev/nullb0 my_node_id=2 listen_port=$PORT_DDMPCACHE is_primary=0 quorum=1 streams=2\"; dmsetup message pcache-bench 0 add-peer 1 $NODE1_IP:$PORT_DDMPCACHE"
    rsh $NODE1_IP "dmsetup remove pcache-bench 2>/dev/null||true; rmmod dm_pcache 2>/dev/null||true; modprobe -r null_blk 2>/dev/null||true; modprobe null_blk nr_devices=1 memory_backed=1 queue_mode=2 irqmode=0 completion_nsec=0 bs=4096 gb=2; dd if=/dev/zero of=/dev/pmem0 bs=1M count=32 oflag=direct status=none; insmod /root/$ko; SZ=\$(blockdev --getsz /dev/nullb0); dmsetup create pcache-bench --table \"0 \$SZ pcache-dist /dev/pmem0 /dev/nullb0 my_node_id=1 listen_port=$PORT_DDMPCACHE is_primary=1 quorum=1 streams=2\"; dmsetup message pcache-bench 0 add-peer 2 $NODE2_IP:$PORT_DDMPCACHE"
    sleep 9
}
# Retry the teardown: the device can still be open for a moment after fio
# exits, and a failed rmmod is what leaves the previous module resident.
down() {
    for ip in $NODE1_IP $NODE2_IP; do
        rsh "$ip" 'for i in 1 2 3 4 5; do dmsetup remove pcache-bench 2>/dev/null && break; sleep 1; done
                   dmsetup remove -f pcache-bench 2>/dev/null
                   for i in 1 2 3 4 5; do rmmod dm_pcache 2>/dev/null && break; sleep 1; done
                   modprobe -r null_blk 2>/dev/null
                   exit 0' >/dev/null 2>&1
        # A hung I/O holds the device open, so rmmod never succeeds and every
        # later round would silently run the previous build. Only a reboot
        # clears that.
        if [ -n "$(rsh "$ip" 'cat /sys/module/dm_pcache/srcversion 2>/dev/null')" ]; then
            echo "  !! $ip: module stuck, rebooting" >&2
            rsh "$ip" '(sleep 1; reboot -f) >/dev/null 2>&1 &' >/dev/null 2>&1
            for i in $(seq 1 30); do sleep 10; rsh "$ip" true >/dev/null 2>&1 && break; done
        fi
    done
}
warm() { rsh $NODE1_IP "fio --name=w --filename=$DDMPCACHE_DEV --rw=write --bs=128k --iodepth=8 --ioengine=libaio --direct=1 --size=512M --runtime=4 --time_based --group_reporting >/dev/null 2>&1 || true"; }
bw() { rsh $NODE1_IP "fio --filename=$DDMPCACHE_DEV --ioengine=libaio --direct=1 --time_based=1 --ramp_time=3 --runtime=12 --norandommap=1 --randrepeat=0 --group_reporting=1 --continue_on_error=write --rw=write --bs=128k --iodepth=32 --numjobs=1 --size=800M --name=w3 2>&1 | grep -oE 'bw=[0-9.]+MiB/s' | head -1 | grep -oE '[0-9.]+'"; }

for r in $(seq 1 $ROUNDS); do
    for ko in "${KOS[@]}"; do
        down; up "$ko"
        verify_loaded "$ko" || { down; exit 1; }
        warm
        echo "round $r  $ko : $(bw) MiB/s"
    done
done
down
echo "DONE"
