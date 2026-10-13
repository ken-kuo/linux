#!/bin/bash
# Replicated 128K sequential write: module x cache backend x cache size x
# (streams, qd), interleaved in one session. Built to answer two questions:
#   1. why the bio_vs_dax matrix showed DAX-repl at ~1660 MiB/s when the
#      documented replicated ceiling is ~2750 (streams=2, qd=32);
#   2. why BIO-repl had a lower p99 than DAX-repl there.
# While fio runs, both nodes sample their debugfs stats every second, so the
# share of time each side spent with a full cache is reported next to p99.
#
# Config name: <ko>-<dax|bioNNNN>-s<streams>q<qd>
#   ko      pre = /root/dm-pcache-pre-nvme.ko, int = /root/dm-pcache.ko
#   vdb     /dev/vdb, a virtio disk backed by a file in the host's /dev/shm
#           (hot-attached), BIO backend at memory speed without using the
#           guest's RAM -- use this one for BIO numbers
#   bioNNNN brd /dev/ram0 of NNNN MiB (BIO backend). brd lives in the VM's
#           RAM: next to null_blk it can OOM the secondary
#   s2q32   the ab_cut.sh shape: size=800M, 4 s warm-up
#   s1q8    the bio_vs_dax.sh w3 shape: size=256M, no warm-up
#   ...rNNNN  optional suffix: fio --rate=NNNNm (compare tails at equal throughput)
#
# Usage: repl_ab.sh [rounds]   (CONFIGS=... to override the list)
set -uo pipefail
cd "$(dirname "$0")"
source ./common.sh

ROUNDS=${1:-3}
CONFIGS=${CONFIGS:-"pre-dax-s2q32 int-dax-s2q32 pre-dax-s1q8 int-dax-s1q8 int-bio2048-s1q8 int-bio2560-s1q8"}
TS=$(date +%Y%m%d-%H%M%S)-repl-ab
RES=results/$TS
mkdir -p "$RES"
rsh() { command ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@"$1" "$2"; }

node_up() { # ip my_id is_primary peer_id peer_ip ko cdev streams ram_mb
	local ip=$1 me=$2 prim=$3 pid=$4 pip=$5 ko=$6 cdev=$7 st=$8 ram=$9
	rsh "$ip" "dmsetup remove pcache-bench 2>/dev/null; rmmod dm_pcache 2>/dev/null
		modprobe -r null_blk brd 2>/dev/null
		modprobe null_blk nr_devices=1 memory_backed=1 queue_mode=2 irqmode=0 completion_nsec=0 bs=4096 gb=2
		[ $ram -gt 0 ] && modprobe brd rd_nr=1 rd_size=$((ram * 1024))
		dd if=/dev/zero of=$cdev bs=1M count=32 oflag=direct status=none
		insmod /root/$ko || exit 1
		SZ=\$(blockdev --getsz /dev/nullb0)
		dmsetup create pcache-bench --table \"0 \$SZ pcache-dist $cdev /dev/nullb0 my_node_id=$me listen_port=$PORT_DDMPCACHE is_primary=$prim quorum=1 streams=$st\" || exit 1
		dmsetup message pcache-bench 0 add-peer $pid $pip:$PORT_DDMPCACHE"
}

