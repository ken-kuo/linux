#!/usr/bin/env python3
# Echo server for the cross-VM round-trip (request-response) micro-benchmark.
#
# For each connection: read exactly SZ bytes, then write the same SZ bytes back.
# This mimics dm-pcache's per-IO ACK coupling (peer receives the write payload,
# then sends a reply) but with zero dm-pcache / PMEM / commit work in the path --
# so the measured rate is purely the host's cross-VM transport round-trip cost.
#
# Usage: pingpong_srv.py <port> [size_bytes]   (default size 131072 = 128 KiB)
import socket
import sys
import threading

port = int(sys.argv[1])
SZ = int(sys.argv[2]) if len(sys.argv) > 2 else 131072


def handle(c):
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = bytearray(SZ)
    while True:
        n = 0
        while n < SZ:                      # read a full SZ request
            r = c.recv_into(memoryview(buf)[n:])
            if r == 0:
                return
            n += r
        c.sendall(buf)                     # echo it back (the "ACK")


s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', port))
s.listen(64)
while True:
    conn, _ = s.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
