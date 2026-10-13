#!/bin/bash
# ---------------------------------------------------------------------------
# f39_silent.sh -- on-demand detector for fix-history #14 (silent divergence
# after a resync that overlapped application writes).
#
# F39 only fails when the timing happens to line up: anything the resync ships
# stale gets repaired by the writer's own later writes (re-dirtied and re-swept,
# or replicated inline once the link reaches ESTABLISHED).  This script removes
# that repair window and the ambiguity that comes with it:
#
#   1. kill the writer the instant the resync reports its verdict, so no write
#      after the verdict can paper over what the resync got wrong;
#   2. compare the whole 512 MiB, not just the first 64;
#   3. if the two sides differ, pay off the *recorded* debt with a bitmap-driven
#      resync (abort-resync drops the sockets and clears FULL_SYNC, so the
#      reconnect resyncs exactly the bits the bitmap still holds) and compare
#      again.
#
# Anything still different after step 3 was neither shipped nor recorded:
# silent loss.  A difference that step 3 repairs was honest debt.
# ---------------------------------------------------------------------------
set -uo pipefail

NODE1="192.168.124.3"
NODE2="192.168.124.50"
DEV="pcache-dist0"
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEMO="${SCRIPT_DIR}/dm_pcache_phase1_demo.sh"
SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5)
ITER="${1:-3}"
# Writes must be slow enough that the per-chunk writeback drain can keep up.
# A saturating writer makes every drain time out, the sweep gives up before it
# ships a single chunk ("resync incomplete, 131072 bit(s) still owed") and the
# overlap this is meant to test never happens.
RATE_IOPS="${2:-400}"
MB=512

log() { echo "[$(date '+%H:%M:%S')] $*"; }
sn()  { local n="$1"; shift; ssh "${SSH_OPTS[@]}" "root@${n}" "$@"; }

md5_dev() { sn "$1" "dd if=/dev/mapper/$DEV bs=1M count=${3:-$MB} skip=${2:-0} iflag=direct status=none | md5sum | awk '{print \$1}'"; }
md5_bk()  {
	local n="$1" l
	l=$(sn "$n" "losetup -j /var/lib/pcache_backing.img | cut -d: -f1 | head -1")
	sn "$n" "dd if=$l bs=1M count=$MB iflag=direct status=none | md5sum | awk '{print \$1}'"
}
dirty()   { sn "$1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+/[0-9]+'"; }

link_dev() { sn "$1" "ip -o route get $2 | awk '{for(i=1;i<=NF;i++) if(\$i==\"dev\") print \$(i+1)}' | head -1"; }

shape() {
	local d; d=$(link_dev "$NODE1" "$NODE2")
	sn "$NODE1" "modprobe sch_prio; modprobe sch_netem; \
		tc qdisc del dev $d root 2>/dev/null; \
		tc qdisc replace dev $d root handle 1: prio bands 3 priomap $(printf '0 %.0s' {1..16}) && \
		tc filter add dev $d parent 1: protocol ip prio 1 u32 match ip dst $NODE2 flowid 1:2 && \
		tc qdisc replace dev $d parent 1:2 handle 20: netem delay 10ms" >/dev/null 2>&1
	echo "$d"
}
unshape() { sn "$NODE1" "tc qdisc del dev $1 root 2>/dev/null" >/dev/null 2>&1; }

