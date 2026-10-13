#!/bin/bash
# Standalone BIO writeback-stall probe: no replication, data_crc off.
# Long random writes so the key log wraps and GC cycles; flags a stall when
# the cache is full and dirty_tail has not moved for STALL_S seconds.
#
# Usage: bio_wb_stall.sh <node> <logfile> [seconds] [module]
N=${1:?node}; LOG=${2:?log}; SECS=${3:-600}; KO=${4:-/root/dm-pcache.ko}
STALL_S=${STALL_S:-60}
SSH="ssh -o ConnectTimeout=5 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 root@$N"
exec > >(tee -a "$LOG") 2>&1

$SSH "set -e
	for d in \$(dmsetup ls | grep -v live-root | cut -f1); do dmsetup remove \$d; done
	rmmod dm_pcache 2>/dev/null || true
	insmod $KO
	mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
	dd if=/dev/zero of=/dev/pcache-cdev bs=1M count=80 oflag=direct status=none
	truncate -s 1G /var/tmp/wbstall-backing.img
	B=\$(losetup -j /var/tmp/wbstall-backing.img | cut -d: -f1 | head -1)
	[ -n \"\$B\" ] || B=\$(losetup -f --show /var/tmp/wbstall-backing.img)
	dmsetup create wbstall --table \"0 \$(blockdev --getsz \$B) pcache-dist /dev/pcache-cdev \$B 4 cache_mode writeback data_crc false\"
	grep cache_dev_type /sys/kernel/debug/dm-pcache/*/stats" || exit 1

$SSH "nohup fio --name=wb --filename=/dev/mapper/wbstall --rw=randwrite --bs=4k --direct=1 \
	--ioengine=libaio --iodepth=16 --numjobs=4 --time_based --runtime=$SECS --group_reporting \
	> /var/tmp/wbstall-fio.log 2>&1 &"

last=""; same=0; t=0
while [ $t -lt $((SECS + 30)) ]; do
	sleep 10; t=$((t + 10))
	st=$($SSH 'grep -hE "cache_full|cache_segs_used|key_head|dirty_tail|key_tail|inflight_reqs" /sys/kernel/debug/dm-pcache/*/stats | tr "\n" " "')
	echo "$(date +%T) $st"
	dt=$(echo "$st" | grep -o 'dirty_tail: [0-9:]*')
	if echo "$st" | grep -q 'cache_full: 1' && [ "$dt" = "$last" ]; then
		same=$((same + 10))
		if [ $same -ge $STALL_S ]; then echo "!!!! STALL: cache full, $dt unchanged for ${same}s"; exit 2; fi
	else
		same=0
	fi
	last=$dt
	$SSH 'pgrep -x fio >/dev/null' || break
done
$SSH 'tail -5 /var/tmp/wbstall-fio.log; dmsetup remove wbstall; rmmod dm_pcache'
echo "== no stall in ${SECS}s"
