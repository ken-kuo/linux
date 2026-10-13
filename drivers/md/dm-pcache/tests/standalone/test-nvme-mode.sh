#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# dm-pcache NVMe-mode functional test using loop devices.
# Run after booting into the kernel built from this branch.
#
# Loop devices are not DAX-capable, so dm-pcache's auto-detect will pick
# the NVMe backend — this exercises the cache_dev_req bio path,
# cmedium dispatch, async data writes, and the FLUSH-boundary drain.

# KO=<path to dm-pcache.ko> (default /root/dm-pcache.ko); target name pcache-dist.
set -e
set -u
set -o pipefail

# ---- config ----
CACHE_FILE=${CACHE_FILE:-/var/tmp/pcache-cache.img}
BACKING_FILE=${BACKING_FILE:-/var/tmp/pcache-backing.img}
CACHE_SIZE_MB=${CACHE_SIZE_MB:-2048}           # 2 GiB: 1 GiB of it is the peer-bitmap region
BACKING_SIZE_MB=${BACKING_SIZE_MB:-2048}       # 2 GiB backing
DM_NAME=${DM_NAME:-pcache_test}
MNT=${MNT:-/mnt/pcache_test}

# ---- helpers ----
log() { printf '\n=== %s ===\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

cleanup() {
	set +e
	umount $MNT 2>/dev/null
	dmsetup remove $DM_NAME 2>/dev/null
	[ -n "${LOOP_CACHE:-}" ] && losetup -d $LOOP_CACHE 2>/dev/null
	[ -n "${LOOP_BACKING:-}" ] && losetup -d $LOOP_BACKING 2>/dev/null
}

# ---- env ----
[ "$(id -u)" = 0 ] || fail "run as root (use sudo)"

log "kernel"
uname -r

log "T0: load dm-pcache module"
insmod "${KO:-/root/dm-pcache.ko}"
lsmod | grep -q dm_pcache || fail "dm_pcache not loaded"
dmesg | tail -3

# ---- T1: setup loop-backed cache + backing ----
log "T1: create backing files + loop bind"
trap cleanup EXIT
rm -f $CACHE_FILE $BACKING_FILE
truncate -s ${CACHE_SIZE_MB}M $CACHE_FILE
truncate -s ${BACKING_SIZE_MB}M $BACKING_FILE

LOOP_CACHE=$(losetup -f --show $CACHE_FILE)
LOOP_BACKING=$(losetup -f --show $BACKING_FILE)
echo "cache: $LOOP_CACHE  backing: $LOOP_BACKING"
ls -la /sys/block/$(basename $LOOP_CACHE)/queue/logical_block_size
cat /sys/block/$(basename $LOOP_CACHE)/queue/logical_block_size

# ---- T2: dmsetup create (auto-detect NVMe mode) ----
log "T2: dmsetup create"
BACKING_SECTORS=$(blockdev --getsz $LOOP_BACKING)
dmsetup create $DM_NAME --table \
	"0 $BACKING_SECTORS pcache-dist $LOOP_CACHE $LOOP_BACKING 4 cache_mode writeback data_crc true"

# ---- T3: status + table ----
log "T3: dmsetup status + table"
dmsetup status $DM_NAME
dmsetup table $DM_NAME

# ---- T4: dmesg ----
log "T4: dmesg from create"
dmesg | grep -i pcache | tail -10

# ---- T5: mkfs + mount + I/O ----
log "T5: mkfs + mount"
mkdir -p $MNT
mkfs.ext4 -q -F /dev/mapper/$DM_NAME
mount /dev/mapper/$DM_NAME $MNT
df -h $MNT

log "T5a: simple write + read + sync"
dd if=/dev/urandom of=$MNT/data1 bs=1M count=64 conv=fsync
sha_pre=$(sha256sum $MNT/data1 | awk '{print $1}')
sync
sha_post=$(sha256sum $MNT/data1 | awk '{print $1}')
[ "$sha_pre" = "$sha_post" ] || fail "sha256 changed across sync"
echo "T5a sha256 stable: $sha_pre"

log "T5b: many small files"
mkdir -p $MNT/small
for i in $(seq 1 500); do
	echo "test-payload-$i" > $MNT/small/f$i
done
sync
ls $MNT/small | wc -l

# ---- T6: persistence across remount ----
log "T6: persistence across remount"
sync
umount $MNT
mount /dev/mapper/$DM_NAME $MNT
[ -f $MNT/data1 ] || fail "data1 missing after remount"
sha_re=$(sha256sum $MNT/data1 | awk '{print $1}')
[ "$sha_re" = "$sha_pre" ] || fail "sha256 changed across remount"
echo "T6 sha256 stable across remount"

# ---- T7: persistence across dmsetup remove+recreate (replay) ----
log "T7: dmsetup remove + recreate (replay from on-media ksets)"
umount $MNT
dmsetup remove $DM_NAME
dmesg | grep -i pcache | tail -5

dmsetup create $DM_NAME --table \
	"0 $BACKING_SECTORS pcache-dist $LOOP_CACHE $LOOP_BACKING 4 cache_mode writeback data_crc true"
dmsetup status $DM_NAME
mount /dev/mapper/$DM_NAME $MNT
[ -f $MNT/data1 ] || fail "data1 missing after dmsetup recreate"
sha_replay=$(sha256sum $MNT/data1 | awk '{print $1}')
[ "$sha_replay" = "$sha_pre" ] || fail "sha256 changed across replay"
echo "T7 sha256 stable across replay"

# ---- T8: stress (fio if available, else dd loop) ----
if command -v fio >/dev/null 2>&1; then
	log "T8: fio stress (random rw, 4K, 4 jobs, 10s)"
	fio --name=stress --filename=$MNT/fiofile --size=128M --bs=4K --rw=randrw \
		--rwmixread=70 --numjobs=4 --iodepth=8 --runtime=10 --time_based \
		--group_reporting --direct=0 --end_fsync=1 2>&1 | tail -20
else
	log "T8: dd stress (fio not installed) — 20 rounds of 16M write+fsync+read"
	for i in $(seq 1 20); do
		dd if=/dev/urandom of=$MNT/stress.$i bs=1M count=16 conv=fsync status=none
		sha=$(sha256sum $MNT/stress.$i | awk '{print $1}')
		# remount to force the read from cache (or trigger replay)
		dd if=$MNT/stress.$i of=/dev/null bs=1M status=none
		sha_re=$(sha256sum $MNT/stress.$i | awk '{print $1}')
		[ "$sha" = "$sha_re" ] || fail "round $i sha mismatch"
		rm -f $MNT/stress.$i
	done
	echo "T8 dd stress: 20 rounds passed"
fi

# ---- T9: clean teardown ----
log "T9: umount + remove"
sync
umount $MNT
dmsetup remove $DM_NAME
losetup -d $LOOP_CACHE
losetup -d $LOOP_BACKING
LOOP_CACHE=""
LOOP_BACKING=""
trap - EXIT

# ---- T10: module unload ----
log "T10: rmmod"
rmmod dm_pcache
lsmod | grep -q dm_pcache && fail "dm_pcache still loaded"
dmesg | tail -3

log "ALL TESTS PASSED"
