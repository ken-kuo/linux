#!/bin/bash
# Setup the zero-cost backing device (null_blk, memory-backed so bcache
# can persist its SB on it) and a ramdisk used as the bcache cache.
# Run on both nodes before any stack setup.
#
# After this script:
#   /dev/nullb0 - memory-backed null block device (RAM-speed, persistent)
#   /dev/ram0   - ramdisk, used as the bcache cache device

set -euo pipefail

BACKING_SIZE_MB=${BACKING_SIZE_MB:-1024}
CACHE_SIZE_MB=${CACHE_SIZE_MB:-1024}

# null_blk:
#   memory_backed=1  -> data is stored (required for bcache backing SB)
#   queue_mode=2     -> blk-mq
#   irqmode=0        -> no IRQ delay
#   completion_nsec=0 -> no artificial latency
modprobe null_blk \
    nr_devices=1 \
    memory_backed=1 \
    queue_mode=2 \
    irqmode=0 \
    completion_nsec=0 \
    bs=4096 \
    gb=$((BACKING_SIZE_MB / 1024))

# brd for bcache cache:
modprobe brd rd_nr=1 rd_size=$((CACHE_SIZE_MB * 1024))

# Verify
echo "nullb0: $(blockdev --getsize64 /dev/nullb0) bytes"
echo "ram0:   $(blockdev --getsize64 /dev/ram0) bytes"
