#!/bin/bash
#
# dm_pcache_phase1_demo.sh
#
# End-to-end test helper for pcache-dist (cluster setup, reset and basic tests).
# Validates: connection establishment + write replication across two VMs.
#
# Prerequisites (run on host):
#   1. Build the module on host:
#        cd <kernel tree>
#        make M=drivers/md/dm-pcache modules
#
#   2. Ensure both VMs have the same PMEM device (/dev/pmem0) and
#      a loop backing device of identical size.
#
# Usage:
#   ./dm_pcache_phase1_demo.sh <build|deploy|start|stop|test|cleanup|all>
#
#   ./dm_pcache_phase1_demo.sh all      # full pipeline: build -> deploy -> start -> test
#   ./dm_pcache_phase1_demo.sh build    # compile dm-pcache.ko on host
#   ./dm_pcache_phase1_demo.sh deploy   # copy .ko + deps to VMs
#   ./dm_pcache_phase1_demo.sh start   # load module + create dmsetup devices on both VMs
#   ./dm_pcache_phase1_demo.sh test     # run I/O test + verify cross-node consistency
#   ./dm_pcache_phase1_demo.sh stop     # remove dmsetup + unload module on both VMs
#   ./dm_pcache_phase1_demo.sh cleanup  # full cleanup: stop + remove backing files
#

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Configuration
# ─────────────────────────────────────────────────────────────────────────────

HOST_KERNEL="${HOST_KERNEL:-$(cd "$(dirname "$0")/../../../.." && pwd)}"  # this tree
MODULE_SRC="${HOST_KERNEL}/drivers/md/dm-pcache/dm-pcache.ko"

NODE1="192.168.124.3"   # Primary  (my_node_id=0)
NODE2="192.168.124.50"  # Secondary (my_node_id=1)
NODE3="192.168.124.60"  # Secondary (my_node_id=2)
SSH_USER="root"
SSH_OPTS="-o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5"

BACKING_FILE="/var/lib/pcache_backing.img"
BACKING_SIZE="512M"      # must match on both nodes

PMEM_DEV="/dev/pmem0"   # persistent memory device used as cache storage
CACHE_SIZE_MB="256"      # cache device size in MB (must be >= 512MB for dm-pcache)
DMSETUP_DEV="pcache-dist0"
DMSETUP_TABLE="pcache-dist"

# TCP port the Secondary node listens on for Primary connections
LISTEN_PORT="9764"

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

log()  { echo "[$(date '+%H:%M:%S')] $*"; }
info() { echo "[$(date '+%H:%M:%S')] [INFO]  $*"; }
warn() { echo "[$(date '+%H:%M:%S')] [WARN]  $*" >&2; }
err()  { echo "[$(date '+%H:%M:%S')] [ERROR] $*" >&2; exit 1; }

ssh_node() {
    local node="$1"; shift
    ssh $SSH_OPTS "${SSH_USER}@${node}" "$@"
}

# One cheap probe instead of letting every ssh in a loop time out separately.
# A powered-off node costs ConnectTimeout per call, and the cleanup/tuning
# passes make many calls each -- a single absent node stalled a demo's reset
# for tens of minutes. Cleaning a node that is down is a no-op anyway (it comes
# up clean), and demos that genuinely need all three still refuse to run via
# require_nodes in demo_features.sh.
node_reachable() {
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=3 \
        "${SSH_USER}@$1" true >/dev/null 2>&1
}

scp_node() {
    local src="$1"; local dst="$2"; local node="$3"
    scp $SSH_OPTS "$src" "${SSH_USER}@${node}:${dst}"
}

# ─────────────────────────────────────────────────────────────────────────────
# Cleanup helpers
# ─────────────────────────────────────────────────────────────────────────────

cleanup_node() {
    if ! node_reachable "$1"; then
        warn "cleanup: $1 unreachable, skipping (it comes up clean)"
        return 0
    fi
    local node="$1"
    echo "  [CLEANUP] $node..."
    ssh_node "$node" bash -s <<'INNER'
set -e
DMSETUP_DEV="pcache-dist0"
BACKING_FILE="/var/lib/pcache_backing.img"

dmsetup remove "$DMSETUP_DEV" 2>/dev/null || true
rm -f "/dev/mapper/$DMSETUP_DEV"   # udev stale-file workaround
rmmod dm_pcache 2>/dev/null || true

for lp in $(losetup -j "$BACKING_FILE" 2>/dev/null | cut -d: -f1); do
    losetup -d "$lp" 2>/dev/null || true
done
rm -f "$BACKING_FILE"

if [ -b /dev/pmem0 ]; then
    # selective wipe: SB+cache_info+cache_ctrl+registry (24 KiB)
    # + the 8 KiB header at the start of EVERY bitmap slot (so all 32 slots
    # appear fresh). Full bitmap data region kept untouched for speed.
    python3 -c "
import os
fd = os.open('/dev/pmem0', os.O_RDWR)
# SB(0x1000) + cache_info(0x2000-0x4000) + cache_ctrl(0x4000-0x5000)
# + registry(0x5000-0x7000) — contiguous 24 KiB
os.lseek(fd, 0x1000, os.SEEK_SET)
os.write(fd, b'\\x00' * 0x6000)
# Zero each of 32 bitmap slot headers (8 KiB at slot_offset 0)
BITMAP_OFF = 0x7000
SLOT_SIZE  = 32 * 1024 * 1024
HDR_SIZE   = 8 * 1024
for i in range(32):
    os.lseek(fd, BITMAP_OFF + i * SLOT_SIZE, os.SEEK_SET)
    os.write(fd, b'\\x00' * HDR_SIZE)
os.close(fd)
" 2>/dev/null || true
fi
echo "  [CLEANUP] $node: done"
INNER
}

