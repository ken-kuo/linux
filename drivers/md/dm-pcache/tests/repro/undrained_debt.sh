#!/bin/bash
# ---------------------------------------------------------------------------
# undrained_debt.sh -- does a dirty bit set while a peer is CONNECTED ever get
# drained without a reconnect?
#
# pcache_do_resync() has exactly one caller: pcache_primary_worker(), right
# after negotiation decides the role. Negotiation only happens on (re)connect.
# On the Primary, peer->bm bits are cleared only by that function's pre-clear
# -- pcache_bm_mark_synced() updates peer_gen_at_sync and flags but clears no
# bits. So any bit set by pcache_net_submit_write()'s skip path
# (!pcache_peer_can_replicate) while the connection stays up should persist
# until the connection is torn down and renegotiated.
#
# F51 saw bm_dirty=144 outlive a 300s settle window with every peer reporting
# ESTABLISHED and all three nodes agreeing on content. This script decides
# which of two things that was:
#
#   A. stale bookkeeping -- the range was re-written and replicated later, so
#      content agrees, but the "am I in sync" signal is permanently wrong; or
#   B. silent divergence -- the skipped write was the last one to that range,
#      so the peer is stale and nothing will ever fix it.
#
# and proves the cause either way, by forcing a reconnect at the end: if the
# debt only clears then, a reconnect is the sole drain.
#
# Usage: bash repro/undrained_debt.sh [iterations]
# ---------------------------------------------------------------------------
set -uo pipefail

NODE1="192.168.124.3"
NODE2="192.168.124.50"
DEV="pcache-dist0"
LISTEN_PORT=9764
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEMO="${SCRIPT_DIR}/dm_pcache_phase1_demo.sh"
SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5
          -o ServerAliveInterval=15 -o ServerAliveCountMax=4)
ITER="${1:-5}"
MB=512                      # whole device: 1048576 sectors, 131072 bits @ 4K
SETTLE=120                  # how long to give the debt to drain on its own

log() { echo "[$(date '+%H:%M:%S')] $*"; }
sn()  { local n="$1"; shift; ssh "${SSH_OPTS[@]}" "root@${n}" "$@"; }

md5_dev() { sn "$1" "dd if=/dev/mapper/$DEV bs=1M count=$MB iflag=direct status=none | md5sum | awk '{print \$1}'"; }
dirty()   { sn "$1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+' | cut -d= -f2"; }
repl()    { sn "$1" "dmesg | grep -oE 'repl_state [A-Z_]+ -> [A-Z_]+' | tail -1 | awk '{print \$4}'"; }

block()   { sn "$NODE1" "nft add table inet dbg 2>/dev/null
                         nft add chain inet dbg output { type filter hook output priority 0\; } 2>/dev/null
                         nft add rule inet dbg output ip daddr $NODE2 tcp dport $LISTEN_PORT drop"; }
unblock() { sn "$NODE1" "nft delete table inet dbg 2>/dev/null" || true; }

A_STALE=0; B_DIVERGED=0; NOHIT=0; DRAINED_BY_RECONNECT=0

for i in $(seq 1 "$ITER"); do
	echo; echo "════════ iteration $i/$ITER ════════"

	bash "$DEMO" reset >/dev/null 2>&1
	bash "$DEMO" start >/dev/null || { log "cluster start failed"; exit 1; }
	sleep 5
	for n in "$NODE1" "$NODE2"; do
		a=$(sn "$n" "cat /sys/module/dm_pcache/srcversion")
		b=$(sn "$n" "modinfo -F srcversion /root/dm-pcache.ko")
		[ "$a" = "$b" ] || { log "STALE MODULE on $n ($a != $b)"; exit 1; }
	done
	sn "$NODE1" "dmesg -C" >/dev/null; sn "$NODE2" "dmesg -C" >/dev/null

	# 1. Build debt while the peer is unreachable.
	log "cut node-2 off and write, so its bitmap accumulates real debt"
	block || exit 1
	sn "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=64 oflag=direct status=none"
	log "  owed after the cut: $(dirty "$NODE1")"

	# 2. Reconnect with a writer running, so writes keep landing across the
	#    whole negotiate -> sweep -> ESTABLISHED transition. That transition
	#    is the window: pcache_peer_can_replicate() is false for all of it,
	#    so every write in it takes the bitmap path.
	log "reconnect with a writer running across the transition"
	sn "$NODE1" "setsid bash -c 'fio --name=w --filename=/dev/mapper/$DEV \
		--rw=randwrite --bs=64k --iodepth=8 --ioengine=libaio --direct=1 \
		--size=${MB}M --time_based --runtime=60 --rate_iops=300 \
		--continue_on_error=all </dev/null >/tmp/ud.json 2>/dev/null & echo \$!'" >/dev/null
	unblock
	sleep 60
	sn "$NODE1" "pkill -9 -x fio" 2>/dev/null || true
	sleep 5

	# 3. Quiesce and watch whether the debt drains on its own.
	log "device quiet; watching the debt for ${SETTLE}s"
	sn "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
	owed=""; t=0
	while [ "$t" -le "$SETTLE" ]; do
		owed=$(dirty "$NODE1")
		[ "${owed:-1}" = "0" ] && break
		sleep 10; t=$((t + 10))
	done
	st=$(repl "$NODE1")
	log "  after ${t}s: bm_dirty=$owed repl_state=$st"

	if [ "${owed:-1}" = "0" ]; then
		log "  RESULT: debt drained on its own — this iteration missed the window"
		NOHIT=$((NOHIT + 1))
		continue
	fi
	if [ "$st" != "ESTABLISHED" ]; then
		log "  RESULT: still $st — not the steady-state case, skipping"
		NOHIT=$((NOHIT + 1))
		continue
	fi

	# 4. Debt outstanding on a connected, established peer. Which kind?
	m1=$(md5_dev "$NODE1"); m2=$(md5_dev "$NODE2")
	if [ "$m1" = "$m2" ]; then
		log "  RESULT A: stale bookkeeping — content agrees, but $owed bit(s)"
		log "            stay owed with the peer ESTABLISHED ($m1)"
		A_STALE=$((A_STALE + 1))
	else
		log "  RESULT B: SILENT DIVERGENCE — peer is stale and nothing will fix it"
		log "            node-1=$m1 node-2=$m2, $owed bit(s) owed"
		B_DIVERGED=$((B_DIVERGED + 1))
	fi

	# 5. The cause check: force a reconnect and see whether that is what
	#    drains it. abort-resync drops the sockets, so the peer renegotiates.
	log "forcing a reconnect to test whether that is the only drain"
	sn "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
	t=0
	while [ "$t" -le 120 ]; do
		owed2=$(dirty "$NODE1")
		[ "${owed2:-1}" = "0" ] && break
		sleep 5; t=$((t + 5))
	done
	if [ "${owed2:-1}" = "0" ]; then
		log "  the reconnect drained it in ${t}s => a reconnect is the only drain"
		DRAINED_BY_RECONNECT=$((DRAINED_BY_RECONNECT + 1))
	else
		log "  still $owed2 owed after a reconnect — a different problem"
	fi
done

unblock
echo
echo "════════ summary over $ITER iteration(s) ════════"
echo "  A stale bookkeeping (content agrees, debt stuck): $A_STALE"
echo "  B silent divergence (peer stale, debt stuck):     $B_DIVERGED"
echo "  window missed (debt drained by itself):           $NOHIT"
echo "  debt cleared only after a forced reconnect:       $DRAINED_BY_RECONNECT"
