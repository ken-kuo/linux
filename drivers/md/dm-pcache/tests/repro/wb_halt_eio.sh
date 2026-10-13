#!/bin/bash
# Writeback halt -> -EIO, not a hang. Standalone BIO cache with data_crc:
# keep writes in the cache, corrupt the cached data on the medium, let
# writeback run (it must stop on the data_crc mismatch), then keep writing
# past the cache size: the writer must get -EIO instead of hanging, and the
# device must still be removable.
#
# Usage (on the node): wb_halt_eio.sh [module]
KO=${1:-/root/dm-pcache.ko}
CDEV=/dev/pcache-cdev
SEG1_DATA=$(( 28*1024 + (1<<30) + 16*1024*1024 + 12*1024 ))
set -u
fail() { echo "FAIL: $*"; exit 1; }
st() { grep -hE "$1" /sys/kernel/debug/dm-pcache/*/stats; }

for d in $(dmsetup ls | grep -v live-root | cut -f1); do dmsetup remove $d; done
rmmod dm_pcache 2>/dev/null; insmod $KO || fail insmod
mount -t debugfs none /sys/kernel/debug 2>/dev/null
dmesg -C
dd if=/dev/zero of=$CDEV bs=1M count=80 oflag=direct status=none
truncate -s 4G /var/tmp/wbhalt-backing.img
B=$(losetup -j /var/tmp/wbhalt-backing.img | cut -d: -f1 | head -1)
[ -n "$B" ] || B=$(losetup -f --show /var/tmp/wbhalt-backing.img)
dmsetup create wbhalt --table "0 $(blockdev --getsz $B) pcache-dist $CDEV $B 4 cache_mode writeback data_crc true" || fail create
dmsetup message wbhalt 0 writeback_percent 90
echo "== write 8 MiB, kept in the cache"
dd if=/dev/urandom of=/dev/mapper/wbhalt bs=1M count=8 oflag=direct status=none || fail "first write"
sleep 2
echo "   $(st 'dirty_tail|key_head|cache_segs_used' | tr '\n' ' ')"
echo "== corrupt the cached data on the medium (segment 1 data area)"
# write the loop's backing file: the block device is held exclusively by dm,
# and loop (buffered) reads the same file page cache
IMG=$(losetup -l -n -O BACK-FILE $(readlink -f $CDEV))
python3 -c "
import os; fd=os.open('$IMG', os.O_WRONLY); os.lseek(fd, $SEG1_DATA, 0); os.write(fd, os.urandom(8<<20)); os.fsync(fd); os.close(fd)" || fail corrupt
echo "== let writeback run"
dmsetup message wbhalt 0 writeback_percent 0
sleep 5
dmesg | grep -m1 'writeback stopped' || fail "writeback did not report a stop"
echo "   $(st 'writeback_errors|cache_full' | tr '\n' ' ')"
echo "== write past the cache size; must end with an I/O error, not hang"
timeout 180 dd if=/dev/urandom of=/dev/mapper/wbhalt bs=1M count=3000 seek=16 oflag=direct status=none
rc=$?
echo "   dd rc=$rc ($( [ $rc = 124 ] && echo TIMEOUT-HANG || echo exited))"
echo "   $(st 'writeback_errors|cache_full|cache_segs_used|inflight_reqs' | tr '\n' ' ')"
[ $rc = 124 ] && fail "writer hung"
[ $rc = 0 ] && fail "writer did not see an error"
ps -eo stat,comm | awk '$1 ~ /D/' | grep . && fail "tasks in D state"
dmsetup remove wbhalt || fail "remove"
losetup -d $B; rm -f /var/tmp/wbhalt-backing.img; rmmod dm_pcache
echo "PASS: writeback stopped on the corrupt data, the writer got -EIO, device removed"
