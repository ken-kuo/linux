#!/bin/bash
# Bring up dm-pcache (distributed) on this node.
# Usage:
#   setup.sh <my_node_id> <is_primary 0|1> <peer_node_id> <peer_addr:port>
# Example (node-1, primary):
#   setup.sh 1 1 2 192.168.124.50:9876
# Example (node-2, secondary):
#   setup.sh 2 0 1 192.168.124.3:9876
#
# Assumes setup_backing.sh has already run (so /dev/nullb0 exists)
# and /dev/pmem0 exists.
#
# Env:
#   CACHE_DEV  cache device (default /dev/pmem0, DAX). A non-DAX block
#              device such as /dev/ram0 runs the BIO backend.
#   SOLO=1     standalone device: no network args, no peer (the peer
#              arguments are then ignored).

set -euo pipefail

MY_ID=$1
IS_PRIMARY=$2
PEER_ID=$3
PEER_ADDR=$4

PORT=${PORT_DDMPCACHE:-9876}
QUORUM=1
CACHE_DEV=${CACHE_DEV:-/dev/pmem0}
SOLO=${SOLO:-0}

# Zero the PMEM superblock so a stale on-media layout from a prior run
# does not break ctr.  We zero only the SB region (4 KiB at offset 0x1000).
python3 -c "
import os
fd = os.open('$CACHE_DEV', os.O_RDWR)
os.lseek(fd, 0x1000, os.SEEK_SET)
os.write(fd, b'\\x00' * 4096)
os.close(fd)
"

# Also zero the peer-registry region (right after cache_ctrl) and the
# bitmap headers, so a stale registry doesn't carry stale peer ids.
# Region offsets from cache_dev.h: registry at 8 KiB+ctrl, bitmap region
# follows.  Easiest: zero the first 32 MiB which covers SB + ctrl +
# registry + a few bitmap slot headers.
dd if=/dev/zero of=$CACHE_DEV bs=1M count=32 oflag=direct 2>/dev/null

insmod /root/dm-pcache.ko

# Size of the dm device: match nullb0 (the backing).
SZ_SECTORS=$(blockdev --getsz /dev/nullb0)

# Create the dm-pcache device.  Table format:
#   start len pcache-dist <pmem_dev> <backing_dev> [dev_args...]
if [ "$SOLO" = 1 ]; then
    dmsetup create pcache-bench --table \
        "0 $SZ_SECTORS pcache-dist $CACHE_DEV /dev/nullb0"
else
    dmsetup create pcache-bench --table \
        "0 $SZ_SECTORS pcache-dist $CACHE_DEV /dev/nullb0 \
         my_node_id=$MY_ID listen_port=$PORT is_primary=$IS_PRIMARY quorum=$QUORUM"

    # Tell our device about the peer.
    dmsetup message pcache-bench 0 add-peer "$PEER_ID" "$PEER_ADDR"
fi

# Show state
echo "--- dmsetup status ---"
dmsetup status pcache-bench
echo "--- dmesg tail ---"
dmesg | tail -20
