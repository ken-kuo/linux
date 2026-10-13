#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Minimum-viable dm-pcache test (BIO cache on a loop device).
# KO=<path to dm-pcache.ko> (default /root/dm-pcache.ko); target name pcache-dist.
# Each step has a hard timeout so we get
# diagnostic signal even if a kernel path wedges. Run as root.

set -u
LOG=/var/tmp/pcache-mini.log
: > $LOG
exec > >(tee -a $LOG) 2>&1

T() {  # T <seconds> <description> <command...>
	local secs=$1; shift
	local desc=$1; shift
	echo
	echo "=== $(date +%T) [$secs s] $desc ==="
	echo "+ $*"
	timeout --kill-after=2 $secs "$@"
	local rc=$?
	if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		echo "FAIL: $desc — TIMEOUT after ${secs}s"
		dump_state
		exit 1
	elif [ $rc -ne 0 ]; then
		echo "FAIL: $desc — exit $rc"
		dump_state
		exit $rc
	fi
}

dump_state() {
	echo
	echo "--- state at failure ---"
	echo "dmsetup ls:"; dmsetup ls 2>&1
	echo "dmsetup status:"; dmsetup status 2>&1
	echo "losetup:"; losetup 2>&1
	echo "/dev/loop* inflight:"
	for d in /sys/block/loop*; do
	  [ -f $d/inflight ] && echo "  $(basename $d): $(cat $d/inflight)"
	done
	echo "D-state tasks:"
	for p in $(ps -eL -o pid,stat | awk '$2~/D/{print $1}' | sort -u); do
	  echo "  PID $p: $(ps -p $p -o comm= 2>/dev/null)"
	  cat /proc/$p/stack 2>/dev/null | head -8 | sed 's/^/    /'
	done
	echo "recent dmesg:"; dmesg | tail -20
}

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

T 5  "kernel"           uname -r
T 10 "free mem before"  free -m

# ---- disable zram swap so we have headroom ----
if [ -e /dev/zram0 ]; then
	T 20 "swapoff zram"     swapoff /dev/zram0 || true
	T 5  "free mem after zram off" free -m
fi

# ---- clean any prior state ----
dmsetup remove pcache_mini 2>/dev/null || true
for l in $(losetup -j /var/tmp/pcache-mini-cache.img | cut -d: -f1) \
         $(losetup -j /var/tmp/pcache-mini-backing.img | cut -d: -f1); do
	losetup -d $l 2>/dev/null || true
done
rmmod dm_pcache 2>/dev/null || true

T 5  "load module"      insmod "${KO:-/root/dm-pcache.ko}"

# ---- create backing files (smallest viable) ----
CACHE_FILE=/var/tmp/pcache-mini-cache.img
BACKING_FILE=/var/tmp/pcache-mini-backing.img
T 10 "make cache file (2G)"    truncate -s 2G $CACHE_FILE
T 10 "make backing file (1G)"    truncate -s 1G $BACKING_FILE
T 10 "losetup cache"             losetup -f --show $CACHE_FILE
LOOP_CACHE=$(losetup -j $CACHE_FILE | cut -d: -f1)
T 10 "losetup backing"           losetup -f --show $BACKING_FILE
LOOP_BACKING=$(losetup -j $BACKING_FILE | cut -d: -f1)
echo "cache=$LOOP_CACHE  backing=$LOOP_BACKING"

BACKING_SECTORS=$(blockdev --getsz $LOOP_BACKING)
T 30 "dmsetup create" dmsetup create pcache_mini --table \
	"0 $BACKING_SECTORS pcache-dist $LOOP_CACHE $LOOP_BACKING 4 cache_mode writeback data_crc true"

T 5  "dmsetup status"  dmsetup status pcache_mini

# ---- minimal raw I/O ----
T 30 "dd write 1MB"    dd if=/dev/urandom of=/dev/mapper/pcache_mini bs=1M count=1 conv=fsync oflag=direct
T 30 "blockdev flush"  blockdev --flushbufs /dev/mapper/pcache_mini
T 30 "dd read 1MB"     dd if=/dev/mapper/pcache_mini of=/dev/null bs=1M count=1 iflag=direct

T 5  "dmsetup status post-I/O" dmsetup status pcache_mini

# ---- teardown ----
# udev scans the new dm device on creation/IO and holds it open briefly;
# settle before remove so dmsetup doesn't hit EBUSY.
T 30 "udev settle"     udevadm settle
T 30 "dmsetup remove"  dmsetup remove --retry pcache_mini
T 10 "losetup -d cache" losetup -d $LOOP_CACHE
T 10 "losetup -d backing" losetup -d $LOOP_BACKING
T 10 "rmmod"           rmmod dm_pcache

echo
echo "=== ALL STEPS PASSED ==="
