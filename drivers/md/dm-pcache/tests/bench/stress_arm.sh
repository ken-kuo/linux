#!/bin/bash
# Keep background stress running on every node for the length of a regression.
# Re-arms on a loop because the suite reboots nodes (F35 crashes one on purpose),
# which would otherwise silently end the stress halfway through the run.
#   CPU: half the cores. Memory: 2 x 256M, to pressure the page allocator --
#   the allocator is the half of #23's neighbourhood we can actually perturb.
#   I/O: sync churn. No --hdd: node-2 has 5 GB free and filling it would fail
#   demos for a reason that has nothing to do with the code.
set -uo pipefail
arm_one() {
    local ip=$1 cpus=$2
    timeout 20 ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 root@"$ip" \
        "pgrep -x stress >/dev/null || (setsid stress --cpu $cpus --vm 2 --vm-bytes 256M --io 2 >/dev/null 2>&1 &) ; exit 0" \
        >/dev/null 2>&1
}
while [ ! -f "$STOP_FLAG" ]; do
    arm_one 192.168.124.3 4
    arm_one 192.168.124.50 4
    arm_one 192.168.124.60 2
    sleep 30
done
for ip in 192.168.124.3 192.168.124.50 192.168.124.60; do
    timeout 20 ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 root@$ip 'pkill -x stress; exit 0' >/dev/null 2>&1
done
echo "stress disarmed"
