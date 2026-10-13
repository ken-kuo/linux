#!/bin/bash
# Trace every bio written to segment 0 of the BIO cache device (the start of
# the key log), whoever issues it. Run on a node: trace_seg0_writes.sh on|off|dump <sector>
T=/sys/kernel/tracing
SEG0_SECT=$(( (28*1024 + (1<<30) + 12*1024) / 512 ))
SEG_SECTS=$(( 16 * 1024 * 1024 / 512 ))
dev=$(readlink -f /dev/pcache-cdev)
maj=$(( 0x$(stat -c %t "$dev") )); min=$(( 0x$(stat -c %T "$dev") ))
case "$1" in
on)
	echo 0 > $T/tracing_on
	echo > $T/trace
	echo 262144 > $T/buffer_size_kb
	echo "dev == $(( (maj << 20) | min )) && sector >= $SEG0_SECT && sector < $((SEG0_SECT + SEG_SECTS))" \
		> $T/events/block/block_bio_queue/filter
	echo 1 > $T/events/block/block_bio_queue/enable
	echo 1 > $T/tracing_on
	echo "tracing writes to $dev ($maj:$min) sectors $SEG0_SECT..+$SEG_SECTS"
	;;
off)
	echo 0 > $T/events/block/block_bio_queue/enable
	echo 0 > $T/tracing_on
	;;
dump)	# dump <byte offset of the hole on the device>
	s=$(( $2 / 512 ))
	grep -E " W[A-Z]* " $T/trace | awk -v s=$s '{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/ && $(i+1)=="+") {st=$i; n=$(i+2); if (s>=st && s<st+n) print; break}}' | tail -20
	;;
esac
