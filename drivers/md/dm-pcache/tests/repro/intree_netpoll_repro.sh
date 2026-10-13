#!/bin/bash
# In-tree-only reproducer for the virtio_net zero-budget refill race (#23).
#
# No dm-pcache: the receiver runs netconsole on the virtio NIC that carries
# heavy TCP receive, user space feeds printk through /dev/kmsg, and stress(1)
# adds memory pressure. netconsole's printer thread then polls that NIC with
# budget 0 (netpoll), which on an unfixed virtio_net refills the RX ring from
# the page pool while the NAPI CPU recycles into the same alloc.cache.
#
# Usage: intree_netpoll_repro.sh <logfile> [minutes]
# Env:   RCV (ssh address of the receiver), RX_IP / RX_MAC (receiver NIC that
#        netconsole is bound to), SND (sender), KMSG_SLEEP (seconds between
#        printk lines), STREAMS (iperf3 -P).
#
# The printk rate matters. netpoll only polls the device (with budget 0) on
# its direct send path, which it takes only while npinfo->txq is empty; once
# a backlog forms, every message is queued for the workqueue instead, and the
# queue has no bound -- a full-speed /dev/kmsg flood filled 3 GB of slab with
# queued skbs and drove the guest into OOM. Keep the rate below what the NIC
# drains, and watch SUnreclaim.
LOG=${1:?usage: $0 <logfile> [minutes]}
MIN=${2:-30}
RCV=${RCV:-192.168.124.50}
RX_IP=${RX_IP:-192.168.124.111}
RX_MAC=${RX_MAC:-52:54:00:88:be:26}
SND=${SND:-192.168.124.3}
KMSG_SLEEP=${KMSG_SLEEP:-0.002}
STREAMS=${STREAMS:-4}
SSH="ssh -o ConnectTimeout=5 -o BatchMode=yes"

exec > >(tee -a "$LOG") 2>&1
ts() { date +%H:%M:%S; }

# "<boot_id> <enp1s0 rx_bytes>"; a kdump reboot can finish between two
# samples, so a changed boot_id -- not an unreachable host -- marks a crash.
sample() { $SSH root@$RCV 'echo $(cat /proc/sys/kernel/random/boot_id /sys/class/net/enp1s0/statistics/rx_bytes)' 2>/dev/null; }
crashes() { $SSH root@$RCV 'ls -d /var/crash/*/ 2>/dev/null | wc -l' 2>/dev/null; }

cleanup() {
	$SSH root@$SND 'pkill -x iperf3' 2>/dev/null
	$SSH root@$RCV 'pkill -x iperf3; pkill -x stress; pkill -f "[k]msg_flood"' 2>/dev/null
}
trap cleanup EXIT

echo "== $(ts) receiver $RCV ($RX_IP, $RX_MAC), sender $SND, ${MIN} min"
$SSH root@$RCV 'uname -rv; echo "virtio_net srcversion $(cat /sys/module/virtio_net/srcversion)";
	dmesg | grep -m1 "netconsole: network logging started" || echo "!! netconsole not started";
	cat /proc/sys/kernel/printk' || { echo "!! receiver unreachable"; exit 1; }
CR0=$(crashes)
read -r BOOT0 _ <<< "$(sample)"

# Pin the sender's neighbour entry so the traffic lands on the netconsole NIC
# (three NICs share one /24 here, so ARP replies come from any of them).
$SSH root@$SND "ip neigh replace $RX_IP lladdr $RX_MAC dev enp1s0 nud permanent"

# printk source: one line per iteration through /dev/kmsg, at a level below
# console_loglevel so that netconsole transmits every line.
$SSH root@$RCV "cat > /root/kmsg_flood.sh" <<FLOOD
#!/bin/bash
i=0
while :; do
	echo "<4>netpoll-repro \$i padding-padding-padding-padding-padding"
	i=\$((i + 1))
	sleep $KMSG_SLEEP
done > /dev/kmsg
FLOOD
$SSH root@$RCV "echo on > /proc/sys/kernel/printk_devkmsg
	pkill -x iperf3; for p in 5201 5202; do iperf3 -s -D -p \$p; done
	setsid nohup stress --cpu 4 --vm 2 --vm-bytes 256M --io 2 >/dev/null 2>&1 < /dev/null &
	setsid nohup bash /root/kmsg_flood.sh >/dev/null 2>&1 < /dev/null &"
sleep 2
$SSH root@$SND "for p in 5201 5202; do nohup iperf3 -c $RX_IP -p \$p --bidir -P $STREAMS -t $((MIN * 60)) >/dev/null 2>&1 & done"

END=$(( $(date +%s) + MIN * 60 ))
read -r _ last <<< "$(sample)"
while [ "$(date +%s)" -lt "$END" ]; do
	sleep 30
	read -r boot now <<< "$(sample)"
	if [ "$boot" != "$BOOT0" ]; then
		echo "!!!! $(ts) receiver ${boot:+rebooted}${boot:-stopped answering} -- waiting for kdump to finish"
		c=
		for i in $(seq 1 60); do
			read -r boot _ <<< "$(sample)"
			[ -n "$boot" ] && [ "$boot" != "$BOOT0" ] && c=$(crashes) && break
			sleep 10
		done
		if [ -n "$c" ] && [ "$c" -gt "$CR0" ]; then
			d=$($SSH root@$RCV 'ls -dt /var/crash/*/ | head -1')
			echo "!!!! $(ts) NEW CRASH $d"
			$SSH root@$RCV "grep -m3 -E 'BUG|general protection|stack segment|RIP' ${d}vmcore-dmesg.txt"
		else
			echo "!!!! $(ts) receiver did not come back with a new vmcore -- stop and check by hand"
		fi
		exit 2
	fi
	read -r seq unrecl < <($SSH root@$RCV "dmesg | grep netpoll-repro | tail -1 | sed 's/.*netpoll-repro \\([0-9]*\\).*/\\1/';
		awk '/SUnreclaim/ {print int(\$2 / 1024)}' /proc/meminfo" | tr '\n' ' ')
	echo "   $(ts) enp1s0 RX $(( (now - last) / 30 / 1048576 )) MB/s, kmsg seq $seq, SUnreclaim ${unrecl} MB"
	if [ "${unrecl:-0}" -gt 1024 ]; then
		echo "!!!! $(ts) netpoll backlog is growing (SUnreclaim ${unrecl} MB) -- lower the printk rate"
		exit 3
	fi
	last=$now
done
echo "== $(ts) clean after ${MIN} min"
