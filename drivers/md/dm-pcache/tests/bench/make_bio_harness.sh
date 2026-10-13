#!/bin/bash
# Generate a copy of the demo harness that runs on the BIO cache device
# set up by bio_cachedev.sh instead of /dev/pmem0. The copies live in
# bench/bio-harness/ (not committed); regenerate after editing the originals.
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$HERE/bench/bio-harness
mkdir -p "$OUT"
for f in demo_features.sh dm_pcache_phase1_demo.sh; do
	sed 's#/dev/pmem0#/dev/pcache-cdev#g' "$HERE/$f" > "$OUT/$f"
done
echo "generated in $OUT: $(grep -c pcache-cdev "$OUT"/*.sh | tr '\n' ' ')"
