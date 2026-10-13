#!/bin/bash
# ---------------------------------------------------------------------------
# zc_hang.sh <ko-name> <bs> -- minimal reproducer for the MSG_ZEROCOPY hang.
#
#   bash repro/zc_hang.sh dm-pcache-zcmore.ko 128k
#
# Every step is verified before the next one runs. That is not ceremony: an
# earlier run of this by hand had dmsetup create fail silently, so
# `dd of=/dev/mapper/pcache-bench` created a 26 MB REGULAR FILE and reported
# 1.5 GB/s with RC=0 -- a completely fake "it works".
# ---------------------------------------------------------------------------
set -uo pipefail
cd "$(dirname "$0")/../bench"
source ./common.sh
KO=${1:?usage: zc_hang.sh <ko-name> <bs>}
BS=${2:-128k}
rsh() { timeout 90 ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 root@"$1" "$2"; }
die() { echo "ABORT: $*"; exit 1; }

for ip in $NODE1_IP $NODE2_IP; do
    rsh "$ip" true >/dev/null 2>&1 || die "$ip unreachable"
    rsh "$ip" "test -f /root/$KO" || die "$ip has no /root/$KO"
done
WANT=$(rsh $NODE1_IP "modinfo -F srcversion /root/$KO")
echo "== testing $KO ($WANT), bs=$BS"

for ip in $NODE1_IP $NODE2_IP; do
    rsh "$ip" 'pkill -9 -x dd 2>/dev/null; dmsetup remove -f pcache-bench 2>/dev/null
               rm -f /dev/mapper/pcache-bench           # a stale regular file would poison the test
               rmmod dm_pcache 2>/dev/null; modprobe -r null_blk 2>/dev/null; dmesg -C; exit 0' >/dev/null 2>&1
    # A previous hang leaves a dd stuck in bio_await holding the device, so the
    # module cannot be unloaded and the next insmod fails with "File exists".
    # Only a reboot clears that, and doing it here is what makes this script
    # runnable back-to-back.
    if [ -n "$(rsh "$ip" 'cat /sys/module/dm_pcache/srcversion 2>/dev/null')" ]; then
        echo "   $ip: module stuck from a previous hang, rebooting"
        rsh "$ip" '(sleep 1; reboot -f) >/dev/null 2>&1 &' >/dev/null 2>&1
        for i in $(seq 1 30); do sleep 10; rsh "$ip" true >/dev/null 2>&1 && break; done
        rsh "$ip" true >/dev/null 2>&1 || die "$ip did not come back"
    fi
done

setup() { # ip node_id is_primary
    rsh "$1" "modprobe null_blk nr_devices=1 memory_backed=1 queue_mode=2 irqmode=0 completion_nsec=0 bs=4096 gb=2 &&
              dd if=/dev/zero of=/dev/pmem0 bs=1M count=32 oflag=direct status=none &&
              insmod /root/$KO &&
              SZ=\$(blockdev --getsz /dev/nullb0) &&
              dmsetup create pcache-bench --table \"0 \$SZ pcache-dist /dev/pmem0 /dev/nullb0 my_node_id=$2 listen_port=$PORT_DDMPCACHE is_primary=$3 quorum=1 streams=1\"" \
        || die "$1 setup failed"
}
setup $NODE2_IP 2 0
setup $NODE1_IP 1 1
rsh $NODE2_IP "dmsetup message pcache-bench 0 add-peer 1 $NODE1_IP:$PORT_DDMPCACHE" || die "node2 add-peer failed"
rsh $NODE1_IP "dmsetup message pcache-bench 0 add-peer 2 $NODE2_IP:$PORT_DDMPCACHE" || die "node1 add-peer failed"

# preconditions the earlier hand-run skipped
for ip in $NODE1_IP $NODE2_IP; do
    rsh "$ip" "test -b $DDMPCACHE_DEV" || die "$ip: $DDMPCACHE_DEV is not a block device"
    got=$(rsh "$ip" 'cat /sys/module/dm_pcache/srcversion')
    [ "$got" = "$WANT" ] || die "$ip loaded $got, want $WANT"
done
echo "   both nodes up on $WANT, $DDMPCACHE_DEV is a block device"

for i in $(seq 1 20); do
    st=$(rsh $NODE1_IP 'dmesg | grep -oE "repl_state [A-Z_]+ -> [A-Z_]+" | tail -1 | awk "{print \$4}"')
    [ "$st" = "ESTABLISHED" ] && break
    sleep 2
done
[ "$st" = "ESTABLISHED" ] || die "replication never reached ESTABLISHED (last=$st)"
echo "   replication ESTABLISHED"

echo "== single $BS direct write (15s timeout)"
rsh $NODE1_IP "timeout 15 dd if=/dev/zero of=$DDMPCACHE_DEV bs=$BS count=1 oflag=direct 2>&1 | tail -1; echo RC=\$?"
echo "== ZCDBG"
rsh $NODE1_IP 'echo "   lines=$(dmesg | grep -c ZCDBG) complete=$(dmesg | grep -c "ZCDBG complete")"; dmesg | grep ZCDBG | tail -5'
