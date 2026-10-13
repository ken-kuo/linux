#!/bin/bash
# Transport-ceiling diagnostic for the replication path: separate raw STREAMING
# bandwidth from cross-VM ROUND-TRIP rate, with zero dm-pcache in the path.
#
# Motivation: on a single host running both VMs, replicated 128K-seq write caps
# ~2750 MiB/s while local (no-peer) does ~5200. This script proves the cap is the
# host's cross-VM request-response round-trip rate, not bandwidth, PMEM, or driver
# code:
#   (1) iperf3  -- pure streaming; the TCP window keeps many bytes in flight with no
#                  per-message reply, so this measures raw transport BANDWIDTH.
#   (2) ping-pong (pingpong_srv.py/pingpong_cli.py) -- send 128K, wait for the 128K
#                  echo, repeat. Every message is a full cross-VM round trip (no
#                  windowing can hide it), so the saturating RT/s is the host's
#                  sustainable cross-VM round-trip RATE.
# If (1) >> dm-pcache and (2) ~= dm-pcache, the wall is the round-trip rate.
#
# Runs node-1 (client) -> node-2 (server) over the replication NIC (enp8s0, the
# NIC that owns NODE2_IP). dm-pcache reference: replicated 128K-seq / 128KiB.
set -uo pipefail
cd "$(dirname "$0")"
source ./common.sh
rsh() { command ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@"$1" "$2"; }

SRV=$NODE2_IP          # server = secondary (receiver) side
CLI=$NODE1_IP          # client = primary (sender) side
IPERF_PORT=5301
RR_PORT=5400
DUR=6

echo "=== (1) iperf3 streaming bandwidth ($CLI -> $SRV) ==="
rsh "$SRV" 'pkill -9 iperf3 2>/dev/null; sleep 1'
for P in 1 4; do
    rsh "$SRV" "pkill -9 iperf3 2>/dev/null; sleep 1; (iperf3 -s -1 -p $IPERF_PORT >/dev/null 2>&1 &); sleep 1"
    printf "  iperf -P %s : " "$P"
    # Last 'sender' line is the per-run summary (P=1) or the [SUM] summary (P>1).
    rsh "$CLI" "iperf3 -c $SRV -p $IPERF_PORT -t $DUR -P $P" 2>&1 \
        | grep sender | tail -1
done
rsh "$SRV" 'pkill -9 iperf3 2>/dev/null' || true

echo "=== (2) 128K ping-pong round-trip rate ($CLI -> $SRV) ==="
# Copy the python helpers to both nodes.
for ip in "$SRV" "$CLI"; do
    scp -q ./pingpong_srv.py ./pingpong_cli.py root@"$ip":/root/ 2>/dev/null
done
rsh "$SRV" 'pkill -9 -f pingpong_srv 2>/dev/null'; sleep 1
# ssh -f detaches the server cleanly (plain '&' over ssh gets SIGHUP'd on exit).
ssh -f -o StrictHostKeyChecking=no root@"$SRV" "setsid python3 /root/pingpong_srv.py $RR_PORT >/root/pp.log 2>&1"
sleep 2
rsh "$SRV" "ss -ltn | grep -q :$RR_PORT && echo '  server listening' || { echo '  server FAILED'; cat /root/pp.log; exit 1; }"
for P in 1 2 4 8 16; do
    printf "  "
    rsh "$CLI" "python3 /root/pingpong_cli.py $SRV $RR_PORT $P $DUR 2>/dev/null"
done
rsh "$SRV" 'pkill -9 -f pingpong_srv 2>/dev/null' || true
echo "DONE"
