#!/bin/bash
# ---------------------------------------------------------------------------
# stray_packet.sh <ko-name> <fault_types> -- fix-history #22.
#
#   bash tests/repro/stray_packet.sh dm-pcache-p22-after.ko 4    # late P_BITMAP
#   bash tests/repro/stray_packet.sh dm-pcache-p22-after.ko 8    # undefined type
#
# Injects an unexpected packet into an established control stream (the
# PCACHE_FAULT_STRAY_* sites) and reports what the peer's receive loop did with
# it. The real race that produced #22 fired twice in an entire regression run,
# which is no way to verify a fix.
#
# Expected on a fixed build:
#   types=4  late BITMAP discarded, connection survives, no misframing
#   types=8  protocol error, connection dropped and recovered, no misframing
# On an unfixed build both show "unknown packet type" -- the stream misframed.
# ---------------------------------------------------------------------------
set -uo pipefail
cd "$(dirname "$0")/../bench"
source ./common.sh
KO=${1:?usage: stray_packet.sh <ko-name> <fault_types>}
TYPES=${2:-4}
RATE=${RATE:-1}
SECS=${SECS:-20}
rsh() { timeout 120 ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 root@"$1" "$2"; }
die() { echo "ABORT: $*"; exit 1; }
cnt() { rsh "$1" "dmesg | grep -c \"$2\" || true"; }

for ip in $NODE1_IP $NODE2_IP; do
    rsh "$ip" true >/dev/null 2>&1 || die "$ip unreachable"
    rsh "$ip" "test -f /root/$KO" || die "$ip has no /root/$KO"
done
WANT=$(rsh $NODE1_IP "modinfo -F srcversion /root/$KO")
echo "== $KO ($WANT), fault_types=$TYPES rate=$RATE% for ${SECS}s"

for ip in $NODE1_IP $NODE2_IP; do
    rsh "$ip" 'pkill -9 -x dd 2>/dev/null
               for d in $(dmsetup ls 2>/dev/null | awk "/pcache/{print \$1}"); do dmsetup remove -f "$d" 2>/dev/null; done
               rm -f /dev/mapper/pcache-bench
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

echo "== write load + injection"
rsh $NODE1_IP "(timeout $((SECS + 10)) dd if=/dev/urandom of=$DDMPCACHE_DEV bs=64k count=100000 oflag=direct >/tmp/dd.log 2>&1 &) ; sleep 2
               echo $RATE > /sys/module/dm_pcache/parameters/fault_rate
               echo $TYPES > /sys/module/dm_pcache/parameters/fault_types
               sleep $SECS
               echo 0 > /sys/module/dm_pcache/parameters/fault_rate
               echo 0 > /sys/module/dm_pcache/parameters/fault_types
               echo injected=\$(cat /sys/module/dm_pcache/parameters/fault_count)"
sleep 12

echo "== verdict"
for ip in $NODE1_IP $NODE2_IP; do
    # A misframe is an unexpected-type line that is NOT one of the new
    # protocol-error drops -- otherwise the fixed build's own log lines are
    # counted as the damage they exist to prevent.
    mis=$(rsh "$ip" "dmesg | grep -E 'unknown packet type|unexpected packet type|unexpected pkt type' | grep -vc 'protocol error' || true")
    echo "   $ip: misframed=$mis" \
         "discarded=$(cnt "$ip" 'after the phase closed')" \
         "proto_err=$(cnt "$ip" 'protocol error')"
done
echo "   final repl_state: $(rsh $NODE1_IP 'dmesg | grep -oE "repl_state [A-Z_]+ -> [A-Z_]+" | tail -1')"
echo "   writer: $(rsh $NODE1_IP 'tail -1 /tmp/dd.log 2>/dev/null')"
for i in $(seq 1 30); do
    d=$(rsh $NODE1_IP 'dmsetup status pcache-bench 2>/dev/null | grep -oE "bm_dirty=[0-9]+" | head -1')
    [ "$d" = "bm_dirty=0" ] && break
    sleep 5
done
echo "   dirty after settle: ${d:-unknown}"
m1=$(rsh $NODE1_IP "dd if=$DDMPCACHE_DEV bs=1M count=64 iflag=direct status=none | md5sum | cut -d' ' -f1")
m2=$(rsh $NODE2_IP "dd if=$DDMPCACHE_DEV bs=1M count=64 iflag=direct status=none | md5sum | cut -d' ' -f1")
echo "   node1=$m1"
echo "   node2=$m2"
[ "$m1" = "$m2" ] && echo "   DATA MATCHES" || echo "   DATA DIVERGED"
