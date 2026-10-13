#!/bin/bash
# Drop every stack and every backing device.  Idempotent.
# Run on each node between stack switches and at end of run.

set -u

# DRBD: must be down before unloading
drbdadm down bench 2>/dev/null || true
sleep 1

# bcache: stop instances
for d in /sys/block/bcache*; do
    [ -e "$d/bcache/stop" ] && echo 1 > "$d/bcache/stop" 2>/dev/null || true
done
for c in /sys/fs/bcache/*-*; do
    [ -e "$c/stop" ] && echo 1 > "$c/stop" 2>/dev/null || true
done
sleep 1

# dm-pcache
dmsetup remove pcache-bench 2>/dev/null || true
dmsetup remove pcache-dist0 2>/dev/null || true
# udev sometimes leaves /dev/mapper/* as a regular file (e.g. an earlier
# `dd of=/dev/mapper/pcache-bench` ran while the device node wasn't created
# yet). Strip those so the next dmsetup create gets a proper symlink.
rm -f /dev/mapper/pcache-bench /dev/mapper/pcache-dist0

# A prior failed run can leave a *regular file* at /dev/drbd0: if the DRBD
# stack didn't come up, `fio --filename=/dev/drbd0` creates a plain file there,
# which then shadows the real DRBD block device (147:0) on every later run and
# silently reports garbage bandwidth. Strip it if it isn't a block device.
for n in /dev/drbd0 /dev/drbd1; do
    [ -e "$n" ] && [ ! -b "$n" ] && rm -f "$n"
done

# unload modules (ignore errors if not loaded / still in use)
rmmod drbd_transport_tcp 2>/dev/null || true
rmmod drbd 2>/dev/null || true
rmmod bcache 2>/dev/null || true
rmmod dm_pcache 2>/dev/null || true
rmmod null_blk 2>/dev/null || true
rmmod brd 2>/dev/null || true

# Sanity print
lsmod | grep -E "dm_pcache|drbd|bcache|null_blk|brd" || echo "  all stack modules unloaded"
dmsetup ls | grep -v "^No devices" || true
