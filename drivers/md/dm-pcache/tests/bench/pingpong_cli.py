#!/usr/bin/env python3
# Client for the cross-VM round-trip (request-response) micro-benchmark.
#
# Spawns P parallel connections. Each connection keeps DEPTH requests in flight
# (a sliding window): it sends DEPTH messages, then for every reply received it
# sends one more, for DUR seconds. One round trip = one request + its reply.
#
#   DEPTH = 1  -> strictly synchronous (send, wait, send): models dm-pcache with no
#                 per-stream pipelining; P alone is the in-flight concurrency.
#   DEPTH > 1  -> pipelined: models dm-pcache qd-depth WITHIN a stream. Lets TCP
#                 coalesce several messages per cross-VM wakeup, so this directly
#                 tests the "amortize more bytes per round trip" (batched-ACK) lever.
#
# Total in-flight round trips = P * DEPTH. Maps to dm-pcache as: P ~ streams,
# DEPTH ~ qd-per-stream.
#
# A select() loop drives each connection so it can send and receive concurrently
# on one non-blocking socket. This avoids the deadlock a naive "send all DEPTH,
# then read" loop hits once DEPTH*SZ exceeds the socket buffers (both sides block:
# sender can't push, receiver isn't draining). SO_SNDBUF/SO_RCVBUF are also raised.
#
# Why this isolates the wall: a request-response per message forces a cross-VM
# round trip (send -> host delivers to peer VM + wakes its vCPU -> peer echoes ->
# host wakes our vCPU). Unlike streaming (iperf), the TCP window cannot hide the
# latency, so the saturating RT/s is the host's sustainable cross-VM round-trip
# rate, shared across all connections.
#
# Usage: pingpong_cli.py <host> <port> <parallel> <duration_s> [depth] [size_bytes]
import selectors
import socket
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])
P = int(sys.argv[3])
DUR = float(sys.argv[4])
DEPTH = int(sys.argv[5]) if len(sys.argv) > 5 else 1
SZ = int(sys.argv[6]) if len(sys.argv) > 6 else 131072

cnt = [0] * P
SOCKBUF = max(4 << 20, DEPTH * SZ * 2)


def worker(i):
    c = socket.create_connection((host, port))
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    c.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, SOCKBUF)
    c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, SOCKBUF)
    c.setblocking(False)
    sel = selectors.DefaultSelector()
    sel.register(c, selectors.EVENT_READ | selectors.EVENT_WRITE)

    data = memoryview(bytes(SZ))
    buf = bytearray(SZ)
    inflight = 0          # requests sent but not yet fully replied
    send_left = 0         # bytes still to write for the current outgoing request
    recv_have = 0         # bytes of the current reply received so far
    end = time.time() + DUR
    want_more = True      # may we start new requests?

    while True:
        if want_more and time.time() >= end:
            want_more = False
        # Top up the window: begin a new request if room and time remains.
        if want_more and send_left == 0 and inflight < DEPTH:
            send_left = SZ
            inflight += 1
        if not want_more and inflight == 0:
            break
        for key, mask in sel.select(timeout=1):
            if (mask & selectors.EVENT_WRITE) and send_left:
                try:
                    n = c.send(data[SZ - send_left:])
                    send_left -= n
                except BlockingIOError:
                    pass
            if mask & selectors.EVENT_READ:
                try:
                    n = c.recv_into(memoryview(buf)[recv_have:])
                except BlockingIOError:
                    n = -1
                if n == 0:
                    return
                if n > 0:
                    recv_have += n
                    if recv_have == SZ:    # one full reply collected
                        recv_have = 0
                        inflight -= 1
                        cnt[i] += 1
                        # loop will refill the window on the next iteration


ts = [threading.Thread(target=worker, args=(i,)) for i in range(P)]
t0 = time.time()
for t in ts:
    t.start()
for t in ts:
    t.join()
el = time.time() - t0
rt = sum(cnt)
print(f"P={P} depth={DEPTH} inflight={P * DEPTH} round_trips={rt} "
      f"rate={rt / el:.0f} RT/s  thr={rt * SZ / el / 1048576:.0f} MiB/s")
