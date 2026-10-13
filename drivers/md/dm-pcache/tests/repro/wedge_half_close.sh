#!/bin/bash
# Does an asymmetric cut leave the Secondary permanently refusing reconnects?
# Cut Primary->Secondary only, drop the Primary's socket, restore the link
# before the peer's keepalive can reap (20s idle + 3x10s = up to 50s), then
# watch whether the connection ever comes back.
N1=192.168.124.3; N2=192.168.124.50; DEV=pcache-dist0
S1(){ ssh -o ConnectTimeout=5 root@$N1 "$@"; }
S2(){ ssh -o ConnectTimeout=5 root@$N2 "$@"; }
log(){ echo "[$(date +%T)] $*"; }

for r in 1 2 3; do
  log "=== round $r: settle first"
  S1 "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1
  for i in $(seq 1 30); do [ "$(S1 "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+' | cut -d= -f2")" = "0" ] && break; sleep 2; done
  S1 'dmesg -C'; S2 'dmesg -C'

  log "cut Primary -> Secondary only, then drop the Primary's socket"
  S1 "nft add table inet wedge 2>/dev/null
      nft add chain inet wedge output '{ type filter hook output priority 0; }' 2>/dev/null
      nft add rule inet wedge output ip daddr $N2 tcp dport 9764 drop"
  S1 "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
  sleep 5
  log "restore the link after 5s (far side has NOT reaped yet)"
  S1 "nft delete table inet wedge 2>/dev/null"

  t=0; recovered=""
  while [ $t -le 300 ]; do
    # Any transition into ESTABLISHED counts. Naming the source states was a
    # bug: recovery actually runs ... -> SYNC_SOURCE -> ESTABLISHED, so once the
    # wedge was fixed this kept reporting "still wedged" on a recovered cluster.
    if S1 "dmesg | grep -q 'repl_state .* -> ESTABLISHED'"; then recovered=$t; break; fi
    sleep 10; t=$((t+10))
  done
  if [ -n "$recovered" ]; then
    log "RESULT round $r: recovered after ${recovered}s  (dup rejects: $(S2 "dmesg | grep -c 'drop dup'"))"
  else
    log "RESULT round $r: *** STILL WEDGED after 300s ***  (dup rejects: $(S2 "dmesg | grep -c 'drop dup'"), primary resets: $(S1 "dmesg | grep -c 'sync_params exchange failed'"))"
  fi
done
