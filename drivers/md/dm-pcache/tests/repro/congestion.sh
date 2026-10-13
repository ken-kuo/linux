#!/bin/bash
# ---------------------------------------------------------------------------
# congestion.sh <ko-name> -- fix-history #21b.
#
#   bash tests/repro/congestion.sh dm-pcache-p21-after.ko
#
# Makes the secondary slow (PCACHE_FAULT_SLOW_ACK stalls its single-threaded
# receive loop for a second before each ACK, which is the shape real congestion
# takes) and reports what the primary does about it.
#
# Unfixed: the oldest unacked write blows PCACHE_ACK_TIMEOUT_SECS, the primary
#          declares the link dead and reconnects -- repeatedly, on a link that
#          is merely slow.
# Fixed:   the primary goes PCACHE_R_AHEAD, accounts the writes in the bitmap,
#          and sweeps the debt back once the backlog drains. No reconnect.
#
# Either way the application write must never fail, and the nodes must agree
# afterwards.
# ---------------------------------------------------------------------------
set -uo pipefail
cd "$(dirname "$0")/../bench"
source ./common.sh
KO=${1:?usage: congestion.sh <ko-name>}
RATE=${RATE:-100}
# Congestion that never clears is a peer that is permanently too slow, and every
# design ends up reconnecting on that. What #21b is about is congestion that
# passes: inject for a window, then let the link recover while the writer runs.
INJECT=${INJECT:-25}
# The backlog depth is what crosses the thresholds: with one second of stall per
# ACK, the oldest unacked write waits roughly iodepth seconds. iodepth=8 tops out
# at ~8s, under both PCACHE_AHEAD_ENTER_SECS and the reap deadline, and the run
# proves nothing.
QD=${QD:-64}
SECS=${SECS:-60}
rsh() { timeout 300 ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 root@"$1" "$2"; }
die() { echo "ABORT: $*"; exit 1; }
cnt() { rsh "$1" "dmesg | grep -c \"$2\" || true"; }

for ip in $NODE1_IP $NODE2_IP; do
    rsh "$ip" true >/dev/null 2>&1 || die "$ip unreachable"
    rsh "$ip" "test -f /root/$KO" || die "$ip has no /root/$KO"
done
WANT=$(rsh $NODE1_IP "modinfo -F srcversion /root/$KO")
echo "== $KO ($WANT), slow-ACK rate=$RATE% for ${SECS}s"

for ip in $NODE1_IP $NODE2_IP; do
    rsh "$ip" 'pkill -9 -x fio 2>/dev/null; pkill -9 -x dd 2>/dev/null
               for d in $(dmsetup ls 2>/dev/null | awk "/pcache/{print \$1}"); do dmsetup remove -f "$d" 2>/dev/null; done
               for i in 1 2 3 4 5; do rmmod dm_pcache 2>/dev/null && break; sleep 1; done
               modprobe -r null_blk 2>/dev/null; dmesg -C; exit 0' >/dev/null 2>&1
    if [ -n "$(rsh "$ip" 'cat /sys/module/dm_pcache/srcversion 2>/dev/null')" ]; then
        echo "   $ip: module stuck, rebooting"
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

for ip in $NODE1_IP $NODE2_IP; do
    rsh "$ip" "test -b $DDMPCACHE_DEV" || die "$ip: $DDMPCACHE_DEV is not a block device"
    got=$(rsh "$ip" 'cat /sys/module/dm_pcache/srcversion')
    [ "$got" = "$WANT" ] || die "$ip loaded $got, want $WANT"
done
for i in $(seq 1 20); do
    st=$(rsh $NODE1_IP 'dmesg | grep -oE "repl_state [A-Z_]+ -> [A-Z_]+" | tail -1 | awk "{print \$4}"')
    [ "$st" = "ESTABLISHED" ] && break
    sleep 2
done
[ "$st" = "ESTABLISHED" ] || die "replication never reached ESTABLISHED (last=$st)"
echo "   both nodes on $WANT, replication ESTABLISHED"

echo "== write load; secondary stalled for the first ${INJECT}s of ${SECS}s"
rsh $NODE1_IP "(timeout $((SECS + 30)) fio --name=w --filename=$DDMPCACHE_DEV --rw=write --bs=64k \
        --iodepth=$QD --ioengine=libaio --direct=1 --size=512M --runtime=$SECS --time_based \
        --group_reporting --continue_on_error=none >/tmp/fio.log 2>&1 &) ; sleep 1" || die "fio launch failed"
rsh $NODE2_IP "echo $RATE > /sys/module/dm_pcache/parameters/fault_rate
               echo 16   > /sys/module/dm_pcache/parameters/fault_types
               sleep $INJECT
               echo 0 > /sys/module/dm_pcache/parameters/fault_rate
               echo 0 > /sys/module/dm_pcache/parameters/fault_types
               echo stalled=\$(cat /sys/module/dm_pcache/parameters/fault_count)"
echo "   injection window over; letting the writer finish"
for i in $(seq 1 60); do
    rsh $NODE1_IP 'pgrep -x fio >/dev/null' || break
    sleep 5
done
rsh $NODE1_IP "grep -E 'err=|io_u error|WRITE:' /tmp/fio.log | head -3"

echo "== verdict"
echo "   went AHEAD          : $(cnt $NODE1_IP 'going AHEAD')"
echo "   swept the debt back : $(cnt $NODE1_IP 'backlog drained')"
echo "   ACK timeouts        : $(cnt $NODE1_IP 'request timeout')"
echo "   forced reconnects   : $(cnt $NODE1_IP 'ACK timeout — forcing reconnect')"
echo "   user I/O errors     : $(cnt $NODE1_IP 'io_u error')"
echo "   repl_state trail    : $(rsh $NODE1_IP 'dmesg | grep -oE "repl_state [A-Z_]+ -> [A-Z_]+" | tail -6 | tr "\n" ";"')"
for i in $(seq 1 40); do
    d=$(rsh $NODE1_IP 'dmsetup status pcache-bench 2>/dev/null | tr " " "\n" | grep -oE "bm_dirty=[0-9]+" | head -1')
    [ "$d" = "bm_dirty=0" ] && break
    sleep 5
done
echo "   dirty after settle  : ${d:-unknown}"
m1=$(rsh $NODE1_IP "dd if=$DDMPCACHE_DEV bs=1M count=64 iflag=direct status=none | md5sum | cut -d' ' -f1")
m2=$(rsh $NODE2_IP "dd if=$DDMPCACHE_DEV bs=1M count=64 iflag=direct status=none | md5sum | cut -d' ' -f1")
[ "$m1" = "$m2" ] && echo "   DATA MATCHES ($m1)" || echo "   DATA DIVERGED: $m1 vs $m2"