cleanup_both_nodes() {
    for node in "$NODE1" "$NODE2"; do
        cleanup_node "$node"
    done
}

# Crash recovery test.
# Verifies PCACHE_BM_F_RESYNC_IN_PROGRESS makes ctr load detect that the
# previous resync was interrupted and force a fresh FULL_SYNC.
#
# Flow:
#   1. cmd_reset + cmd_start
#   2. Trigger resync on Primary (force-resync — sets FULL_SYNC + drops sock)
#   3. While resync is in flight, sysrq+c on Primary
#   4. Wait, then virsh destroy/start to recover
#   5. After SSH back, reload module + check dmesg for "previous resync was
#      interrupted; forcing FULL_SYNC"
#
# WARNING: step 3 hard-crashes NODE1. Step 4 needs sudo for virsh.
cmd_test_crash() {
    log "=== Crash recovery test (sysrq+c) ==="

    cmd_reset
    cmd_start
    sleep 8

    log "[1/5] Primary writes 4 MiB + force-resync (sets FULL_SYNC + RESYNC_IN_PROGRESS)..."
    ssh_node "$NODE1" "
        dd if=/dev/urandom of=/dev/mapper/$DMSETUP_DEV bs=1M count=4 oflag=direct conv=fsync 2>&1 | tail -1
        dmsetup message $DMSETUP_DEV 0 force-resync
    " || true
    sleep 2

    log "[2/5] sysrq+c on $NODE1 (controlled hard crash)..."
    ssh_node "$NODE1" 'echo c > /proc/sysrq-trigger' 2>&1 || true

    log "[3/5] Waiting 15 s for node to die..."
    sleep 15

    if ssh -o ConnectTimeout=3 -o BatchMode=yes "$SSH_USER@$NODE1" 'true' 2>/dev/null; then
        warn "[!] $NODE1 still responding after sysrq+c — VM may have just rebooted itself"
    else
        log "[4/5] $NODE1 unresponsive; running virsh destroy+start..."
        ${SUDO:-sudo} virsh destroy fedora-clone 2>&1 | tail -1
        ${SUDO:-sudo} virsh start fedora-clone 2>&1 | tail -1
    fi

    log "  Waiting up to 90 s for SSH on $NODE1..."
    local i
    for i in $(seq 1 30); do
        if ssh -o ConnectTimeout=3 -o BatchMode=yes "$SSH_USER@$NODE1" 'echo back' 2>/dev/null | grep -q back; then
            log "  $NODE1 SSH back at attempt $i"
            break
        fi
        sleep 3
    done

    log "[5/5] Reload module on $NODE1 + look for RESYNC_IN_PROGRESS recovery log..."
    ssh_node "$NODE1" "
        insmod /root/dm-pcache.ko 2>/dev/null || true
        # Re-create the device with same table so peer_bitmap_init runs
        LOOP=\$(losetup -f --show /var/lib/pcache_backing.img 2>/dev/null || \\
               losetup -j /var/lib/pcache_backing.img 2>/dev/null | cut -d: -f1 | head -1)
        SIZE=\$(blockdev --getsz \$LOOP)
        dmsetup create $DMSETUP_DEV --table \"0 \$SIZE pcache-dist $PMEM_DEV \$LOOP my_node_id=0 is_primary=1 listen_port=$LISTEN_PORT\" 2>&1
        dmsetup message $DMSETUP_DEV 0 add-peer 1 $NODE2:$LISTEN_PORT 2>&1
        sleep 3
        echo '--- dmesg looking for RESYNC_IN_PROGRESS recovery ---'
        dmesg | grep -E 'previous resync was interrupted|FULL_SYNC|bitmap\\[slot' | tail -10
    " || warn "(test reload step failed; inspect $NODE1 manually)"

    log "Crash recovery test complete."
}