for r in $(seq 1 "$ROUNDS"); do
	for cfg in $CONFIGS; do
		IFS=- read -r kname cache shape <<< "$cfg"
		case $kname in pre) ko=dm-pcache-pre-nvme.ko ;; int) ko=dm-pcache.ko ;; esac
		case $cache in
		dax) cdev=/dev/pmem0; ram=0 ;;
		vdb) cdev=/dev/vdb; ram=0 ;;	# host-tmpfs-backed virtio disk (BIO)
		bio*) cdev=/dev/ram0; ram=${cache#bio} ;;
		esac
		st=${shape#s}; st=${st%%q*}; qd=${shape#*q}; qd=${qd%%r*}
		rate=; case $shape in *r*) rate="--rate=${shape##*r}m" ;; esac
		case $shape in
		s2q32) size=800M; warm=1 ;;
		*) size=256M; warm=0 ;;
		esac
		log "== round $r: $cfg ($ko $cdev streams=$st qd=$qd size=$size)"
		node_up $NODE2_IP 2 0 1 $NODE1_IP $ko $cdev $st $ram || { log "!! setup failed on node-2"; continue; }
		node_up $NODE1_IP 1 1 2 $NODE2_IP $ko $cdev $st $ram || { log "!! setup failed on node-1"; continue; }
		want=$(rsh $NODE1_IP "modinfo -F srcversion /root/$ko")
		for ip in $NODE1_IP $NODE2_IP; do
			got=$(rsh $ip 'cat /sys/module/dm_pcache/srcversion')
			[ "$got" = "$want" ] || { log "!! $ip loaded $got, want $want"; exit 1; }
		done
		sleep 9
		for i in $(seq 1 90); do
			dirty=$(rsh $NODE1_IP "dmsetup status pcache-bench" | grep -o 'bm_dirty=[0-9]*' | cut -d= -f2)
			[ "${dirty:-1}" = 0 ] && break
			sleep 2
		done
		[ $warm = 1 ] && rsh $NODE1_IP "fio --name=w --filename=$DDMPCACHE_DEV --rw=write --bs=128k --iodepth=8 --ioengine=libaio --direct=1 --size=512M --runtime=4 --time_based >/dev/null 2>&1"
		for ip in $NODE1_IP $NODE2_IP; do
			rsh $ip 'mount -t debugfs none /sys/kernel/debug 2>/dev/null
				cat > /tmp/samp.sh <<"S"
echo $$ > /tmp/samp.pid
while :; do
	grep -hE "^(cache_full|cache_segs_used|inflight_reqs):" /sys/kernel/debug/dm-pcache/*/stats | tr "\n" " "
	echo
	sleep 1
done
S
				setsid nohup bash /tmp/samp.sh > /tmp/samp 2>&1 < /dev/null &'
		done
		rsh $NODE1_IP "fio --name=w3 --filename=$DDMPCACHE_DEV --ioengine=libaio --direct=1 --time_based=1 --ramp_time=3 --runtime=12 --norandommap=1 --randrepeat=0 --group_reporting=1 --continue_on_error=write --rw=write --bs=128k --iodepth=$qd --numjobs=1 --size=$size $rate --output-format=json" > "$RES/$cfg-r$r.json"
		for n in 1 2; do
			ip=$NODE1_IP; [ $n = 2 ] && ip=$NODE2_IP
			rsh $ip 'kill $(cat /tmp/samp.pid) 2>/dev/null; cat /tmp/samp' > "$RES/$cfg-r$r.node$n.samp"
		done
		python3 - "$RES/$cfg-r$r" <<'EOF'
import json, sys
p = sys.argv[1]
w = json.load(open(p + '.json'))['jobs'][0]['write']
pc = w['clat_ns']['percentile']
full = []
for n in (1, 2):
    lines = [l for l in open(f'{p}.node{n}.samp') if 'cache_full' in l]
    f = sum(1 for l in lines if 'cache_full: 1' in l)
    full.append(f'{f}/{len(lines)}')
print(f"   bw={w['bw']/1024:.0f} MiB/s  p50={pc['50.000000']/1e3:.0f}us p99={pc['99.000000']/1e3:.0f}us "
      f"p99.9={pc['99.900000']/1e3:.0f}us  cache_full samples: node1 {full[0]} node2 {full[1]}")
EOF
	done
done | tee "$RES/run.txt"
rsh $NODE1_IP 'dmsetup remove pcache-bench; rmmod dm_pcache' ; rsh $NODE2_IP 'dmsetup remove pcache-bench; rmmod dm_pcache'
log "results in $RES"
