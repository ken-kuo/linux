#!/bin/bash
# Host health check for the replication rig — dm-pcache-free.
#
# The dm-pcache 128K-seq replication throughput is dominated by the host's
# cross-VM request-response ROUND-TRIP latency, which drifts a lot on a shared
# desktop host and is not reproducible across hours. This probes that path with
# ZERO dm-pcache in the way, so you can tell whether the environment is in a fast
# or slow state BEFORE trusting any dm-pcache A/B number.
#
# Two independent measurements between node-1 (client) and node-2 (server) over
# the replication NIC (enp8s0 = NODE2_IP):
#   (1) iperf3 single-stream  -> raw transport BANDWIDTH (streaming; TCP window
#       hides latency). Fresh reference ~65-70 Gbit/s. This is the control.
#   (2) 128K ping-pong P=8 depth=1 -> cross-VM ROUND-TRIP rate (each message is a
#       full cross-VM round trip; latency cannot be hidden). Fresh reference
#       ~2300-2400 MiB/s.
#
# Verdict: iperf high + ping-pong low  => host cross-VM latency path is SLOW;
#          dm-pcache numbers will be depressed and A/B deltas unreliable.
#          iperf low                    => a real network/link problem (rare here).
#
# Usage: bench/host_health.sh
set -uo pipefail
cd "$(dirname "$0")"
source ./common.sh
rsh() { command ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@"$1" "$2"; }

SRV=$NODE2_IP          # server = secondary side (owns enp8s0 / NODE2_IP)
CLI=$NODE1_IP          # client = primary side
IPERF_PORT=5301
RR_PORT=5400
DUR=5

# Fresh references (this rig, right after a host reboot).
IPERF_REF_GBIT=65
RR_REF_MIBS=2350

echo "=== host health check ($CLI -> $SRV, dm-pcache-free) ==="

# (1) iperf3 streaming bandwidth (control).
rsh "$SRV" 'pkill -9 iperf3 2>/dev/null; sleep 1; (iperf3 -s -1 -p '"$IPERF_PORT"' >/dev/null 2>&1 &); sleep 1'
IPERF_LINE=$(rsh "$CLI" "iperf3 -c $SRV -p $IPERF_PORT -t $DUR -P 1" 2>&1 | grep sender | tail -1)
rsh "$SRV" 'pkill -9 iperf3 2>/dev/null' || true
IPERF_GBIT=$(echo "$IPERF_LINE" | grep -oE '[0-9.]+ Gbits/sec' | grep -oE '[0-9.]+' | head -1)
echo "  iperf3  bandwidth   : ${IPERF_GBIT:-?} Gbit/s   (fresh ~${IPERF_REF_GBIT})"

# (2) 128K ping-pong round-trip rate.
for ip in "$SRV" "$CLI"; do scp -q ./pingpong_srv.py ./pingpong_cli.py root@"$ip":/root/ 2>/dev/null; done
rsh "$SRV" 'pkill -9 -f pingpong_srv 2>/dev/null'; sleep 1
# ssh -f detaches the server cleanly (a plain '&' gets SIGHUP'd when ssh exits).
ssh -f -o StrictHostKeyChecking=no root@"$SRV" "setsid python3 /root/pingpong_srv.py $RR_PORT >/root/pp.log 2>&1"
sleep 2
if ! rsh "$SRV" "ss -ltn | grep -q :$RR_PORT"; then
	echo "  ping-pong           : server failed to start"; exit 1
fi
RR_OUT=$(rsh "$CLI" "python3 /root/pingpong_cli.py $SRV $RR_PORT 8 $DUR 1 2>/dev/null")
rsh "$SRV" 'pkill -9 -f pingpong_srv 2>/dev/null' || true
RR_MIBS=$(echo "$RR_OUT" | grep -oE 'thr=[0-9]+' | grep -oE '[0-9]+' | head -1)
echo "  ping-pong RR rate   : ${RR_MIBS:-?} MiB/s   (fresh ~${RR_REF_MIBS})"

# Verdict.
echo "--- verdict ---"
if [ -z "${RR_MIBS:-}" ] || [ -z "${IPERF_GBIT:-}" ]; then
	echo "  INCONCLUSIVE (a measurement failed)"
elif awk "BEGIN{exit !($IPERF_GBIT < $IPERF_REF_GBIT * 0.5)}"; then
	echo "  iperf bandwidth is LOW -> real network/link problem, investigate the transport."
elif awk "BEGIN{exit !($RR_MIBS < $RR_REF_MIBS * 0.8)}"; then
	pct=$(awk "BEGIN{printf \"%.0f\", 100*$RR_MIBS/$RR_REF_MIBS}")
	echo "  SLOW: cross-VM round-trip path at ${pct}% of fresh while bandwidth is fine."
	echo "  -> dm-pcache throughput is depressed now; do NOT trust A/B deltas until it recovers"
	echo "     (retry later, or reboot the host to reset the cross-VM latency path)."
else
	echo "  OK: round-trip path near fresh -> dm-pcache A/B numbers should be trustworthy."
fi
