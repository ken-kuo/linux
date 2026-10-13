#!/bin/bash
# One-key prep for the RAM-backed pmem rig. Run on the HOST.
#
# Why this exists: the VMs' emulated nvdimm (/dev/pmem0) is backed by tmpfs files
# on the host (/dev/shm/daxN.img), NOT the consumer SSD file it used to be
# (/imgs/daxN.img on an NVMe). The SSD backing threw guest MOVNT writes onto host
# page-cache -> disk writeback, throttling "pmem" to ~1.1 GB/s and drifting with
# SSD (SLC-cache) state, which can be mistaken for a throughput ceiling or for
# host degradation. RAM backing = memory-speed, stable, no disk IO.
#
# Two things must be (re)established for dm-pcache to run:
#   (a) host, after a HOST reboot (tmpfs cleared): recreate the 2 GiB backing files
#       BEFORE the VMs can start.
#   (b) guest, after every VM (re)start (fresh tmpfs -> "raw", label-less namespace):
#       convert /dev/pmem0 to fsdax, else dm-pcache's dax_direct_access -> -EOPNOTSUPP.
#
# This script does (a), starts the VMs if needed, then does (b). Idempotent.
#
# Prereq: the domain XML must already point the nvdimm <source><path> at
# /dev/shm/daxN.img with NO <label> (so the 2 GiB region is 16 MiB-aligned and
# fsdax-capable). Those definitions are persisted in libvirt. To revert to the
# persistent SSD backing, virsh-define the /imgs/daxN.img XML instead.
#
# Usage: bench/pmem_prep.sh
set -uo pipefail
cd "$(dirname "$0")"
source ./common.sh
rsh() { command ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@"$1" "$2"; }

DAX_SIZE=2147483648            # 2 GiB — must match <target><size> in the domain XML
# node IP -> (libvirt domain, host tmpfs backing file)
declare -A DOMAIN=( [$NODE1_IP]=fedora-clone [$NODE2_IP]=fedora-clone-2 )
declare -A DAXFILE=( [$NODE1_IP]=/dev/shm/dax.img [$NODE2_IP]=/dev/shm/dax2.img )

# Common case: both VMs already up -> skip all host/sudo steps, go straight to
# fsdax. Only when a node is unreachable (e.g. after a HOST reboot: VMs off, tmpfs
# cleared) do we touch the host (needs sudo: recreate backing files + virsh start).
up() { ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no root@"$1" true 2>/dev/null; }
if up "$NODE1_IP" && up "$NODE2_IP"; then
	echo "== both VMs reachable — skipping host prep =="
else
	echo "== a node is down: host prep (recreate tmpfs backing + start VMs) [needs sudo] =="
	for ip in $NODE1_IP $NODE2_IP; do
		f=${DAXFILE[$ip]}
		if [ -f "$f" ] && [ "$(stat -c%s "$f" 2>/dev/null)" = "$DAX_SIZE" ]; then
			echo "  $f ok"
		else
			echo "  creating $f (2 GiB tmpfs, qemu-owned)"
			sudo truncate -s "$DAX_SIZE" "$f"
			sudo chown qemu:qemu "$f"; sudo chmod 600 "$f"
		fi
	done
	for ip in $NODE1_IP $NODE2_IP; do
		dom=${DOMAIN[$ip]}
		[ "$(sudo virsh domstate "$dom" 2>/dev/null)" = "running" ] || \
			{ echo "  starting $dom"; sudo virsh start "$dom" >/dev/null; }
	done
	echo "  waiting for SSH..."
	for ip in $NODE1_IP $NODE2_IP; do
		for _ in $(seq 1 40); do up "$ip" && break; sleep 3; done
	done
fi

echo "== ensure /dev/pmem0 namespace is fsdax (each VM boot resets tmpfs -> raw) =="
ok=1
for ip in $NODE1_IP $NODE2_IP; do
	mode=$(rsh "$ip" "ndctl list -N 2>/dev/null | grep -o '\"mode\":\"[a-z]*\"' | head -1")
	if echo "$mode" | grep -q fsdax; then
		echo "  [$ip] already fsdax"
	else
		echo "  [$ip] namespace is ${mode:-unknown} -> converting to fsdax"
		rsh "$ip" "dmsetup remove pcache-bench 2>/dev/null; rmmod dm_pcache 2>/dev/null;
			   ndctl create-namespace -f -e namespace0.0 --mode=fsdax >/dev/null 2>&1"
		now=$(rsh "$ip" "ndctl list -N 2>/dev/null | grep -o '\"mode\":\"[a-z]*\"' | head -1")
		echo "    [$ip] now: ${now:-FAILED}"
		echo "$now" | grep -q fsdax || ok=0
	fi
done

[ "$ok" = 1 ] && echo "pmem rig ready (RAM-backed, fsdax on both nodes)." || \
	{ echo "!!! fsdax conversion failed on a node — check ndctl / domain XML alignment."; exit 1; }