# 3-node mesh stress test.
# Light variant: ~10 minutes of continuous write + churn, periodic MD5 check.
#
# Iterations every ~30 s:
#   • Primary writes random 1-8 MiB at random offset
#   • Every 3rd: dmsetup message remove-peer 2 + add-peer 2 (re-mesh node-3)
#   • Every 6th: MD5 across all 3 nodes — fail-fast if mismatch
cmd_stress() {
    local total_iters="${1:-20}"   # default 20 iter ≈ 10 min
    log "=== 3-node mesh stress test ($total_iters iters) ==="

    cmd_reset
    cmd_start3
    sleep 10

    local fail=0
    local i
    for i in $(seq 1 "$total_iters"); do
        local seek size
        seek=$((RANDOM % 480))
        size=$((1 + RANDOM % 8))
        log "[iter $i/$total_iters] write ${size} MiB at offset ${seek} MiB on Primary"
        ssh_node "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DMSETUP_DEV bs=1M count=$size seek=$seek oflag=direct conv=fsync status=none" || warn "write failed"

        # Every 3rd iter: churn node-3 (remove + re-add)
        if [ $((i % 3)) -eq 0 ]; then
            log "  churn: remove-peer 2 + re-add"
            ssh_node "$NODE1" "dmsetup message $DMSETUP_DEV 0 remove-peer 2" 2>&1 | tail -1 || true
            sleep 2
            ssh_node "$NODE1" "dmsetup message $DMSETUP_DEV 0 add-peer 2 $NODE3:$LISTEN_PORT" 2>&1 | tail -1 || true
            sleep 3
        fi

        # Every 6th iter: full MD5 check across all 3 nodes (first 4 MiB)
        if [ $((i % 6)) -eq 0 ]; then
            log "  [MD5 check] iter $i"
            sleep 3   # let replication settle
            local m1 m2 m3
            m1=$(ssh_node "$NODE1" "dd if=/dev/mapper/$DMSETUP_DEV bs=1M count=4 iflag=direct status=none | md5sum | awk '{print \$1}'")
            m2=$(ssh_node "$NODE2" "dd if=/dev/mapper/$DMSETUP_DEV bs=1M count=4 iflag=direct status=none | md5sum | awk '{print \$1}'")
            m3=$(ssh_node "$NODE3" "dd if=/dev/mapper/$DMSETUP_DEV bs=1M count=4 iflag=direct status=none | md5sum | awk '{print \$1}'")
            if [ "$m1" = "$m2" ] && [ "$m2" = "$m3" ]; then
                log "    MATCH: $m1"
            else
                warn "    MISMATCH: n1=$m1  n2=$m2  n3=$m3"
                fail=$((fail + 1))
            fi
        fi
        sleep 1
    done

    log "=== Stress test done: $fail MD5 mismatch(es) over $total_iters iters ==="
    log "  final bitmap-status on each node:"
    for n in "$NODE1" "$NODE2" "$NODE3"; do
        echo "  $n:"
        ssh_node "$n" "dmsetup status $DMSETUP_DEV | tail -1" || true
    done
    [ $fail -eq 0 ] && log "STRESS PASS" || warn "STRESS FAIL ($fail mismatch)"
}

