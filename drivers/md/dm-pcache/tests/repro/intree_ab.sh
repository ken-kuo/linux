#!/bin/bash
# A/B of the in-tree reproducer (intree_netpoll_repro.sh) on the receiver:
#   nofix: virtio_net built from the unfixed source -> expect the page BUG
#   fix:   virtio_net with the zero-budget refill fix -> expect none
# Both modules must already be on the receiver as /root/virtio_net-<tag>.ko.
#
# Usage: intree_ab.sh <logdir> [runs] [minutes]
DIR=${1:?usage: $0 <logdir> [runs] [minutes]}
RUNS=${2:-3}
MIN=${3:-15}
RCV=${RCV:-192.168.124.50}
HERE=$(cd "$(dirname "$0")" && pwd)
SSH="ssh -o ConnectTimeout=5 -o BatchMode=yes"
MOD=/lib/modules/7.3.0-rc3+/kernel/drivers/net/virtio_net.ko

wait_up() {
	local i
	for i in $(seq 1 60); do
		$SSH root@$RCV true 2>/dev/null && return 0
		sleep 10
	done
	return 1
}

reboot_rcv() {
	$SSH root@$RCV 'systemctl reboot' 2>/dev/null
	sleep 30
	wait_up || { echo "!! $(date +%T) receiver did not come back -- stopping"; exit 1; }
	sleep 20	# let netconsole bring up its target
}

for tag in nofix fix; do
	$SSH root@$RCV "cp /root/virtio_net-$tag.ko $MOD" || exit 1
	reboot_rcv
	echo "== [$tag] $(date +%T) virtio_net srcversion $($SSH root@$RCV 'cat /sys/module/virtio_net/srcversion')"
	for r in $(seq 1 "$RUNS"); do
		log=$DIR/intree-$tag-r$r.log
		bash "$HERE/intree_netpoll_repro.sh" "$log" "$MIN" > /dev/null
		rc=$?
		case $rc in
		0) echo "   [$tag r$r] clean after $MIN min";;
		2) echo "   [$tag r$r] $(grep -m1 'NEW CRASH' "$log" | sed 's/^!!!! //')"
		   grep -q 'NEW CRASH' "$log" || { echo "!! no vmcore -- stopping"; exit 1; }
		   wait_up || exit 1;;
		*) echo "   [$tag r$r] aborted rc=$rc: $(grep -m1 '!!' "$log")"; exit 1;;
		esac
		[ "$r" = "$RUNS" ] || reboot_rcv
	done
done
echo "== done $(date +%T)"
