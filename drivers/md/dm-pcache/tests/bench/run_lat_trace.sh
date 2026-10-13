#!/bin/bash
# Latency-decomposition run for the replicated 128K seq-write path.
#
# Brings up dm-pcache (node-1 primary, node-2 secondary), enables the
# PCACHE_LAT_TRACE trace_printk instrumentation on both nodes, runs the
# 128K sequential write workload at qd=1 (clean serial per-IO timing) and
# qd=8 (the actual benchmark concurrency), and pulls both trace buffers
# back for post-processing.
#
# Requires the instrumented dm-pcache.ko (PCACHE_LAT_TRACE=1) already at
# /root/dm-pcache.ko on both nodes.
set -euo pipefail
cd "$(dirname "$0")"
source ./common.sh

TS=$(date +%Y%m%d-%H%M%S)-lattrace
RES=results/$TS
mkdir -p "$RES"

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

# Tracing helpers (global ring buffer, nop tracer; trace_printk records
# regardless of current_tracer while tracing_on=1).
trace_setup() {
    both 'T=/sys/kernel/tracing; echo 0 > $T/tracing_on; echo nop > $T/current_tracer; echo 32768 > $T/buffer_size_kb; echo > $T/trace; echo 1 > $T/tracing_on'
}
trace_clear() { both 'echo > /sys/kernel/tracing/trace'; }
trace_grab() {  # $1 = label
    ssh_node $NODE1_IP 'echo 0 > /sys/kernel/tracing/tracing_on; cat /sys/kernel/tracing/trace' > "$RES/primary_$1.trace"
    ssh_node $NODE2_IP 'cat /sys/kernel/tracing/trace'                                          > "$RES/secondary_$1.trace"
}

deploy $NODE1_IP
deploy $NODE2_IP

log "===== bring up dm-pcache cluster ====="
both "/root/bench/teardown_all.sh" || true
both "/root/bench/setup_backing.sh"
ssh_node $NODE2_IP "/root/bench/ddmpcache/setup.sh 2 0 1 $NODE1_IP:$PORT_DDMPCACHE" >/dev/null
ssh_node $NODE1_IP "/root/bench/ddmpcache/setup.sh 1 1 2 $NODE2_IP:$PORT_DDMPCACHE" >/dev/null
log "wait 10s for connection..."; sleep 10

FIO_COMMON="--ioengine=libaio --direct=1 --filename=$DDMPCACHE_DEV --rw=write --bs=128k --time_based=1 --ramp_time=3 --runtime=10 --norandommap=1 --group_reporting=1 --output-format=json --name=lat"

run_one() {  # $1 = label  $2 = iodepth
    log "----- 128K seq write qd=$2 -----"
    trace_setup
    ssh_node $NODE1_IP "fio $FIO_COMMON --iodepth=$2 --size=512M" \
        > "$RES/fio_$1.json" 2>/dev/null || true
    trace_grab "$1"
    # quick fio headline
    python3 - "$RES/fio_$1.json" <<'PY'
import json,sys
j=json.load(open(sys.argv[1]))
w=j["jobs"][0]["write"]
print("  fio: BW=%.1f MiB/s IOPS=%.0f lat_avg=%.1fus"%(
    w["bw"]/1024, w["iops"], w["lat_ns"]["mean"]/1000))
PY
}

run_one qd1 1
trace_clear
run_one qd8 8

log "===== teardown ====="
both "/root/bench/teardown_all.sh" || true

log "results in $RES"
ls -la "$RES"
