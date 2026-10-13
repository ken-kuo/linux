#!/bin/bash
# Run the 4-workload fio matrix against $DEV and write per-workload JSON
# results into $OUTDIR.
#
# Working sets are sized to fit inside the cache so neither stack triggers
# its eviction path during the run.  continue_on_error=write keeps fio
# emitting numbers when one stack returns an EIO mid-run.
#
# Usage:
#   DEV=/dev/mapper/pcache-bench OUTDIR=/tmp/r-ddmpcache run_fio.sh
#
# Workloads:
#   w1 4K random write, qd=1,  size=64M   - single-shot latency
#   w2 4K random write, qd=8,  size=128M  - moderate concurrency
#   w3 128K sequential write, qd=8, size=256M - bandwidth
#   w4 4K random read after warm, qd=32, size=128M - cache-hit read path

set -euo pipefail

DEV=${DEV:?DEV not set}
OUTDIR=${OUTDIR:?OUTDIR not set}
RUNTIME=${RUNTIME:-15}        # seconds per workload
RAMP=${RAMP:-3}               # warmup seconds

mkdir -p "$OUTDIR"

[ -b "$DEV" ] || { echo "no such block device: $DEV"; exit 1; }

# Common fio flags.  --direct=1 bypasses the page cache so we measure
# the block stack itself, not Linux's page cache amplifying.
# continue_on_error=write tells fio not to abort if a write returns
# EIO mid-run (dm-pcache may do this when its Secondary cache is full).
COMMON=(
    --ioengine=libaio
    --direct=1
    --filename="$DEV"
    --runtime="$RUNTIME"
    --ramp_time="$RAMP"
    --time_based=1
    --norandommap=1
    --randrepeat=0
    --group_reporting=1
    --output-format=json
    --continue_on_error=write
)

run() {
    local name=$1; shift
    echo "  -> $name" >&2
    fio "${COMMON[@]}" --name="$name" "$@" --output="$OUTDIR/${name}.json"
}

echo "[$(date +%H:%M:%S)] fio matrix on $DEV -> $OUTDIR" >&2

# w1: single-shot latency.  qd=1 is the cleanest measure of one write's
# end-to-end cost (cache insert + sync replicate + ack).
run w1_randw_qd1_jobs1   --rw=randwrite --bs=4k   --iodepth=1  --numjobs=1 --size=64M

# w2: moderate concurrency.
run w2_randw_qd8_jobs1   --rw=randwrite --bs=4k   --iodepth=8  --numjobs=1 --size=128M

# w3: bandwidth-ish (large block, modest qd).
run w3_seqw_128k_qd8     --rw=write     --bs=128k --iodepth=8  --numjobs=1 --size=256M

# w4: cache-hit reads.  Pre-write the working set so cache is warm, then
# the timed read run measures the cache-read path with replication idle.
echo "[$(date +%H:%M:%S)] pre-warm cache for read test" >&2
fio --ioengine=libaio --direct=1 --filename="$DEV" \
    --name=prewarm --rw=write --bs=128k --iodepth=8 --numjobs=1 \
    --size=128M --output=/dev/null
run w4_randr_qd32_jobs1  --rw=randread  --bs=4k   --iodepth=32 --numjobs=1 --size=128M

echo "[$(date +%H:%M:%S)] fio matrix done" >&2