LOSS=0; HONEST=0; CLEAN=0
for i in $(seq 1 "$ITER"); do
	echo; echo "════════ iteration $i/$ITER ════════"

	bash "$DEMO" reset >/dev/null 2>&1
	bash "$DEMO" start >/dev/null || { log "cluster start failed"; exit 1; }
	sleep 5
	for n in "$NODE1" "$NODE2"; do
		a=$(sn "$n" "cat /sys/module/dm_pcache/srcversion")
		b=$(sn "$n" "modinfo /root/dm-pcache.ko | awk '/^srcversion/{print \$2}'")
		[ "$a" = "$b" ] || { log "STALE MODULE on $n ($a != $b)"; exit 1; }
	done
	sn "$NODE1" "dmesg -C" >/dev/null; sn "$NODE2" "dmesg -C" >/dev/null

	dev=$(shape); log "shaped $NODE1 -> $NODE2 on $dev (10ms)"

	pid=$(sn "$NODE1" "setsid bash -c 'fio --name=busy --filename=/dev/mapper/$DEV \
		--rw=randwrite --bs=64k --iodepth=32 --ioengine=libaio --direct=1 \
		--size=${MB}M --time_based --runtime=900 --continue_on_error=all \
		--rate_iops=$RATE_IOPS \
		</dev/null >/tmp/busy.json 2>/tmp/busy.err & echo \$!'")
	[[ "$pid" =~ ^[0-9]+$ ]] || { log "fio launch failed"; exit 1; }
	log "writer up (pid=$pid)"
	sleep 4

	# Kill the writer from *inside* the node the moment the verdict lands, so
	# no round trip is spent between the verdict and the last write.
	# fio forks a job process; killing the recorded pid leaves that child
	# writing.  pkill -x matches both.
	sn "$NODE1" "setsid bash -c 'while ! dmesg | grep -qE \"resync (done|incomplete)\"; do sleep 0.1; done; pkill -9 -x fio' >/dev/null 2>&1 &" >/dev/null 2>&1

	log "force-resync with the writer running"
	sn "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1

	t=0; verdict=""
	while [ "$t" -le 400 ]; do
		verdict=$(sn "$NODE1" "dmesg | grep -E 'resync (done|incomplete)' | tail -1")
		[ -n "$verdict" ] && break
		sleep 3; t=$((t + 3))
	done
	unshape "$dev"
	if [ -z "$verdict" ]; then log "no verdict in 400s -- skipping iteration"; continue; fi
	log "verdict (${t}s): ${verdict#*dm-pcache: }"
	owed=$(echo "$verdict" | grep -oE '[0-9]+ bit\(s\) still owed' | grep -oE '^[0-9]+')
	if [ "${owed:-0}" = "131072" ]; then
		log "NOTE: the sweep shipped nothing (every bit still owed) -- this iteration does not exercise the overlap"
	fi

	sn "$NODE1" "pkill -9 -x fio 2>/dev/null; while pgrep -x fio >/dev/null; do sleep 0.2; done" >/dev/null 2>&1
	log "writer stopped; settling 30s"
	sleep 30

	m1=$(md5_dev "$NODE1"); m2=$(md5_dev "$NODE2")
	log "after resync : node-1=${m1:0:12} node-2=${m2:0:12}  dirty1=$(dirty "$NODE1") dirty2=$(dirty "$NODE2")"
	if [ "$m1" = "$m2" ]; then
		log "RESULT: converged with no debt to settle"
		CLEAN=$((CLEAN + 1)); continue
	fi

	# Differ. Pay off whatever the bitmap still records, on a quiet device.
	log "differ -- paying recorded debt via abort-resync (bitmap-driven)"
	sn "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
	t=0
	while [ "$t" -le 400 ]; do
		n=$(sn "$NODE1" "dmesg | grep -cE 'resync (done|incomplete)'")
		[ "${n:-0}" -ge 2 ] && break
		sleep 3; t=$((t + 3))
	done
	log "second verdict (${t}s): $(sn "$NODE1" "dmesg | grep -E 'resync (done|incomplete)' | tail -1" | sed 's/.*dm-pcache: //')"
	sleep 5

	m1b=$(md5_dev "$NODE1"); m2b=$(md5_dev "$NODE2")
	log "after debt   : node-1=${m1b:0:12} node-2=${m2b:0:12}  dirty1=$(dirty "$NODE1") dirty2=$(dirty "$NODE2")"

	d1=$(sn "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+' | cut -d= -f2")
	if [ "$m1b" = "$m2b" ]; then
		log "RESULT: honest debt only (the bitmap knew about it)"
		HONEST=$((HONEST + 1))
	elif [ "${d1:-1}" != "0" ]; then
		log "RESULT: INCONCLUSIVE -- the rescue resync did not finish either (bm_dirty=$d1)"
		HONEST=$((HONEST + 1))
	else
		log "RESULT: *** SILENT LOSS *** neither shipped nor recorded"
		LOSS=$((LOSS + 1))
		echo "  ---- evidence ----"
		for c in $(seq 0 $(( MB / 64 - 1 )) ); do
			a=$(md5_dev "$NODE1" $((c * 64)) 64); b=$(md5_dev "$NODE2" $((c * 64)) 64)
			[ "$a" != "$b" ] && echo "    chunk $c ($((c*64))-$((c*64+64))M): ${a:0:12} vs ${b:0:12}"
		done
		for n in "$NODE1" "$NODE2"; do
			echo "    $n device=$(md5_dev "$n" | cut -c1-12) backing=$(md5_bk "$n" | cut -c1-12)"
		done
		for n in "$NODE1" "$NODE2"; do
			echo "    --- $n dmesg (this iteration) ---"
			sn "$n" "dmesg | grep -E 'RS_DATA|write to cache failed|bad RS_ACK|resync |split-brain|drain' | tail -12" | sed 's/^/      /'
		done
	fi
done

echo
echo "════════ summary over $ITER iteration(s) ════════"
echo "  converged, no debt : $CLEAN"
echo "  honest debt only   : $HONEST"
echo "  SILENT LOSS        : $LOSS"
[ "$LOSS" -eq 0 ]
