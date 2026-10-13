#!/bin/bash
# Give every node a BIO (non-DAX) cache device for the pcache-dist demos:
# a sparse image file on a loop device, reachable under one fixed name,
# /dev/pcache-cdev, so the generated harness (make_bio_harness.sh) can use
# it wherever the DAX runs use /dev/pmem0.
#
# A systemd oneshot (pcache-cdev.service) recreates the loop device and the
# link at boot, so demos that crash or restart a node (F35, vm restarts)
# find the cache device again. "down" removes the unit too.
#
# Usage: bio_cachedev.sh up|down|show
NODES=${NODES:-"192.168.124.3 192.168.124.50 192.168.124.60"}
IMG=/var/lib/pcache_cachedev.img
SIZE_MB=${SIZE_MB:-2560}	# > 1 GiB bitmap region + 2 * nr_cpus segments
LINK=/dev/pcache-cdev
UNIT=/etc/systemd/system/pcache-cdev.service

for n in $NODES; do
	case "${1:-show}" in
	up)
		ssh root@$n "set -e
			[ -f $IMG ] || truncate -s ${SIZE_MB}M $IMG
			loop=\$(losetup -j $IMG | cut -d: -f1 | head -1)
			[ -n \"\$loop\" ] || loop=\$(losetup -f --show $IMG)
			ln -sfn \$loop $LINK
			cat > $UNIT <<UNIT_EOF
[Unit]
Description=pcache BIO cache device for the demos ($LINK)
After=local-fs.target
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'l=\\\$(losetup -j $IMG | cut -d: -f1 | head -1); [ -n \"\\\$l\" ] || l=\\\$(losetup -f --show $IMG); ln -sfn \\\$l $LINK'
[Install]
WantedBy=multi-user.target
UNIT_EOF
			systemctl daemon-reload; systemctl enable -q pcache-cdev.service
			echo \"$n: $LINK -> \$loop (\$(blockdev --getsize64 \$loop) bytes), boot unit enabled\""
		;;
	down)
		ssh root@$n "systemctl disable -q pcache-cdev.service 2>/dev/null; rm -f $UNIT; systemctl daemon-reload
			rm -f $LINK
			for l in \$(losetup -j $IMG | cut -d: -f1); do losetup -d \$l; done
			rm -f $IMG; echo '$n: removed'"
		;;
	show)
		ssh root@$n "echo \"$n: \$(readlink $LINK) \$(losetup -j $IMG)\""
		;;
	esac
done
