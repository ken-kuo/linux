#!/bin/bash
# Run only the ddmpcache stack (w1..w4) — used to verify a kernel rebuild
# without re-running the DRBD stack each time. Compares to results/final2/.
set -euo pipefail
cd "$(dirname "$0")"
source ./common.sh

TS=$(date +%Y%m%d-%H%M%S)-verify
RES=results/$TS
mkdir -p "$RES/ddmpcache"

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

log "===== STACK: ddmpcache (verify) ====="
both "/root/bench/teardown_all.sh"
both "/root/bench/setup_backing.sh"
ssh_node $NODE2_IP "/root/bench/ddmpcache/setup.sh 2 0 1 $NODE1_IP:$PORT_DDMPCACHE"
ssh_node $NODE1_IP "/root/bench/ddmpcache/setup.sh 1 1 2 $NODE2_IP:$PORT_DDMPCACHE"
log "wait 10s for connection..."; sleep 10

log "run fio on node-1"
ssh_node $NODE1_IP "DEV=$DDMPCACHE_DEV OUTDIR=/tmp/r-ddmpcache /root/bench/fio/run_fio.sh"
rsync -az root@$NODE1_IP:/tmp/r-ddmpcache/ "$RES/ddmpcache/"

both "/root/bench/teardown_all.sh"

log "===== summary ====="
python3 ./summarize.py "$RES" | tee "$RES/summary.md"
log "results in $RES"
