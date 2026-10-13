# Shared knobs and helpers for the dm-pcache vs DRBD+bcache benchmark.
# Sourced by every other script in this directory.

NODE1_IP=192.168.124.3
NODE2_IP=192.168.124.50
NODE1_HOST=fedora-clone
NODE2_HOST=fedora-clone-2

# Sizes (in MiB unless suffix).
BACKING_SIZE_MB=4096      # /dev/nullb0 on each node
CACHE_SIZE_MB=2048        # /dev/ram0 on each node (bcache cache); pmem0 is already 2 GiB
PORT_DDMPCACHE=9876
PORT_DRBD=7788

DDMPCACHE_DEV=/dev/mapper/pcache-bench
DRBD_DEV=/dev/drbd0

# Helper: run a command on a node via ssh, no host-key prompt.
ssh_node() {
    local ip=$1; shift
    ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@"$ip" "$@"
}

log() {
    echo "[$(date +%H:%M:%S)] $*" >&2
}
