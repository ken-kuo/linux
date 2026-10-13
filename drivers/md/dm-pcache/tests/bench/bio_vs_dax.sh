#!/bin/bash
# DAX vs BIO cache backend, standalone and replicated, interleaved in one
# session (cross-session numbers on this rig are not comparable).
#
#   dax-solo   /dev/pmem0 (fsdax, tmpfs-backed nvdimm), no peer
#   bio-solo   /dev/ram0  (brd, memory speed, no DAX -> BIO backend), no peer
#   dax-repl   /dev/pmem0, node-1 primary -> node-2
#   bio-repl   /dev/ram0,  node-1 primary -> node-2
#
# Both cache devices are memory-backed, so the difference is the code path
# (DAX memcpy vs block I/O through cache_dev_req), not the medium. ram0 is
# 2560 MiB: the layout reserves 1 GiB for the peer bitmaps before the first
# segment.
#
# Usage: bio_vs_dax.sh [rounds]
set -euo pipefail
cd "$(dirname "$0")"
source ./common.sh

ROUNDS=${1:-2}
CONFIGS=${CONFIGS:-"dax-solo bio-solo dax-repl bio-repl"}
TS=$(date +%Y%m%d-%H%M%S)-bio-vs-dax
RES=results/$TS
mkdir -p "$RES"

deploy() {
	ssh_node "$1" 'mkdir -p /root/bench'
	rsync -az --delete --exclude='results/' ./ root@"$1":/root/bench/
	ssh_node "$1" 'find /root/bench -name "*.sh" -exec chmod +x {} \;'
}
both() {
	for ip in $NODE1_IP $NODE2_IP; do
		ssh_node "$ip" "$1" 2>&1 | sed "s/^/[$ip] /"
	done
}

deploy $NODE1_IP
deploy $NODE2_IP

for r in $(seq 1 "$ROUNDS"); do
	for cfg in $CONFIGS; do
		case $cfg in
		dax-*) cdev=/dev/pmem0 ;;
		bio-*) cdev=/dev/ram0 ;;
		esac
		log "===== round $r: $cfg ($cdev) ====="
		both "/root/bench/teardown_all.sh >/dev/null"
		both "CACHE_SIZE_MB=2560 /root/bench/setup_backing.sh >/dev/null"
		case $cfg in
		*-solo)
			ssh_node $NODE1_IP "CACHE_DEV=$cdev SOLO=1 /root/bench/ddmpcache/setup.sh 1 1 2 x >/dev/null"
			;;
		*-repl)
			ssh_node $NODE2_IP "CACHE_DEV=$cdev /root/bench/ddmpcache/setup.sh 2 0 1 $NODE1_IP:$PORT_DDMPCACHE >/dev/null"
			ssh_node $NODE1_IP "CACHE_DEV=$cdev /root/bench/ddmpcache/setup.sh 1 1 2 $NODE2_IP:$PORT_DDMPCACHE >/dev/null"
			;;
		esac
		log "settle 10s"; sleep 10
		# A fresh peer takes a full initial sync; numbers taken while it
		# runs measure the resync, not the write path (a run did: 39 IOPS
		# at 25 ms with bm_dirty=108096). Wait until it is done.
		case $cfg in
		*-repl)
			for i in $(seq 1 90); do
				dirty=$(ssh_node $NODE1_IP "dmsetup status pcache-bench" | grep -o 'bm_dirty=[0-9]*' | cut -d= -f2)
				[ "${dirty:-1}" = 0 ] && break
				sleep 2
			done
			log "initial sync done (bm_dirty=${dirty:-?}, waited $((i * 2))s)"
			;;
		esac
		ssh_node $NODE1_IP 'mount -t debugfs none /sys/kernel/debug 2>/dev/null; grep -h cache_dev_type /sys/kernel/debug/dm-pcache/*/stats; dmsetup status pcache-bench | cut -c1-120' |
			tee "$RES/$cfg-r$r.state"
		ssh_node $NODE1_IP "DEV=$DDMPCACHE_DEV OUTDIR=/tmp/r-$cfg-r$r RUNTIME=12 RAMP=3 /root/bench/fio/run_fio.sh"
		mkdir -p "$RES/$cfg-r$r"
		rsync -az root@$NODE1_IP:/tmp/r-$cfg-r$r/ "$RES/$cfg-r$r/"
		both "/root/bench/teardown_all.sh >/dev/null"
	done
done

log "===== summary ====="
python3 ./summarize_matrix.py "$RES" | tee "$RES/summary.md"
log "results in $RES"
