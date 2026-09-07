set architecture i386:x86-64:intel
target remote localhost:12349
file ./vmlinux
add-auto-load-safe-path .
source ./vmlinux-gdb.py
#add-symbol-file ./drivers/nvme/host/nvme.ko 0xffffffffc01ac000
#add-symbol-file ./drivers/nvme/host/nvme-core.ko 0xffffffffc0181000
#add-symbol-file ./drivers/block/virtio_blk.ko 0xffffffffc009f000