# top-level dmsetup-message-style reset (3-node aware)
cmd_reset() {
    log "=== Full reset on all 3 VMs (stop + wipe PMEM) ==="
    for n in "$NODE1" "$NODE2" "$NODE3"; do
        cleanup_node "$n"
    done
    log "Reset complete."
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Build (run on host)
# ─────────────────────────────────────────────────────────────────────────────

cmd_build() {
    log "=== Step 1: Build dm-pcache.ko on host ==="

    if [ ! -d "$HOST_KERNEL" ]; then
        err "Kernel source not found at $HOST_KERNEL"
    fi

    cd "$HOST_KERNEL"
    log "Compiling dm-pcache module..."
    make M=drivers/md/dm-pcache modules -j$(nproc)

    if [ ! -f "$MODULE_SRC" ]; then
        err "Build failed: $MODULE_SRC not found"
    fi

    # Verify vermagic matches the VM kernel (7.0.0+)
    local vermagic
    vermagic=$(modinfo "$MODULE_SRC" 2>/dev/null | grep vermagic | awk '{print $2}')
    log "Module vermagic: $vermagic"
    if ! echo "$vermagic" | grep -q "7.0.0"; then
        warn "Vermagic does not contain '7.0.0' — module may not load on VM"
    fi

    log "Build complete: $MODULE_SRC"
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: Deploy (run on host — push to VMs)
# ─────────────────────────────────────────────────────────────────────────────

cmd_deploy() {
    log "=== Step 2: Deploy dm-pcache.ko to both VMs ==="

    if [ ! -f "$MODULE_SRC" ]; then
        err "Module not built. Run '$0 build' first."
    fi

    for node in "$NODE1" "$NODE2"; do
        log "Deploying to $node..."
        scp_node "$MODULE_SRC" "/root/dm-pcache.ko" "$node"

        # Verify kernel version matches
        local kver
        kver=$(ssh_node "$node" "uname -r")
        info "  $node kernel: $kver"
    done

    log "Deployment complete."
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 3: Start (run on VMs — load module + create devices)
# ─────────────────────────────────────────────────────────────────────────────

# Internal: prepare backing store on ONE node (creates loop + file)
prepare_backing() {
    local node="$1"
    ssh_node "$node" bash <<'REMOTE'
set -e

BACKING_FILE="/var/lib/pcache_backing.img"
BACKING_SIZE="512M"

# Create backing file if missing
if [ ! -f "$BACKING_FILE" ]; then
    echo "[DEBUG] Creating backing file..."
    truncate -s "$BACKING_SIZE" "$BACKING_FILE"
    chmod 600 "$BACKING_FILE"
fi

# Check if loop-control exists; load loop module if needed
if [ ! -e /dev/loop-control ]; then
    echo "[DEBUG] /dev/loop-control not found — loading loop module..."
    modprobe loop || {
        echo "[ERROR] Failed to load loop module"
        exit 1
    }
    sleep 1
fi
if [ ! -e /dev/loop-control ]; then
    echo "[ERROR] /dev/loop-control still not found after modprobe"
    echo "[DEBUG] /dev/loop* contents: $(ls -la /dev/loop* 2>/dev/null)"
    exit 1
fi

# Find existing loop or attach
existing=$(losetup -j "$BACKING_FILE" 2>/dev/null | head -1 | cut -d: -f1 || true)
if [ -n "$existing" ] && [ -b "$existing" ]; then
    LOOP_DEV="$existing"
    echo "[DEBUG] Reusing existing loop device: $LOOP_DEV"
else
    echo "[DEBUG] No existing loop, creating new one..."
    LOOP_DEV=$(losetup -f --show "$BACKING_FILE") || {
        echo "[ERROR] losetup -f --show failed"
        echo "[DEBUG] Available loop devs: $(ls /dev/loop* 2>/dev/null | tr '\n' ' ')"
        exit 1
    }
    echo "[DEBUG] Created new loop device: $LOOP_DEV"
fi

echo "LOOP_DEV=$LOOP_DEV"

# zero a wide enough region to clear SB + cache_info + cache_ctrl +
# the new 64 MiB bitmap region (PCACHE_BITMAP_REGION_SIZE). 80 MiB > 64 MiB +
# 4 MiB scratch.  Kernel sb_init() then writes a fresh, valid SB on first load.
if [ -b /dev/pmem0 ]; then
    echo "[DEBUG] Zeroing first 80 MiB of /dev/pmem0 (clears SB + bitmap region)..."
    dd if=/dev/zero of=/dev/pmem0 bs=1M count=80 2>/dev/null || true
else
    echo "[WARN] /dev/pmem0 not found — skipping PMEM setup"
fi

# Show block size
if [ -b "$LOOP_DEV" ]; then
    SIZE=$(blockdev --getsz "$LOOP_DEV")
    echo "SIZE=$SIZE"
else
    echo "[ERROR] $LOOP_DEV is not a block device"
    exit 1
fi
REMOTE
}

# Internal: get backing device size from a node, given the loop dev path
get_backing_size() {
    local node="$1"
    local loop_dev="$2"
    ssh_node "$node" "blockdev --getsz $loop_dev 2>/dev/null || echo '0'"
}

cmd_start() {
    log "=== Step 3: Start dm-pcache on both VMs ==="

    # --- Clean environment first to ensure fresh state ---
    log "Cleaning both VMs..."
    cleanup_both_nodes

    # --- TCP tuning: increase socket buffer limits for high-throughput replication ---
    log "Tuning TCP buffer settings on both VMs..."
    for node in "$NODE1" "$NODE2"; do
        ssh_node "$node" bash -s <<'TUNE'
# TCP tuning for high-throughput replication
# tcp_rmem format: min/default/max
# Setting default to 16MB so wscale negotiation gives large enough rcv_wnd
echo 4194304 16777216 16777216 > /proc/sys/net/ipv4/tcp_rmem
echo 4194304 4194304 4194304 > /proc/sys/net/ipv4/tcp_wmem
echo 16777216 > /proc/sys/net/core/rmem_max
echo 16777216 > /proc/sys/net/core/wmem_max
# Disable slow-start-after-idle so cwnd doesn't reset after the 5s wait
echo 0 > /proc/sys/net/ipv4/tcp_slow_start_after_idle
echo "TCP tuning done:"
cat /proc/sys/net/ipv4/tcp_rmem
cat /proc/sys/net/ipv4/tcp_slow_start_after_idle
TUNE
    done

    # --- Prepare backing store on Node 1 (creates the loop device) ---
    log "Preparing backing store on $NODE1..."
    local loop_out
    loop_out=$(prepare_backing "$NODE1")
    local loop_dev
    loop_dev=$(echo "$loop_out" | grep "^LOOP_DEV=" | cut -d= -f2)
    local backing_sz
    backing_sz=$(echo "$loop_out" | grep "^SIZE=" | cut -d= -f2)

    if [ -z "$loop_dev" ] || [ -z "$backing_sz" ]; then
        echo "DEBUG loop_out:"
        echo "$loop_out"
        err "Failed to get loop device on $NODE1"
    fi
    log "  Loop device: $loop_dev  |  Size sectors: $backing_sz"

    # --- Clone backing file to Node 2 so sizes match ---
    log "Cloning backing file to $NODE2..."
    ssh_node "$NODE2" "
        set -e
        mkdir -p /var/lib
        truncate -s $BACKING_SIZE /var/lib/pcache_backing.img
        chmod 600 /var/lib/pcache_backing.img
        echo 'Backing file created on $NODE2'
    "

    # --- Attach loop on Node 2 ---
    log "Attaching loop device on $NODE2..."
    local n2_loop_out
    n2_loop_out=$(ssh_node "$NODE2" 'bash -s' <<'REMOTE'
set -e
BACKING_FILE="/var/lib/pcache_backing.img"
existing=$(losetup -j "$BACKING_FILE" 2>/dev/null | head -1 | cut -d: -f1)
if [ -b "$existing" ]; then
    LOOP_DEV="$existing"
else
    LOOP_DEV=$(losetup -f --show "$BACKING_FILE")
fi
echo "LOOP_DEV=$LOOP_DEV"
REMOTE
)
    local n2_loop_dev
    n2_loop_dev=$(echo "$n2_loop_out" | grep "^LOOP_DEV=" | cut -d= -f2)
    local n2_sz
    n2_sz=$(get_backing_size "$NODE2" "$n2_loop_dev")

    if [ -z "$n2_loop_dev" ] || [ -z "$n2_sz" ] || [ "$n2_sz" = "0" ]; then
        echo "DEBUG n2_loop_out:"
        echo "$n2_loop_out"
        err "Failed to get loop device on $NODE2"
    fi

    if [ "$backing_sz" != "$n2_sz" ]; then
        err "Backing device size mismatch: Node1=$backing_sz Node2=$n2_sz"
    fi
    info "Backing device size verified: $backing_sz sectors ($NODE1=$loop_dev, $NODE2=$n2_loop_dev)"

    # --- Load module on BOTH nodes (order: Secondary first, then Primary) ---
    log "Loading dm-pcache.ko on both VMs..."
    for node in "$NODE2" "$NODE1"; do
        log "  Loading on $node..."
        ssh_node "$node" "
            set -e
            dmsetup remove $DMSETUP_DEV 2>/dev/null || true
            sleep 2
            rmmod dm_pcache 2>/dev/null || true
            sleep 2
            insmod /root/dm-pcache.ko && echo '  Module loaded OK' || (echo '  Module load FAILED'; exit 1)
        "
    done

    # --- Create dmsetup devices (Secondary first, then Primary) ---
    log "Creating dmsetup devices on both VMs..."

    # device-level args only (my_node_id, listen_port, is_primary).
    # Peers come from dmsetup message add-peer after both ends are up.
    # Secondary first so its accept_thread is ready when Primary connects.
    log "  Creating device on $NODE2 (Secondary, my_node_id=1)..."
    local n2_table="0 $n2_sz pcache-dist $PMEM_DEV $n2_loop_dev my_node_id=1 is_primary=0 listen_port=$LISTEN_PORT"
    ssh_node "$NODE2" "
        set -e
        dmsetup remove $DMSETUP_DEV 2>/dev/null || true
        dmsetup create $DMSETUP_DEV --table '$n2_table'
        echo '  dmsetup create OK'
        dmsetup table $DMSETUP_DEV
    "

    log "  Creating device on $NODE1 (Primary, my_node_id=0)..."
    local n1_table="0 $backing_sz pcache-dist $PMEM_DEV $loop_dev my_node_id=0 is_primary=1 listen_port=$LISTEN_PORT"
    ssh_node "$NODE1" "
        set -e
        dmsetup remove $DMSETUP_DEV 2>/dev/null || true
        dmsetup create $DMSETUP_DEV --table '$n1_table'
        echo '  dmsetup create OK'
        dmsetup table $DMSETUP_DEV
    "

    log "  Adding peers via dmsetup message..."
    ssh_node "$NODE2" "dmsetup message $DMSETUP_DEV 0 add-peer 0 $NODE1:$LISTEN_PORT" \
        && echo "    $NODE2 add-peer 0 OK"
    ssh_node "$NODE1" "dmsetup message $DMSETUP_DEV 0 add-peer 1 $NODE2:$LISTEN_PORT" \
        && echo "    $NODE1 add-peer 1 OK"

    # --- Wait for connection to establish ---
    log "Waiting for Primary-Secondary connection (5s)..."
    sleep 5

    # --- Verify connection state ---
    log "Verifying connection state..."
    for node in "$NODE1" "$NODE2"; do
        log "  $node module state:"
        ssh_node "$node" "
            if lsmod | grep -q dm_pcache; then
                dmsetup status $DMSETUP_DEV 2>/dev/null || echo '  (no status)'
                dmsetup table $DMSETUP_DEV
            else
                echo '  MODULE NOT LOADED'
            fi
        "
    done

    log "Start complete. Devices created:"
    log "  Primary:   /dev/mapper/$DMSETUP_DEV (connects to $NODE2:$LISTEN_PORT)"
    log "  Secondary: /dev/mapper/$DMSETUP_DEV (listens on port $LISTEN_PORT)"
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 4: Test (run I/O + verify cross-node consistency)
# ─────────────────────────────────────────────────────────────────────────────

cmd_test() {
    log "=== Step 4: Run I/O test + verify data consistency ==="

    # --- 4a. Functional write test ---
    log "4a. Functional write test (dd)..."

    local test_file="/tmp/test_pattern.bin"
    local test_sz="4M"

    # Generate test pattern on Node 1
    log "  Generating $test_sz random test file on $NODE1..."
    ssh_node "$NODE1" "dd if=/dev/urandom of=$test_file bs=1M count=$((4)) 2>/dev/null && echo '  Test file created'"

    # Compute reference hash
    local ref_md5
    ref_md5=$(ssh_node "$NODE1" "md5sum $test_file | awk '{print \$1}'")
    log "  Reference MD5: $ref_md5"

    # Write via Primary's dmsetup device
    log "  Writing to /dev/mapper/$DMSETUP_DEV on $NODE1..."
    ssh_node "$NODE1" "
        dd if=$test_file of=/dev/mapper/$DMSETUP_DEV bs=1M count=4 oflag=direct 2>&1 | tail -3
    "

    log "  Waiting for replication to Secondary (3s)..."
    sleep 3

    # --- 4b. Verify data on Secondary via raw PMEM read ---
    log "4b. Verifying data on Secondary..."

    # Read back from Secondary's /dev/mapper device (after sync)
    local n2_md5
    n2_md5=$(ssh_node "$NODE2" "
        dd if=/dev/mapper/$DMSETUP_DEV of=/tmp/verify.bin bs=1M count=4 iflag=direct 2>/dev/null
        md5sum /tmp/verify.bin | awk '{print \$1}'
    " 2>/dev/null || echo "FAILED")

    log "  Secondary MD5: $n2_md5"

    if [ "$ref_md5" = "$n2_md5" ]; then
        log "  DATA MATCH: Replication verified successfully!"
    else
        warn "DATA MISMATCH: Primary=$ref_md5 vs Secondary=$n2_md5"
        warn "This may indicate replication lag — retry after longer wait."
    fi

    # --- 4c. Check network stats ---
    log "4c. Network / connection diagnostics..."

    for node in "$NODE1" "$NODE2"; do
        log "  $node:"
        ssh_node "$node" "
            # Check if module is loaded
            if lsmod | grep -q dm_pcache; then
                echo '  Module: loaded'
                # Try to get dmsetup status
                dmsetup status $DMSETUP_DEV 2>/dev/null | head -3 || echo '  (no status available)'
                # Check for open network connections on the port
                ss -tnp 2>/dev/null | grep -E '$LISTEN_PORT|$NODE1|$NODE2' | head -5 || echo '  (no TCP sessions found)'
            else
                echo '  Module: NOT LOADED'
            fi
        "
    done

    # --- 4d. Stress test (sequential writes) ---
    log "4d. Sequential write stress test..."

    ssh_node "$NODE1" "
        for i in \$(seq 1 4); do
            dd if=/dev/urandom of=/dev/mapper/$DMSETUP_DEV bs=64K count=32 seek=\$((i * 32)) oflag=direct 2>/dev/null
        done
        echo '  Sequential writes done'
    " &

    ssh_node "$NODE2" "
        sleep 2
        for i in \$(seq 1 4); do
            dd if=/dev/urandom of=/dev/mapper/$DMSETUP_DEV bs=64K count=32 seek=\$((i * 32)) oflag=direct 2>/dev/null
        done
        echo '  Sequential writes done'
    " &

    wait
    log "  Stress test complete."

    log "Test suite finished."
}

# ─────────────────────────────────────────────────────────────────────────────
# bitmap status query
# ─────────────────────────────────────────────────────────────────────────────

cmd_bm_status() {
    log "=== bitmap-status on both nodes ==="
    for node in "$NODE1" "$NODE2"; do
        log "  $node:"
        ssh_node "$node" "dmsetup message $DMSETUP_DEV 0 bitmap-status 2>&1" || true
        ssh_node "$node" "dmsetup status $DMSETUP_DEV 2>&1" || true
    done
}

# ─────────────────────────────────────────────────────────────────────────────
# incremental resync test
#   1. Steady state (assumes 'start' has completed initial sync)
#   2. Drop port 9764 traffic on Secondary via nftables
#   3. Primary writes 1 MiB → should mark bitmap dirty
#   4. Restore traffic → Primary reconnects → SYNC_SOURCE → incremental resync
#   5. Verify both bitmaps clean + MD5 match
# ─────────────────────────────────────────────────────────────────────────────

cmd_test_resync() {
    log "=== incremental resync test ==="

    # Wait for initial full sync to complete + both ends in ESTABLISHED before
    # we start tearing the link down. Otherwise we hit the "first connect"
    # full-sync path instead of the steady-state incremental one.
    log "  Waiting up to 30 s for initial connection + sync to complete..."
    local tries=0
    while [ $tries -lt 30 ]; do
        local n1_state n2_state
        n1_state=$(ssh_node "$NODE1" "dmesg | grep -oE 'primary: repl_state [A-Z_]+ -> [A-Z_]+' | tail -1")
        n2_state=$(ssh_node "$NODE2" "dmesg | grep -oE 'secondary: repl_state [A-Z_]+ -> [A-Z_]+' | tail -1")
        if [[ "$n1_state" == *"-> ESTABLISHED" ]] && [[ "$n2_state" == *"-> ESTABLISHED" ]]; then
            log "  Both nodes ESTABLISHED (n1='$n1_state' n2='$n2_state')"
            break
        fi
        sleep 1
        tries=$((tries + 1))
    done
    if [ $tries -ge 30 ]; then
        warn "Timed out waiting for ESTABLISHED — proceeding anyway"
    fi

    log "  Initial bitmap state:"
    cmd_bm_status

    log "  [1/5] Blocking port $LISTEN_PORT inbound on $NODE2..."
    ssh_node "$NODE2" "
        nft add table inet pcache_test 2>/dev/null || true
        nft add chain inet pcache_test input '{ type filter hook input priority 0; }' 2>/dev/null || true
        nft flush chain inet pcache_test input 2>/dev/null || true
        nft add rule inet pcache_test input tcp dport $LISTEN_PORT drop
        echo '  nft rules:'
        nft list chain inet pcache_test input
    "

    log "  [2/5] Waiting 35 s for Primary's 30 s ACK timeout + a few retries..."
    sleep 35

    log "  Bitmap after disconnect:"
    cmd_bm_status

    log "  [3/5] Writing 1 MiB on Primary while peer offline (expects bitmap to dirty)..."
    ssh_node "$NODE1" "
        dd if=/dev/urandom of=/tmp/resync_input.bin bs=1M count=1 status=none
        dd if=/tmp/resync_input.bin of=/dev/mapper/$DMSETUP_DEV bs=1M count=1 oflag=direct conv=fsync 2>&1 | tail -1
        md5sum /tmp/resync_input.bin
    " || true

    log "  Bitmap after offline write (Primary should show dirty>0):"
    cmd_bm_status

    log "  [4/5] Unblocking port $LISTEN_PORT on $NODE2..."
    ssh_node "$NODE2" "nft flush chain inet pcache_test input"

    log "       Waiting 15 s for reconnect + resync..."
    sleep 15

    log "  Bitmap after reconnect (expected: both clean):"
    cmd_bm_status

    log "  [5/5] Verifying MD5 across nodes..."
    local n1_md5 n2_md5
    n1_md5=$(ssh_node "$NODE1" "dd if=/dev/mapper/$DMSETUP_DEV of=/tmp/verify_n1.bin bs=1M count=4 iflag=direct status=none && md5sum /tmp/verify_n1.bin | awk '{print \$1}'")
    n2_md5=$(ssh_node "$NODE2" "dd if=/dev/mapper/$DMSETUP_DEV of=/tmp/verify_n2.bin bs=1M count=4 iflag=direct status=none && md5sum /tmp/verify_n2.bin | awk '{print \$1}'")
    log "  Primary  MD5: $n1_md5"
    log "  Secondary MD5: $n2_md5"
    if [ "$n1_md5" = "$n2_md5" ]; then
        log "  DATA MATCH: incremental resync succeeded."
    else
        warn "DATA MISMATCH: $n1_md5 vs $n2_md5"
    fi

    log "  Removing nft table on $NODE2..."
    ssh_node "$NODE2" "nft delete table inet pcache_test 2>/dev/null || true"
}

# ─────────────────────────────────────────────────────────────────────────────
# force-resync message test
# ─────────────────────────────────────────────────────────────────────────────

cmd_test_force() {
    log "=== force-resync test ==="
    cmd_bm_status
    log "  Issuing 'force-resync' on Primary..."
    ssh_node "$NODE1" "dmsetup message $DMSETUP_DEV 0 force-resync && echo 'force-resync issued'"
    log "  Waiting 10 s for reconnect + full resync..."
    sleep 10
    log "  Bitmap after force-resync:"
    cmd_bm_status
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 5: Stop (remove devices + unload module)
# ─────────────────────────────────────────────────────────────────────────────

cmd_stop() {
    log "=== Step 5: Stop dm-pcache on all VMs ==="

    for node in "$NODE1" "$NODE2" "$NODE3"; do
        log "  Stopping on $node..."
        ssh_node "$node" "
            set -e
            dmsetup remove $DMSETUP_DEV 2>/dev/null && echo '  Device removed' || echo '  (no device)'
            rmmod dm_pcache 2>/dev/null && echo '  Module unloaded' || echo '  (module not loaded)'
        " || true
    done

    log "Stop complete."
}

# ─────────────────────────────────────────────────────────────────────────────
# 3-node mesh start
# Topology: node-1 (Primary, id=0) ↔ node-2 (Secondary, id=1)
#                                ↔ node-3 (Secondary, id=2)
# Note: only Primary↔Secondary connections in this step. Sec↔Sec links
# require id-based direction (deferred — current worker uses role-based
# choose).
# ─────────────────────────────────────────────────────────────────────────────
cmd_start3() {
    log "=== Start 3-node mesh ==="

    log "Cleaning all 3 VMs..."
    for n in "$NODE1" "$NODE2" "$NODE3"; do cleanup_node "$n"; done

    log "Tuning TCP on all 3 VMs..."
    for n in "$NODE1" "$NODE2" "$NODE3"; do
        ssh_node "$n" bash -s <<'TUNE'
echo 4194304 16777216 16777216 > /proc/sys/net/ipv4/tcp_rmem
echo 4194304 4194304 4194304 > /proc/sys/net/ipv4/tcp_wmem
echo 16777216 > /proc/sys/net/core/rmem_max
echo 16777216 > /proc/sys/net/core/wmem_max
echo 0 > /proc/sys/net/ipv4/tcp_slow_start_after_idle
TUNE
    done

    log "Preparing backing on $NODE1 (origin)..."
    local loop_out
    loop_out=$(prepare_backing "$NODE1")
    local loop_dev
    loop_dev=$(echo "$loop_out" | grep "^LOOP_DEV=" | cut -d= -f2)
    local backing_sz
    backing_sz=$(echo "$loop_out" | grep "^SIZE=" | cut -d= -f2)
    log "  $NODE1 loop=$loop_dev size=$backing_sz sectors"

    local -A LOOPS=()
    LOOPS[$NODE1]=$loop_dev
    for n in "$NODE2" "$NODE3"; do
        log "  Mirroring backing onto $n..."
        ssh_node "$n" "
            set -e
            mkdir -p /var/lib
            truncate -s $BACKING_SIZE /var/lib/pcache_backing.img
            chmod 600 /var/lib/pcache_backing.img
        "
        local out
        out=$(ssh_node "$n" 'bash -s' <<'REMOTE'
set -e
BACKING_FILE="/var/lib/pcache_backing.img"
existing=$(losetup -j "$BACKING_FILE" 2>/dev/null | head -1 | cut -d: -f1)
if [ -b "$existing" ]; then LOOP_DEV="$existing"
else LOOP_DEV=$(losetup -f --show "$BACKING_FILE"); fi
echo "LOOP_DEV=$LOOP_DEV"
REMOTE
)
        local lp
        lp=$(echo "$out" | grep "^LOOP_DEV=" | cut -d= -f2)
        LOOPS[$n]=$lp
        log "  $n loop=$lp"
    done

    log "Loading module on all 3 (Secondaries first)..."
    for n in "$NODE3" "$NODE2" "$NODE1"; do
        ssh_node "$n" "
            dmsetup remove $DMSETUP_DEV 2>/dev/null || true
            rmmod dm_pcache 2>/dev/null || true
            python3 -c \"import os; fd = os.open('/dev/pmem0', os.O_RDWR); os.lseek(fd, 0x1000, os.SEEK_SET); os.write(fd, b'\\x00' * 0x6000); os.close(fd)\"
            insmod /root/dm-pcache.ko && echo '  $n module loaded' || (echo '  $n module load FAILED'; exit 1)
        "
    done

    log "dmsetup create on all 3 (Secondaries first)..."
    for spec in "$NODE3 2 0" "$NODE2 1 0" "$NODE1 0 1"; do
        local n nid prim
        n=$(echo "$spec" | awk '{print $1}')
        nid=$(echo "$spec" | awk '{print $2}')
        prim=$(echo "$spec" | awk '{print $3}')
        local tbl="0 $backing_sz pcache-dist $PMEM_DEV ${LOOPS[$n]} my_node_id=$nid is_primary=$prim listen_port=$LISTEN_PORT"
        ssh_node "$n" "dmsetup create $DMSETUP_DEV --table '$tbl'" \
            && echo "  $n created with my_node_id=$nid is_primary=$prim"
    done

    log "add-peer messages (Primary -> both Secondaries)..."
    ssh_node "$NODE1" "dmsetup message $DMSETUP_DEV 0 add-peer 1 $NODE2:$LISTEN_PORT" && echo "  $NODE1 add-peer 1 OK"
    ssh_node "$NODE1" "dmsetup message $DMSETUP_DEV 0 add-peer 2 $NODE3:$LISTEN_PORT" && echo "  $NODE1 add-peer 2 OK"
    ssh_node "$NODE2" "dmsetup message $DMSETUP_DEV 0 add-peer 0 $NODE1:$LISTEN_PORT" && echo "  $NODE2 add-peer 0 OK"
    ssh_node "$NODE3" "dmsetup message $DMSETUP_DEV 0 add-peer 0 $NODE1:$LISTEN_PORT" && echo "  $NODE3 add-peer 0 OK"

    log "Waiting 8s for full mesh to converge..."
    sleep 8

    log "All-node ESTABLISHED check:"
    for n in "$NODE1" "$NODE2" "$NODE3"; do
        log "  $n repl_state(s):"
        ssh_node "$n" "dmesg | grep -oE 'repl_state [A-Z_]+ -> [A-Z_]+' | tail -5"
    done
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 6: Cleanup (full teardown)
# ─────────────────────────────────────────────────────────────────────────────

cmd_cleanup() {
    log "=== Step 6: Full cleanup on both VMs ==="

    cmd_stop

    for node in "$NODE1" "$NODE2"; do
        log "  Cleaning up on $node..."
        ssh_node "$node" "
            set -e
            # Detach loop devices
            for lp in \$(losetup -j /var/lib/pcache_backing.img 2>/dev/null | cut -d: -f1); do
                losetup -d \$lp 2>/dev/null && echo \"  Loop \$lp detached\" || true
            done
            # Remove backing file
            rm -f /var/lib/pcache_backing.img && echo '  Backing file removed' || true
        " || true
    done

    log "Cleanup complete."
}

# ─────────────────────────────────────────────────────────────────────────────
# Full pipeline
# ─────────────────────────────────────────────────────────────────────────────

cmd_all() {
    log "========================================="
    log "Full pipeline: build -> deploy -> start -> test"
    log "========================================="

    cmd_build
    cmd_deploy
    cmd_start
    cmd_test

    log ""
    log "========================================="
    log "Pipeline complete!"
    log "To inspect logs on VMs:"
    log "  Primary:   ssh $SSH_USER@$NODE1 'dmesg | grep dm-pcache'"
    log "  Secondary: ssh $SSH_USER@$NODE2 'dmesg | grep dm-pcache'"
    log "To stop:    $0 stop"
    log "To cleanup:  $0 cleanup"
    log "========================================="
}

# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

usage() {
    cat <<EOF
Usage: $0 <command>

Commands:
  build         - Compile dm-pcache.ko on the host kernel tree
  deploy        - Copy the module to both VMs
  start         - Load module + create dmsetup device on both VMs
  test          - Run I/O test and verify data replication
  stop          - Remove dmsetup device + unload module on both VMs
  cleanup       - Full teardown (stop + remove backing files)
  all           - Run the complete pipeline (build -> deploy -> start -> test)

  bm-status     - query bitmap-status on both nodes
  test-resync   - drop port → write while offline → reconnect → verify
  test-force    - trigger dmsetup 'force-resync' message

Prerequisites:
  1. Host: the kernel tree ($HOST_KERNEL, default: this tree) must be present
  2. Both VMs must have /dev/pmem0 and /dev/loop0 available
  3. SSH key-based auth must be configured for root on both VMs

EOF
}

main() {
    if [ $# -eq 0 ]; then
        usage
        exit 1
    fi

    case "$1" in
        reset)        cmd_reset ;;
        test-crash)   cmd_test_crash ;;
        stress)       cmd_stress "${2:-20}" ;;
        start3)       cmd_start3 ;;
        build)   cmd_build ;;
        deploy)  cmd_deploy ;;
        start)   cmd_start ;;
        test)    cmd_test ;;
        stop)    cmd_stop ;;
        cleanup) cmd_cleanup ;;
        all)     cmd_all ;;
        bm-status)    cmd_bm_status ;;
        test-resync)  cmd_test_resync ;;
        test-force)   cmd_test_force ;;
        -h|--help|help) usage ;;
        *)       err "Unknown command: $1" ;;
    esac
}

main "$@"
