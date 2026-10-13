# pcache-dist 功能参考

pcache-dist 的功能清单。每项功能包含：
- **原理**：解决什么问题 + 设计思路
- **实现**：关键文件 / 函数 / 数据结构 / wire protocol（如适用）
- **Demo script**：可直接运行的 bash 指令，验证该 feature 行为

运行环境前提（所有 demo 通用）：
- 模块由本内核树编译
- 3 个节点：node-1（id=0）、node-2（id=1）、node-3（id=2），地址分别记为 `$NODE1`、`$NODE2`、`$NODE3`
  （与 `tests/bench/common.sh` 中的 `NODE1_IP` 等一致）
- 每个节点有 `/dev/pmem0`，模块部署为 `/root/dm-pcache.ko`
- 下文的 `demo.sh` 指 `tests/dm_pcache_phase1_demo.sh`，`demo_features.sh` 指 `tests/demo_features.sh`
- 开始任何 demo 前先 `bash demo.sh reset` 确保 PMEM 干净

---

## 目录

### Section 1 — Replication Path
- [F01](#f01-synchronous-write-replication-protocol-c) Synchronous write replication (Protocol C)
- [F02](#f02-multi-peer-fan-out-broadcast-write) Multi-peer fan-out broadcast write
- [F03](#f03-zero-copy-network-transport-msg_splice_pages) Zero-copy network transport (MSG_SPLICE_PAGES)
- [F04](#f04-ack-timeout-reaping-and-auto-reconnect) ACK timeout reaping and auto-reconnect
- [F05](#f05-disconnected-peer-fallback-bitmap-dirty) Disconnected peer fallback (bitmap-dirty)
- [F06](#f06-req_preflush-cross-cluster-flush-barrier) REQ_PREFLUSH cross-cluster flush barrier

### Section 2 — Network & Mesh
- [F07](#f07-full-mesh-accept-dispatch) Full-mesh accept dispatch (per-network listen and handshake routing)
- [F08](#f08-id-based-connection-direction) id-based connection direction
- [F09](#f09-so_keepalive-driven-dead-link-detection) SO_KEEPALIVE-driven dead-link detection

### Section 3 — Dirty Tracking & Resync
- [F10](#f10-per-peer-persistent-dirty-bitmap-on-pmem) Per-peer persistent dirty bitmap on PMEM
- [F11](#f11-bitmap-based-incremental-resync) Bitmap-based incremental resync
- [F12](#f12-bilateral-bitmap-merge-p_bitmap-and-enc_run) Bilateral bitmap merge (P_BITMAP and ENC_RUN)
- [F13](#f13-generation-counter-and-per-peer-peer_gen_at_sync) Generation counter and per-peer peer_gen_at_sync

### Section 4 — Persistence & Crash Recovery
- [F14](#f14-pmem-superblock-and-double-copy-crc-headers) PMEM superblock and double-copy CRC headers
- [F15](#f15-persistent-peer-registry-32-slots) Persistent peer registry (32 slots)
- [F16](#f16-resync_in_progress-crash-detection) RESYNC_IN_PROGRESS crash detection
- [F17](#f17-idempotent-add-peer-registry-recovery) Idempotent add-peer (registry recovery)

### Section 5 — HA / Role Management
- [F18](#f18-primarysecondary-role-and-write-fencing) Primary/Secondary role and write fencing
- [F19](#f19-set-role-runtime-role-switch) `set-role` runtime role switch
- [F20](#f20-coordinated-demote-handshake-p_demote_reqack) Coordinated demote handshake (P_DEMOTE_REQ/ACK)
- [F21](#f21-worker-kthread-role-swap) Worker kthread role swap

### Section 6 — Quorum & Split-brain Safety
- [F22](#f22-quorum-check-drbd-style-local-no-election) Quorum check (DRBD-style local, no election)
- [F23](#f23-configurable-quorum-off-majority-n) Configurable quorum (off, majority, N)
- [F24](#f24-split-brain-per-pair-detection) Split-brain per-pair detection
- [F25](#f25-split-brain-manual-resolve-victim-survivor) Split-brain manual resolve (victim, survivor)

### Section 7 — Operational Interface
- [F26](#f26-dmsetup-table-line-config) dmsetup table line config
- [F27](#f27-add-peer-remove-peer-list-peers-messages) add-peer, remove-peer, list-peers messages
- [F28](#f28-abort-resync-force-resync-reset-generation) abort-resync, force-resync, reset-generation
- [F29](#f29-status-reporting-status-and-bitmap-status) Status reporting (status and bitmap-status)
- [F30](#f30-clean-pmem-reset-script) Clean PMEM reset script

### Section 8 — Performance / Multi-stream
- [F31](#f31-multi-stream-replication-streamsn) Multi-stream replication (`streams=N`)
- [F32](#f32-multi-stream-churn-robustness-kref-socket-lifetime) Multi-stream churn robustness (kref socket lifetime)
- [F33](#f33-deferred-write-replication-cache-pressure-correctness-fix) Deferred-write replication (cache-pressure correctness fix)

---

# Section 1 — Replication Path

## F01: Synchronous write replication (Protocol C)

### 原理
每笔 WRITE bio 必须在**所有当前连接的 peer** ACK 后才回应 user-space。确保任一节点当机数据不丢。
- Protocol C = 严格同步（DRBD 同款）
- 不支持 Protocol A (async) / B (memory-sync)

### 实现
- `drivers/md/dm-pcache/dm_pcache.c::dm_pcache_map_bio`：bio 进来先写本地 cache，成功后若 `is_primary && WRITE`，调用 `pcache_net_submit_write` fan-out
- `dm_pcache_state.c::pcache_net_submit_write`：每 connected peer 配一个
  `pcache_pending_req`，依 `hash_64(sector,32) % nr_streams` 选一条 stream 加进
  **`stream->pending_list`**（pending 为 per-stream；`streams=1` 时即
  `streams[0]`，[F31](#f31-multi-stream-replication-streamsn)）；caller 用 `pcache_req_get` 追加 refcount
- send 与 recv 分属不同 kthread：`pcache_primary_worker`（control stream）/
  `pcache_data_sender`（data stream）drain `pending_list` → send `P_DATA`；
  `P_ACK` 由独立的 `pcache_primary_receiver` / `pcache_data_receiver` kthread 收
- `pcache_handle_ack(struct pcache_stream *stream, …)`：每 ACK `pcache_req_put`，
  refcount 归零时 bio_endio
- Wire packet: `PCACHE_P_DATA` + payload + `PCACHE_F_ACK` flag

### Demo
```bash
bash demo.sh reset
bash demo.sh start      # 2-node setup: node-1 Primary, node-2 Secondary

# Primary 写 4 MiB，conv=fsync 强制完成
ssh root@$NODE1 'dd if=/dev/urandom of=/tmp/in.bin bs=1M count=4 status=none
                        md5sum /tmp/in.bin
                        dd if=/tmp/in.bin of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct conv=fsync status=none'

# Cross-node MD5 — 两端应完全一致
ssh root@$NODE1  'dd if=/dev/mapper/pcache-dist0 bs=1M count=4 iflag=direct status=none | md5sum'
ssh root@$NODE2 'dd if=/dev/mapper/pcache-dist0 bs=1M count=4 iflag=direct status=none | md5sum'

# 预期：两个 MD5 完全一致 = Protocol C 同步成功
```

---

## F02: Multi-peer fan-out broadcast write

### 原理
Primary 对所有 connected peer 同时 fan-out 一笔 bio。每 peer 收到独立 `pcache_pending_req`，独立 ACK。bio 等所有 connected peer ACK 才完成。
- All-connected ACK 语义（vs all-configured）
- Disconnected peer 走 [F05](#f05-disconnected-peer-fallback-bitmap-dirty) bitmap-dirty fallback，不阻塞 IO

### 实现
- `pcache_net_submit_write` 迭代 `for_each_active_peer`：CONNECTED 的 enqueue + 第 2+ peer `pcache_req_get`；SKIPPED 的调用 `pcache_bm_set_range(peer->bm, ...)` 标 dirty
- 每 peer 的 pending 互不影响；pending 是 **per-stream**（`stream->pending_list`
  + `stream->pending_cnt`），同一 peer 内再依 `hash_64(sector)%nr_streams` 分流
  （[F31](#f31-multi-stream-replication-streamsn)）。per-peer 各自独立 ACK 的 fan-out 语义不变
- Refcount: caller 1 + (M-1) extra get → M puts (from M ACKs) → bio_endio

### Demo
```bash
bash demo.sh reset
bash demo.sh start3     # 3-node mesh: node-1 Primary, node-2/3 Secondary

# 4 MiB 写从 Primary
ssh root@$NODE1 'dd if=/dev/urandom of=/tmp/in.bin bs=1M count=4 status=none
                        md5sum /tmp/in.bin
                        dd if=/tmp/in.bin of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct conv=fsync status=none'
sleep 3

# 三端 MD5 应全相同
for n in $NODE1 $NODE2 $NODE3; do
  echo "$n: $(ssh root@$n 'dd if=/dev/mapper/pcache-dist0 bs=1M count=4 iflag=direct status=none | md5sum')"
done
# 预期：3 个 MD5 全部一致
```

---

## F03: Zero-copy network transport (MSG_SPLICE_PAGES)

### 原理
复制 bio payload 时不经过中间 buffer 复制。直接从 bio_vec 的 page 通过
`MSG_SPLICE_PAGES` flag 推到 TCP send queue。`MSG_SPLICE_PAGES` 是 kernel 网络层的
send flag（约 v6.5 引入，取代已移除的 `kernel_sendpage()`）。

**一般 copy 路径 vs splice 路径：**

```
# 不带 flag（一般 sendmsg）：
sock_sendmsg → tcp_sendmsg 配置 skb buffer
             → memcpy / skb_copy_to_page  ← 把 bio page 数据「复制」进新配的 skb 内存
             → NIC 从 skb 内存 DMA
# 每送一笔 128K 就有一次 128K memcpy，纯 CPU 成本 + 污染 cache。

# 带 MSG_SPLICE_PAGES：
sock_sendmsg → skb_splice_from_iter
             → get_page() 对来源 page 取参考          ← 不复制 payload
             → 直接把该 page 挂成 skb fragment(frags)
             → NIC 从「原本的 bio page」DMA
             → TX 完成后 put_page() 放掉参考
```

payload 一个 byte 都不搬，只是把 page refcount +1 挂进 skb，NIC 直接从原 bio page
DMA 出去。

**三个前提（dm-pcache 为何能安全使用）：**
1. 来源必须是真正的 page（`ITER_BVEC` over real pages），不能是 slab / stack 内存
   —— bio 的 segment 正好都是 page，符合。
2. page 在 TX 完成前必须稳定不被改写或释放 —— socket 持有 page ref 直到送完；
   dm-pcache 的 bio 靠 kref 卡在 ACK 回来才 `bio_endio`（[F01](#f01-synchronous-write-replication-protocol-c)），
   送出期间 bio page 必定还在。
3. 走 TCP `sock_sendmsg`（非已移除的 `kernel_sendpage`）。

- 对齐 DRBD 9 的 `_drbd_send_page()` 设计：DRBD 早期用 `sendpage` 做 zero-copy，新
  kernel 拿掉 `sendpage` 后，等价作法即 `sendmsg + MSG_SPLICE_PAGES`。
- **省的是「payload 进 skb 的那次 memcpy」**，不是完全没有任何数据搬动（checksum 通常
  仍由 NIC offload，实际 byte 搬移由 DMA 从原 page 直接做）。故大页写入（128K / 1M）
  CPU 才明显下降；小 IO 因 per-send 固定成本（skb / page 管理）反而看不太出来。

### 实现
- `dm_pcache_net.c::pcache_send_bio_pages`：
  ```c
  bio_for_each_segment(bvec, bio, iter) {
      bvec_set_page(&out, page, len, offset);
      iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &out, 1, len);
      msg.msg_flags = MSG_NOSIGNAL | MSG_SPLICE_PAGES;
      sent = sock_sendmsg(sock, &msg);
  }
  ```

### Demo
```bash
bash demo.sh reset && bash demo.sh start

# 观察 perf top 期间做大 IO，确认 cpu copy 路径没进来
ssh root@$NODE1 'taskset -c 0 dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=1M count=200 oflag=direct status=progress'

# 注：send 路径无 per-IO 日志（pcache_send_bio_pages 只回错误码，不打印），
# 故不靠 dmesg 验证 —— 直接看 dd 吞吐：zero-copy 下 200 MiB 应 < 2 sec、
# 且 perf top 不应出现 payload memcpy 路径。
# 预期：高速 throughput（200 MiB < 2 sec）
```

---

## F04: ACK timeout reaping and auto-reconnect

### 原理
网络断线但 TCP 还没声明死亡时，发出去的 `P_DATA` 等不到 `P_ACK`。设 30 秒 timeout：超时请求视为失败 → 标 bitmap dirty + 完成 bio with EIO + 强制 socket shutdown 触发 reconnect。

### 实现
- `dm_pcache_state.c::pcache_primary_worker` 的 send-loop 开头（多串流下
  pending 队列为 **per-stream**，timeout 侦测在 control stream `streams[0]`）：
  ```c
  spin_lock(&peer->streams[0].pending_lock);
  if (atomic_read(&peer->streams[0].pending_cnt) > 0) {
      unsigned long deadline = jiffies - PCACHE_ACK_TIMEOUT_SECS * HZ;
      list_for_each_entry_safe(req, tmp, &peer->streams[0].pending_list, list) {
          if (!req->sent)                          continue;  // 只收已送出、等 ACK 的
          if (time_after_eq(req->send_time, deadline)) continue;
          list_del(&req->list);
          atomic_dec(&peer->streams[0].pending_cnt);
          pcache_bm_set_range(peer->bm, req->sector, req->size);  // 先标 dirty 供 resync
          pcache_req_put(req->pcache_req, -ETIMEDOUT);            // 完成 bio with EIO
          pcache_pending_req_free(peer, req);
          any_timed_out = true;
      }
  }
  spin_unlock(&peer->streams[0].pending_lock);

  if (any_timed_out)
      pcache_conn_shutdown(cst);   // kref conn helper（[F32](#f32-multi-stream-churn-robustness-kref-socket-lifetime)）；
                                   // 唤醒 receiver -ECONNRESET → state=CONNECTING → 回 connect:
  ```
- **多串流行为**：侦测只在 `streams[0]`；触发 `pcache_conn_shutdown` → peer 转
  `CONNECTING` → 重连时各 stream 的 `pending_list` 走 fault drain（`list_splice_init`）
  逐笔完成。data stream（`streams[1..N-1]`）的 sender 自身不独立 reap。
- `PCACHE_ACK_TIMEOUT_SECS = 30`（`dm_pcache_net.h:145`，未变）
- Heartbeat: 5 秒 `wait_event_interruptible_timeout(peer->streams[0].wait_for_work, …, 5 * HZ)`
  确保 reap 不被无 IO 卡住

### Demo
```bash
bash demo.sh reset && bash demo.sh start

# 在 Secondary 用 nft drop port 9764 模拟静默断线
ssh root@$NODE2 'nft add table inet block 2>/dev/null
                          nft add chain inet block input { type filter hook input priority 0\; } 2>/dev/null
                          nft add rule inet block input tcp dport 9764 drop'

# Primary 写数据，预期 ~30s 后超时 → EIO
ssh root@$NODE1 'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct status=none; echo exit=$?'
# 预期：exit != 0；dmesg 显示 "ACK timeout — forcing reconnect"

ssh root@$NODE1 'dmesg | grep -E "ACK timeout|primary recv" | tail -5'

# 恢复
ssh root@$NODE2 'nft delete table inet block'
```

---

## F05: Disconnected peer fallback (bitmap-dirty)

### 原理
Fan-out 时若某 peer 不在 CONNECTED 状态，不阻塞 bio：标该 peer 的 per-peer bitmap dirty，bio 对其他 peer 仍同步进行。Peer 重连后走 [F11](#f11-bitmap-based-incremental-resync) 增量 resync 补齐。

### 实现
- `pcache_net_submit_write` skip 分支：
  ```c
  for_each_active_peer(net, peer, slot) {
      if (peer->state != PCACHE_C_CONNECTED) {
          if (peer->bm)
              pcache_bm_set_range(peer->bm, sector, size);   // per-peer dirty
          any_skipped = true;
          continue;
      }
      // enqueue to this peer
  }
  ```
- `dm_pcache.c::pcache_mark_all_peers_dirty`：当 0 peer connected 时走完整 fallback

### Demo

> 注意：要演的是「peer 还配置着、但**断线**」的 fallback —— 此时 peer 仍在
> `net->peers[]`、`peer->bm` 仍在，fan-out 才会走 skip 分支标 dirty。**不要用
> `remove-peer`**：那是把 peer 整个释放（`net->peers[slot]=NULL`、`peer->bm` free），
> slot 都不存在了，写入根本不迭代到它，也就无从标 dirty。

```bash
bash demo.sh reset && bash demo.sh start3
sleep 8

# 先写 2 MiB 并同步，让 node-3 起点是最新的
ssh root@$NODE1 'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=2 oflag=direct conv=fsync status=none'
sleep 3

# 让 node-3 静默断线：nft drop 它的 inbound port（slot 与 bm 都保留，仅 state 翻成非 CONNECTED）
ssh root@$NODE3 'nft add table inet block 2>/dev/null
                          nft add chain inet block input { type filter hook input priority 0\; } 2>/dev/null
                          nft add rule inet block input tcp dport 9764 drop'
sleep 55   # 等 ACK timeout / keepalive 完成断线检测

# Primary 写 2 MiB at offset 2M —— fan-out 同步到 node-2；node-3 走 skip 分支标 bm dirty
ssh root@$NODE1 'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=2 seek=2 oflag=direct conv=fsync status=none'

# bitmap-status：node-2 slot clean，node-3 slot 仍在且 dirty 增加（这才是 F05 的 bitmap-dirty fallback）
ssh root@$NODE1 'dmsetup message pcache-dist0 0 bitmap-status'
ssh root@$NODE1 'dmsetup status pcache-dist0'

# 解除阻挡 → 自动重连 → F11 增量 resync 只补 dirty 的 2 MiB（因为断线期间一直有 bitmap 追踪，非全量）
ssh root@$NODE3 'nft delete table inet block'
sleep 15

# 验证 node-3 已补齐：与 Primary MD5 一致
ssh root@$NODE1  'dd if=/dev/mapper/pcache-dist0 bs=1M count=4 iflag=direct status=none | md5sum'
ssh root@$NODE3 'dd if=/dev/mapper/pcache-dist0 bs=1M count=4 iflag=direct status=none | md5sum'
# 预期：两端 MD5 一致；dmesg resync 日志显示只送 dirty 范围（增量，非 131072 bits 全量）
```

---

## F06: REQ_PREFLUSH cross-cluster flush barrier

### 原理
`dd conv=fsync` / `fsync(2)` 触发 REQ_PREFLUSH bio（size=0），barrier 语义、不带数据，要求「此前已完成的写入都持久」。dm-pcache 的关键：**数据每笔即时落盘，但 metadata（cache key）批次惰性刷盘** —— 写入时数据 `memcpy_flushcache` 立即进 PMEM，但 key 进 kset、只有 ① FUA ② kset 满 ③ 延迟 1s 的 `flush_work` ④ 显式 `pcache_cache_flush` 才落 PMEM。crash 在 key 落盘前 → 数据成孤儿、该笔写入恢复时丢失。
- **本地**：REQ_PREFLUSH → `pcache_cache_flush` 强制 close 所有 kset → 此前写入的 key 落 PMEM → crash-recoverable。
- **跨集群（`d7d704bbf904`）**：仅本地 flush 不够。secondary 同样「数据即时、metadata 惰性」，且 ACK 在 `cache_key_append(.., false)` 之后、kset 刷盘之前送出（`pcache_handle_data_recv`）。所以一笔已被用户 fsync 的写入，在 secondary 仍可能 metadata 未持久；若 secondary 此时掉电崩溃，`cache_replay` 停在未写的 kset → 该 key 不重建 → 写入在 secondary 静默丢失，而 primary 视其为 clean、永不 resync。
  修正：primary 本地 flush 后，向每个 connected peer 的 **control stream[0]** 送 size=0 + `PCACHE_F_FLUSH`；peer 跑 `pcache_cache_flush`（刷其全部 kset，覆盖所有 stream 写入的 key）后 ACK；flush bio 等所有 connected peer ACK 才完成（Protocol C）。disconnected peer 跳过（靠 dirty bitmap 在重连时 resync）。
- secondary 对 `F_FLUSH` 显式回 ACK；若不回 ACK，primary 会等到 30 s ACK timeout 并以 `Connection timed out` 失败。

### 实现
- 协议：`PCACHE_F_FLUSH`（dm_pcache_protocol.h）；`pcache_pending_req.is_flush`。
- primary `dm_pcache.c::pcache_submit_req`，本地 `pcache_cache_handle_req`（含 REQ_PREFLUSH→cache_flush）之后：
  ```c
  if ((bio->bi_opf & REQ_PREFLUSH) && pcache->is_primary &&
      pcache_peer_connected(pcache)) {
      pcache_req_get(pcache_req);
      if (pcache_net_submit_flush(pcache, pcache_req))
          pcache_req_put(pcache_req, 0);   // 无 peer 入队；本地已持久
  }
  ```
- `pcache_net_submit_flush`（dm_pcache_state.c）：仅 connected peer、仅 stream[0]，入队 `is_flush` 的 size=0 pending_req；send-loop 送 `PCACHE_F_ACK|PCACHE_F_FLUSH`、跳过 bio pages。
- secondary `pcache_handle_data_recv`：在 `if (!size) return 0` **之前**拦截 `F_FLUSH` → `pcache_cache_flush` → 回 ACK/NACK（block_id 回显，复用既有 ACK 匹配 + Protocol C refcount）。

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# fsync 现在是跨集群 barrier：本地 flush + 各 connected peer flush 后才完成
ssh root@$NODE1 'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct conv=fsync status=none 2>&1; echo exit=$?'
# 预期：exit=0、无 "Connection timed out"（secondary 对 flush 显式回 ACK，不再超时）
# 注：fsync 现在会做一次网络 round-trip（送 flush + 收 ACK），正常链路下毫秒级完成
ssh root@$NODE2 'dmesg | grep -i "cache flush failed" | tail -3'   # 预期：无错误
```

---

# Section 2 — Network & Mesh

## F07: Full-mesh accept dispatch

### 原理
N 节点 cluster 共 N*(N-1)/2 个 TCP 连接（多串流下再 ×nr_streams）。每节点开
**一个** listen socket，accept 后读 handshake 拿 `(peer_node_id, stream_id)`，用这对
值路由到对应 peer 的对应 stream slot。
- 避免 per-peer listen 浪费 port + 无法做动态 add-peer

### 实现
- `pcache_network` 结构添加 `listen_sock` + `accept_thread`
- `dm_pcache_state.c::pcache_accept_thread`（以 `(node_id, stream_id)` 路由
  + `pcache_conn_install` 安装到 stream slot）：
  ```c
  while (...) {
      kernel_accept(net->listen_sock, &sock, 0);
      pcache_recv_handshake(sock, &peer_node_id, &stream_id);   // 握手带 stream_id
      pcache_send_handshake(sock, my_node_id, stream_id);        // reply

      for_each_active_peer(net, peer, slot)                      // 1) 依 node_id 找 peer
          if (peer->node_id == peer_node_id) break;
      if (!peer)                            { sock_release; continue; }  // 未配置 → drop
      if (stream_id >= peer->nr_streams)    { sock_release; continue; }  // 非法 stream_id
      if (pcache_stream_has_conn(&peer->streams[stream_id]))            // race：对端先连到了
                                            { sock_release; continue; }  // → drop dup

      pcache_conn_install(&peer->streams[stream_id], sock);     // 2) 装到 stream slot(kref)
      if (stream_id == 0)
          peer->state = PCACHE_C_CONNECTED;                     //    control stream 拥有 peer->state

      if (stream_id == 0)                                       // 3) 唤醒对应 worker
          wake_up(&peer->streams[0].wait_for_socket);           //    control：per-peer worker 驱动
      else
          pcache_data_stream_start(&peer->streams[stream_id]);  //    data：responder 端直接起 worker
  }
  ```
- 拒绝未配置的 node_id（log "no peer slot for node_id=X; drop"）或非法 stream_id
  （"bad stream_id=X (nr_streams=N)"）

### Demo
```bash
bash demo.sh reset && bash demo.sh start3

# dmesg 看 accept dispatch 的 routing 消息
for n in $NODE1 $NODE2 $NODE3; do
  echo "--- $n ---"
  ssh root@$n 'dmesg | grep -E "accept: handed inbound|accept: .*drop" | tail -5'
done
# 预期：每节点 accept 后正确匹配到 peer slot
```

---

## F08: id-based connection direction

### 原理
**Lower node_id 永远是 initiator (主动 connect)**，higher node_id 永远 wait for accept。避免双向同时 connect 竞赛 + 与 Primary/Secondary 角色解耦（重要：Secondary↔Secondary 连接也需要方向决定）。

### 实现
- Helper: `pcache_peer_is_initiator(peer) = (pcache->my_node_id < (u32)peer->node_id)`
- `primary_worker` / `secondary_worker` 开头都 check initiator 决定 connect-or-wait 分支
- 对称套用到 `pcache_exchange_sync_params(..., initiator)` 和 `pcache_exchange_bitmaps(..., initiator)`

### Demo
```bash
bash demo.sh reset && bash demo.sh start3
sleep 5

# 每节点 dmesg 显示自己的角色 (initiator / responder)
for n in $NODE1 $NODE2 $NODE3; do
  echo "--- $n ---"
  ssh root@$n 'dmesg | grep -E "handshake complete \\((initiator|responder)\\)" | tail -5'
done
# 预期：
#   node-1 (id=0): 对 node-2, node-3 都是 (initiator) — lower id 主动连
#   node-2 (id=1): 对 node-1 是 (responder)，对 node-3 是 (initiator)
#   node-3 (id=2): 对 node-1, node-2 都是 (responder)
```

---

## F09: SO_KEEPALIVE-driven dead-link detection

### 原理
TCP keepalive 在 ~50 秒内侦测 silent failure（peer 防火墙 drop 但无 RST）。配合 [F04](#f04-ack-timeout-reaping-and-auto-reconnect) ACK timeout 双重保护。

### 实现
- `dm_pcache_net.c::pcache_apply_socket_opts` 套到 connected 和 accepted sockets：
  ```c
  TCP_KEEPIDLE  = 20s   // 闲置 20s 后第一次 probe
  TCP_KEEPINTVL = 10s   // 之后每 10s probe
  TCP_KEEPCNT   = 3     // 3 次 probe 失败才声明死
  // → 最坏 ~50s 侦测
  ```

### Demo
```bash
bash demo.sh reset && bash demo.sh start

# Secondary 用 nft 完全 drop 入流
ssh root@$NODE2 'nft add table inet block 2>/dev/null
                          nft add chain inet block input { type filter hook input priority 0\; } 2>/dev/null
                          nft add rule inet block input tcp dport 9764 drop'

# 等 ~50 秒，Primary 应自动侦测到断线
echo "Waiting 55s for SO_KEEPALIVE detection..."
sleep 55
ssh root@$NODE1 'dmesg | grep -E "ACK recv failed|connection lost|repl_state.*OFF" | tail -5'
# 预期：dmesg 显示 ACK recv failed -104 (ECONNRESET) 或 -110 (ETIMEDOUT)

ssh root@$NODE2 'nft delete table inet block'
```

---

# Section 3 — Dirty Tracking & Resync

## F10: Per-peer persistent dirty bitmap on PMEM

### 原理
每 peer 一个独立 bitmap，记录「对该 peer 哪些 4 KiB block 过时」。Bitmap 持久化在 PMEM cache_dev 的对应 slot，跨 reboot 幸存。
- 32 个 slot 共占 1 GiB PMEM（每 slot 32 MiB → 支持 ≤4 TiB backing per peer）
- 状态为 per-peer（不在设备内共享）

### 实现
- `struct pcache_bitmap` 改为 per-peer，字段 `peer` back-ref + `slot_idx` + `pmem_base`/`pmem_data`
- `struct pcache_peer` 添加 `struct pcache_bitmap *bm`
- `pcache_peer_bitmap_init(peer, backing_sectors, slot_idx)` 在 `pcache_add_peer` 内调用
- PMEM layout per slot：8 KiB header (2× double-copy CRC) + 32 MiB - 8 KiB bitmap data
- Header struct: `struct pcache_bm_header_onmedia` (magic + generation + peer_gen_at_sync + flags + CRC)
- 更新逻辑：`pcache_bm_set_range` 改 RAM working copy + 标 page dirty；`pcache_bm_flush_sync` 把 dirty pages `memcpy_flushcache` 到 PMEM

### Demo
```bash
bash demo.sh reset && bash demo.sh start

# 写数据 → 触发 bitmap dirty (因 fsync 完成 cache 落盘后 bitmap 是 clean 的，这个示范用 force-resync)
ssh root@$NODE1 'dmsetup message pcache-dist0 0 force-resync'
sleep 2

# 看 PMEM 上 slot 0 header magic (应为 PCACMB1\3 = 0x504341434d423103 LE = "03 31 42 4d 43 41 43 50")
ssh root@$NODE1 'dd if=/dev/pmem0 bs=1 skip=$((0x7000+8)) count=8 status=none | xxd'
# 预期：03 31 42 4d 43 41 43 50

# 重启 module → 看 bitmap_init 从 PMEM 加载
ssh root@$NODE1 'dmsetup remove pcache-dist0; rmmod dm_pcache; insmod /root/dm-pcache.ko
                        LOOP=$(losetup -j /var/lib/pcache_backing.img | cut -d: -f1 | head -1)
                        SIZE=$(blockdev --getsz $LOOP)
                        dmsetup create pcache-dist0 --table "0 $SIZE pcache-dist /dev/pmem0 $LOOP my_node_id=0 is_primary=1 listen_port=9764"
                        dmsetup message pcache-dist0 0 add-peer 1 '"$NODE2"':9764
                        dmesg | grep -E "bitmap\\[slot" | tail -3'
# 预期：dmesg 显示 "loaded gen=X peer_gen=Y dirty=Z/N" — 跨 reboot 保留
```

---

## F11: Bitmap-based incremental resync

### 原理
断线重连后不必全量复制。只重送 bitmap 标 dirty 的 4 KiB blocks，速度与 dirty 量成正比。
- 对应 DRBD `Inconsistent → SyncSource/Target` 增量 sync

### 实现
- `dm_pcache_state.c::pcache_do_resync` 主循环：
  ```c
  while (pcache_bm_find_next_dirty(bm, &cursor, &nr_bits, MAX_RUN)) {
      bio_alloc + bio_add_page + submit_bio (read from backing);
      send P_RS_DATA + payload;
      recv P_RS_ACK;
      pcache_bm_clear_range(bm, sector, size);
      cursor += nr_bits;
  }
  send P_BITMAP_DONE;
  pcache_bm_mark_synced(bm, peer_gen);
  ```
- Secondary 收 `P_RS_DATA` → `handle_rs_data_recv` 写入 cache + `pcache_bm_clear_range(peer->bm, ...)` 也清自己 bitmap
- Chunk size: `PCACHE_RS_BITS_PER_CHUNK = 64 bits = 256 KiB / chunk`

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# 1. 写 4 MiB，等同步
ssh root@$NODE1 'dd if=/dev/urandom of=/tmp/a.bin bs=1M count=4 status=none
                        dd if=/tmp/a.bin of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct conv=fsync status=none'
sleep 3

# 2. nft drop port 切断 Secondary
ssh root@$NODE2 'nft add table inet block 2>/dev/null
                          nft add chain inet block input { type filter hook input priority 0\; } 2>/dev/null
                          nft add rule inet block input tcp dport 9764 drop'

# 3. 等 55s 断线侦测
sleep 55

# 4. 在断线期间 Primary 写 1 MiB at offset 0
ssh root@$NODE1 'dd if=/dev/urandom of=/tmp/b.bin bs=1M count=1 status=none
                        dd if=/tmp/b.bin of=/dev/mapper/pcache-dist0 bs=1M count=1 oflag=direct conv=fsync status=none'

# 5. 解除 nft 阻挡，等 reconnect + 增量 resync
ssh root@$NODE2 'nft delete table inet block'
sleep 15

# 6. 验证 dmesg 显示「只送 256 bits」(= 1 MiB) 而非全量 131072 bits
ssh root@$NODE1 'dmesg | grep "resync starting" | tail -3'
# 预期：resync starting (dirty=256 / 131072 bits) ← 增量
```

---

## F12: Bilateral bitmap merge (P_BITMAP and ENC_RUN)

### 原理
Handshake 后双方互送 dirty bitmap，OR-merge 进本地。确保 SYNC_SOURCE 的 drain 涵盖双方都标的 dirty 区段，避免漏送。
- 对应 DRBD `bm_uuid` based bitmap exchange
- ENC_RUN encoding：只送 `(start_bit, nr_bits)` 描述连续 set-bit run，无 payload（contiguous-1s 的特殊情况下比 raw bitmap 高效很多）

### 流程

**1. 在 handshake 序列里的位置** —— bitmap 交换只在角色为 SYNC_SOURCE / SYNC_TARGET 时做
（ESTABLISHED 跳过、HALT 停住）：
```
NEGOTIATING → exchange_sync_params   交换 P_SYNC_PARAMS(generation, peer_gen_at_sync, flags)
            → decide_sync_role        只比 generation + FULL_SYNC flag，定角色（还没看 bitmap）
   若 SOURCE/TARGET → BITMAP_EXCHANGE → exchange_bitmaps     ← 本 feature
            → do_resync(SOURCE 推 dirty range) → mark_synced → ESTABLISHED
```

**2. 收发顺序（防死锁）** —— 两边都要「送自己的 + 收对方的」，用 id-based initiator
（lower node_id = initiator）错开，避免双方同时 block 在 recv：
```c
if (initiator) { send_bitmap_full(); recv_bitmap_merge(); }   // 先送后收
else           { recv_bitmap_merge(); send_bitmap_full(); }   // 先收后送
```

**3. send 侧** —— `pcache_send_bitmap_full` 只送 set bits 的连续段，不送整张 raw bitmap：
```c
while (pcache_bm_find_next_dirty(bm, &cursor, &nr_bits, MAX_RUN)) {
    pcache_send_bitmap_run(sock, cursor, nr_bits);   // P_BITMAP: (start_bit, nr_bits) ENC_RUN
    cursor += nr_bits;
}
pcache_send_packet(sock, &done);                     // P_BITMAP_DONE 收尾
```

**4. recv 侧** —— `pcache_recv_bitmap_merge` 读到 DONE 为止，每段 `bm_set_range`（= OR，只置位、
从不清位）叠进本地：
```c
while (收到的不是 P_BITMAP_DONE) {
    收 (start_bit, nr_bits);
    pcache_bm_set_range(bm, start_bit * sectors_per_bit, nr_bits * 4K);  // OR 进自己
}
```

**5. 结果：两边 bitmap 收敛成「并集」** —— 交换完双方 bitmap 都 = `(我原本 dirty) ∪ (对方 dirty)`。
为什么需要：断线期间两侧可能各标了不同区段（primary 标 {A,B}，secondary 标 {B,C}）；只看
primary 的帐会漏 C。OR-merge 后 SYNC_SOURCE 的 bitmap = {A,B,C}，`do_resync` 把三段全推一遍 →
保证不漏。之后 SOURCE 收到 `P_RS_ACK`、TARGET 写入 cache 后各自 `pcache_bm_clear_range` 清掉对
应段（见 [F11](#f11-bitmap-based-incremental-resync)），最后两边全 clean → ESTABLISHED。

### 实现
- Wire protocol: `PCACHE_P_BITMAP` packet + `struct pcache_bitmap_pkt { start_bit, nr_bits, encoding }` + `PCACHE_BM_ENC_RUN = 2`
- `dm_pcache_state.c::pcache_send_bitmap_full`：walk `pcache_bm_find_next_dirty` 把每个 run 送出 + 结尾 `P_BITMAP_DONE`
- `pcache_recv_bitmap_merge`：对每收到的 run 调用 `pcache_bm_set_range(bm, start_bit*sectors_per_bit, nr_bits*4K)`
- `pcache_exchange_bitmaps(peer, sock, initiator)`：initiator send-then-recv，non-initiator 相反

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# 触发 force-resync 后双方应交换 bitmap (即使是 0 runs)
ssh root@$NODE1 'dmsetup message pcache-dist0 0 force-resync'
sleep 5

for n in $NODE1 $NODE2; do
  echo "--- $n ---"
  ssh root@$n 'dmesg | grep "bitmap-exchange" | tail -3'
done
# 预期：每端 dmesg 显示 "sent N run(s)" 和 "received M run(s)"
```

---

## F13: Generation counter and per-peer peer_gen_at_sync

### 原理
- **Device-wide generation** (`u64`)：本节点的单调写世代。每次首个 dirty bit 触发时 +1。
- **Per-peer `peer_gen_at_sync`**：记录该 peer 上次成功同步时对方的 generation。
- `decide_sync_role` 比对 `(my_gen, peer_gen, my_peer_gen_at_sync, peer's_peer_gen_at_sync_of_me)` 决定谁是 SOURCE / TARGET / ESTABLISHED / HALT。

`peer_gen_at_sync` 为 per-peer：若在设备内共享，多个 peer 会互相覆写，导致连锁的误判 HALT。

### 实现
- `struct dm_pcache` 添加 `u64 generation; bool gen_dirty; spinlock_t gen_lock;`
- `struct pcache_bitmap` 保留 `peer_gen_at_sync` 字段但语义改为 per-peer (通过 `peer->bm`)
- `pcache_bm_set_range` 拿 `pcache->gen_lock` 做 gen bump
- `pcache_bm_mark_synced` 写 `peer->bm->peer_gen_at_sync` 并 reset device `gen_dirty`
- `decide_sync_role` 比较逻辑（4-case decision tree）

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# 看 device 与 per-peer gen 的状态
ssh root@$NODE1 'dmsetup message pcache-dist0 0 bitmap-status
                        dmesg | grep "bitmap\\[slot" | tail -3'
# 预期：device gen=N, peer[0 node_id=1] peer_gen_at_sync=M

# force-resync → bump device gen
#  注意：普通成功写入对已连线 peer 走 fast path（pcache_net_submit_write），
#  不调用 pcache_bm_set_range，故不会 bump gen。gen 只在「标 bitmap dirty」时 +1
#  （断线/NACK/timeout/无 peer 连线），或由 force-resync / reset-generation 显式 bump。
ssh root@$NODE1 'dmsetup message pcache-dist0 0 force-resync'
sleep 10
ssh root@$NODE1 'dmsetup message pcache-dist0 0 bitmap-status'
# 预期：gen 从 N 增加（force-resync bump）；resync 完成后 peer_gen_at_sync 经 mark_synced 更新
```

---

# Section 4 — Persistence & Crash Recovery

## F14: PMEM superblock and double-copy CRC headers

### 原理
所有 metadata header（cache_info, cache_ctrl, peer registry, bitmap slot header）都采 **2× 4 KiB 双复本 + 单调 seq + CRC32C**。写入时 alternating 写 next index，确保任何时刻至少有一份 valid copy（atomic update without journaling）。

### 实现
- `pcache_internal.h::struct pcache_meta_header { u32 crc; u8 seq; u8 version; u16 res; }`
- Helpers: `pcache_meta_crc()` (crc32c over body), `pcache_meta_seq_after()` (handles 8-bit wraparound), `pcache_meta_find_latest()` (scans copies, picks highest valid seq)
- Bitmap header: `dm_pcache_bitmap.c::pcache_bm_header_flush`：
  ```c
  hdr.seq = ++bm->meta_seq;
  hdr.crc = pcache_meta_crc(&hdr.header, sizeof(hdr));
  bm->meta_index = (bm->meta_index + 1) % PCACHE_META_INDEX_MAX;
  memcpy_flushcache(pmem_base + meta_index * 4K, &hdr, sizeof(hdr));
  pmem_wmb();
  ```
- 写入顺序保证：`memcpy_flushcache` flush CPU cache → `pmem_wmb()` 确保到 PMEM controller

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# 对 slot 0 看双复本 header (alternating seq)
ssh root@$NODE1 'echo "--- copy 0 @ 0x7000 ---"
                        dd if=/dev/pmem0 bs=1 skip=$((0x7000)) count=16 status=none | xxd
                        echo "--- copy 1 @ 0x8000 ---"
                        dd if=/dev/pmem0 bs=1 skip=$((0x8000)) count=16 status=none | xxd'
# 预期：两个复本都有 valid CRC，seq 互差 1；latest 由 highest seq 决定
```

---

## F15: Persistent peer registry (32 slots)

### 原理
PMEM 上记录 `(node_id, addr, added_generation, flags)` × 32 slots。Slot index 对应 bitmap region 的 slot。跨 reboot peer 配置不丢，且 add-peer 是 idempotent。
- peer 不写在 dmsetup table 中，重启后无需重新 add-peer

### 实现
- On-media: `struct pcache_peer_registry_onmedia { meta_header + magic + my_node_id + entries[32] }`
- Per entry (96 bytes): `node_id`, `flags` (IN_USE / TOMBSTONE), `added_generation`, `addr[64]`
- `dm_pcache_registry.c`：
  - `pcache_registry_init` — load 或 fresh init
  - `pcache_registry_add` — 找空 slot 或 tombstoned slot 回收
  - `pcache_registry_remove` — 设 tombstone（软删除，slot 可被 add 回收）
  - `pcache_registry_find_slot_by_node_id` — by id 反查
- Idempotent add-peer: `pcache_add_peer` 先 `find_slot_by_node_id`，若 registry 有但 `peers[]` 空 → 重建 in-memory peer（[F17](#f17-idempotent-add-peer-registry-recovery)）

### Demo
```bash
bash demo.sh reset && bash demo.sh start3
sleep 5

# 看 registry 内容 — slot 0 应有 peer 1 的纪录
ssh root@$NODE1 'echo "--- registry header copy 0 @ 0x5000 ---"
                        dd if=/dev/pmem0 bs=1 skip=$((0x5000)) count=64 status=none | xxd'
# 预期：magic = PCACMPR3 (0x50434143_4d505233 LE = "33 52 50 4d 43 41 43 50")

# remove-peer + 重启 → 重启后 registry 仍记得这个 peer (tombstone)
ssh root@$NODE1 'dmsetup message pcache-dist0 0 remove-peer 2
                        dmsetup remove pcache-dist0
                        rmmod dm_pcache; insmod /root/dm-pcache.ko
                        LOOP=$(losetup -j /var/lib/pcache_backing.img | cut -d: -f1 | head -1)
                        SIZE=$(blockdev --getsz $LOOP)
                        dmsetup create pcache-dist0 --table "0 $SIZE pcache-dist /dev/pmem0 $LOOP my_node_id=0 is_primary=1 listen_port=9764"
                        # 不重 add-peer
                        dmsetup status pcache-dist0
                        dmesg | grep "registry: loaded" | tail -1'
# 预期：dmesg 显示 "registry: loaded — my_node_id=0, N/32 peer slot(s) in use"
```

---

## F16: RESYNC_IN_PROGRESS crash detection

### 原理
`do_resync` 开始时设 `PCACHE_BM_F_RESYNC_IN_PROGRESS` flag + flush 到 PMEM。完成时 `mark_synced` 清掉。若中途 crash，重启 `pcache_peer_bitmap_init` 载到 PMEM 仍有此 flag → 强制设 `FULL_SYNC_NEEDED` + 清 flag → 下次 reconnect 自动全量重做。

### 实现
- `dm_pcache_bitmap.h` 旗位定义: `PCACHE_BM_F_RESYNC_IN_PROGRESS = BIT(1)`
- `pcache_do_resync` 入口：set + `pcache_bm_flush_sync(bm)`
- `pcache_bm_mark_synced` 出口：clear + flush
- `pcache_peer_bitmap_init` load 逻辑：
  ```c
  if (bm->flags & PCACHE_BM_F_RESYNC_IN_PROGRESS) {
      pcache_dev_info(pcache, "...previous resync was interrupted; forcing FULL_SYNC");
      bm->flags |= PCACHE_BM_F_FULL_SYNC_NEEDED;
      bm->flags &= ~PCACHE_BM_F_RESYNC_IN_PROGRESS;
  }
  ```

### Demo
```bash
bash demo.sh test-crash
# 完整 crash recovery 流程 (sysrq+c → 重启节点 → reload + recreate)
# 预期：crash recovery path 通；若 crash 中 do_resync 进行中，
#       dmesg 显示 "previous resync was interrupted; forcing FULL_SYNC"
# 注：要可靠触发 flag-set 需 256 MiB+ workload (4 MiB resync < 1 sec)
```

---

## F17: Idempotent add-peer (registry recovery)

### 原理
若 peer 已在 registry（跨重启持久化）但 `peers[]` 空（kthread 没起），add-peer 不报 EEXIST，而是复用 slot 并重新启动 kthread，操作员无需在每次重启后先 remove-peer 再 add-peer。

### 实现
- `pcache_add_peer` 开头：
  ```c
  slot = pcache_registry_find_slot_by_node_id(pcache->registry, peer_node_id);
  if (slot >= 0) {
      if (net->peers[slot])
          return -EEXIST;  // 真重复
      // registry has it but peers[] empty → recreate in-memory peer
  } else {
      pcache_registry_add(...);
      // 新 slot
  }
  // pcache_alloc_peer + pcache_peer_bitmap_init + start worker
  ```

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# dmsetup remove + recreate，registry 仍保留旧记录
ssh root@$NODE1 'dmsetup remove pcache-dist0
                        rmmod dm_pcache; insmod /root/dm-pcache.ko
                        LOOP=$(losetup -j /var/lib/pcache_backing.img | cut -d: -f1 | head -1)
                        SIZE=$(blockdev --getsz $LOOP)
                        dmsetup create pcache-dist0 --table "0 $SIZE pcache-dist /dev/pmem0 $LOOP my_node_id=0 is_primary=1 listen_port=9764"
                        # add-peer：registry 中已有该 peer
                        dmsetup message pcache-dist0 0 add-peer 1 '"$NODE2"':9764'
# 预期：成功 (exit 0)，不是 EEXIST
```

---

# Section 5 — HA / Role Management

## F18: Primary/Secondary role and write fencing

### 原理
分布式语义要求**只有 Primary 可服务 WRITE**。Secondary 收到 WRITE 应拒写，防止双写 → split-brain。
- READ 允许在 Secondary（读本地已复制的数据）
- Standalone 模式（无 net）不检查

### 实现
- `dm_pcache.c::dm_pcache_map_bio` 前置 guard：
  ```c
  if (pcache->net && !pcache->is_primary && bio_data_dir(bio) == WRITE) {
      bio_io_error(bio);
      return DM_MAPIO_SUBMITTED;
  }
  ```
- `pcache->is_primary` 由 dmsetup table `is_primary=0|1` 初始化，runtime 通过 [F19](#f19-set-role-runtime-role-switch) 修改

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# Primary 写 OK
ssh root@$NODE1 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct status=none; echo Primary exit=$?'

# Secondary 写 EIO
ssh root@$NODE2 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct status=none 2>&1; echo Secondary exit=$?'
# 预期：Primary exit=0；Secondary "Input/output error" + exit=1

# Secondary 读 OK
ssh root@$NODE2 'dd if=/dev/mapper/pcache-dist0 bs=4K count=1 iflag=direct status=none | wc -c'
# 预期：4096
```

---

## F19: `set-role` runtime role switch

### 原理
Runtime promote (Secondary → Primary) 或 demote (Primary → Secondary)，不需 dmsetup remove + recreate。
- Quorum check (除非 `force`) 确保安全
- Promote 自动触发 [F23](#f23-configurable-quorum-off-majority-n) full resync 确保数据对齐
- Worker kthread 自动 swap ([F21](#f21-worker-kthread-role-swap))

### 实现
- `dm_pcache.c::dm_pcache_message` 解析 `set-role primary|secondary [force]`
- `dm_pcache_state.c::pcache_set_role(pcache, primary, force)`：
  - Promote 路径：`pcache_has_quorum` check → 翻 `is_primary=true` → 广播 `P_DEMOTE_REQ` ([F20](#f20-coordinated-demote-handshake-p_demote_reqack)) → 设 `FULL_SYNC_NEEDED` + `bm_set_all` for each peer → swap workers
  - Demote 路径：翻 `is_primary=false` → swap workers → flush bitmaps

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# 初始状态：node-1 Primary, node-2 Secondary
ssh root@$NODE1 'dmsetup message pcache-dist0 0 set-role secondary' && echo n1-demote-OK
ssh root@$NODE2 'dmsetup message pcache-dist0 0 set-role primary force' && echo n2-promote-OK
sleep 3

# 验证：node-2 (new Primary) 可写；node-1 (now Secondary) 写 EIO
ssh root@$NODE2 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct status=none; echo new-Primary exit=$?'
ssh root@$NODE1  'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct status=none 2>&1; echo old-Primary exit=$?'
# 预期：new-Primary exit=0；old-Primary "Input/output error" exit=1
```

---

## F20: Coordinated demote handshake (P_DEMOTE_REQ/ACK)

### 原理
Promote (non-force) 时自动广播 `P_DEMOTE_REQ` 通知所有 connected peer「我要 promote」。Peer 收到 reply `P_DEMOTE_ACK`，operator 可手动 demote 老 Primary。
- Best-effort（不 wait ACK）避免 lock 复杂度
- Split-brain detection ([F24](#f24-split-brain-per-pair-detection)) 兜底

### 实现
- Wire protocol v3: `PCACHE_P_DEMOTE_REQ = 0x30`, `PCACHE_P_DEMOTE_ACK = 0x31`
- `struct pcache_demote_pkt { initiator_node_id, flags, status, reserved }` 8 bytes
- `dm_pcache_net.c::pcache_send_demote_req/ack/recv_body` helpers
- `pcache_set_role` promote 分支：iterate `for_each_active_peer`，每 connected sock 送 P_DEMOTE_REQ
- `primary_receiver` + `secondary_worker` recv loop 添加 `case PCACHE_P_DEMOTE_REQ` → log + reply ACK

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# node-1 demote → node-2 promote (no force)
ssh root@$NODE1 'dmsetup message pcache-dist0 0 set-role secondary && echo demote-OK'
sleep 2
ssh root@$NODE2 'dmsetup message pcache-dist0 0 set-role primary && echo promote-OK'
sleep 3

# node-2 应有 "broadcast DEMOTE_REQ to N peer(s)" + DEMOTE_ACK 回响
ssh root@$NODE2 'dmesg | grep -E "DEMOTE_REQ|DEMOTE_ACK|broadcast" | tail -5'

# node-1 应有 "DEMOTE_REQ from node_id=1 acknowledged"
ssh root@$NODE1 'dmesg | grep -E "DEMOTE_REQ" | tail -3'
# 预期：三段 dmesg 全到位
```

---

## F21: Worker kthread role swap

### 原理
Role 改变后 worker 逻辑也要切换。`primary_worker` 跑 send-loop + ACK receiver；`secondary_worker` 跑 recv-loop。set-role 同步停旧 worker + 起新 worker，避免 fan-out 写进空集合。

### 实现
- `dm_pcache_state.c::pcache_set_role_swap_workers(net, to_primary)`，per peer 对
  control stream `streams[0]` 做 stop→restart（[F31](#f31-multi-stream-replication-streamsn) 多串流重构后改用
  `pcache_conn` helper，不再直接碰 `peer->sock`）：
  ```c
  for_each_active_peer(net, peer, slot) {
      peer->is_primary = to_primary;
      pcache_conn_shutdown(&peer->streams[0]);    // 唤醒 worker（helper 自带 sock_lock）
      if (peer->streams[0].sender) {              // primary：sender 退出时连带停 receiver
          kthread_stop(peer->streams[0].sender);
          peer->streams[0].sender = peer->streams[0].receiver = NULL;
      } else if (peer->streams[0].receiver) {     // secondary
          kthread_stop(peer->streams[0].receiver);
          peer->streams[0].receiver = NULL;
      }
      pcache_conn_drop(&peer->streams[0]);         // detach + 放掉 install ref
      peer->state = CONNECTING; peer->repl_state = OFF;
      to_primary ? pcache_start_primary(peer) : pcache_start_secondary(peer);
  }
  ```
  注意：`pcache_conn_shutdown` / `pcache_conn_drop` **内部**会拿 `streams[0].sock_lock`，
  调用端不可再持该锁（否则 recursive spinlock 自锁 → watchdog reset；见 [F32](#f32-multi-stream-churn-robustness-kref-socket-lifetime)，
  kernel `0ac2cb2`）。

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# Demote 触发 worker swap (primary_worker → secondary_worker)
ssh root@$NODE1 'dmsetup message pcache-dist0 0 set-role secondary'
sleep 2

# dmesg 应看到老 worker 停 + 新 worker 起
ssh root@$NODE1 'dmesg | grep -E "set-role: demoted|repl_state.*OFF|handshake complete" | tail -10'
# 预期：primary 路径 OFF，secondary 路径重新 NEGOTIATING
```

---

# Section 6 — Quorum & Split-brain Safety

## F22: Quorum check (DRBD-style local, no election)

### 原理
**Local check**：「我能看见的 peer 数 + 1 (self) ≥ quorum_min」就有 quorum。没有投票、没有 Raft/Paxos 共识协议。
- 对应 DRBD 9 的 `quorum {off, majority, all}` 概念
- 简单但有效：每个 Primary 自己负责 fence-on-write

### 实现
- `dm_pcache_state.c::pcache_has_quorum(pcache)`：
  ```c
  total = 1; connected = 1;            // self 预先计入
  for_each_active_peer(net, peer, slot) {
      total++;
      if (peer->state == PCACHE_C_CONNECTED) connected++;
  }
  threshold = pcache->quorum_majority ? (total/2)+1 : pcache->quorum_min;
  return connected >= threshold;       // self 已含在 connected 里，无需再 +1
  ```

### Demo
```bash
# 看 [F23](#f23-configurable-quorum-off-majority-n) configurable quorum 的完整示范
```

---

## F23: Configurable quorum (off, majority, N)

### 原理
- `quorum=off` (默认)：threshold=1，永远有 quorum（向下兼容）
- `quorum=majority`：threshold=(active+1)/2 + 1（DRBD majority）
- `quorum=N`：明确指定门槛

### 实现
- dmsetup table line 新参数: `quorum=off|majority|N`
- `pcache_parse_dev_args` 解析设 `pcache->quorum_min` + `pcache->quorum_majority`
- Map_bio Primary WRITE check：`!pcache_has_quorum(pcache)` → `bio_io_error`
- set-role promote `!force` check：`!pcache_has_quorum(pcache)` → `-EBUSY`

### Demo
```bash
# 用 quorum=majority 启动 2-node (threshold=2)
bash demo.sh reset
bash demo.sh start
ssh root@$NODE1 'dmsetup remove pcache-dist0
                        LOOP=$(losetup -j /var/lib/pcache_backing.img | cut -d: -f1 | head -1); SIZE=$(blockdev --getsz $LOOP)
                        dmsetup create pcache-dist0 --table "0 $SIZE pcache-dist /dev/pmem0 $LOOP my_node_id=0 is_primary=1 listen_port=9764 quorum=majority"
                        dmsetup message pcache-dist0 0 add-peer 1 '"$NODE2"':9764'
sleep 5

# Quorum 2/2 → write OK
ssh root@$NODE1 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct status=none; echo write-1 exit=$?'

# Drop peer → quorum 1/2 → write EIO
ssh root@$NODE1 'nft add table inet block 2>/dev/null
                        nft add chain inet block output { type filter hook output priority 0\; } 2>/dev/null
                        nft add rule inet block output ip daddr '"$NODE2"' tcp dport 9764 drop'
sleep 55  # wait keepalive
ssh root@$NODE1 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct status=none 2>&1; echo write-2 exit=$?'
# 预期：write-1 exit=0; write-2 "Input/output error" exit=1

ssh root@$NODE1 'nft delete table inet block'
```

---

## F24: Split-brain per-pair detection

### 原理
Handshake 后双方比对 `(my_gen, peer_gen, my_peer_gen_at_sync, peer's_peer_gen_at_sync_of_me)`。若 generation 互不为对方的祖先（diverged）→ 判定 split-brain → 该 pair 进 HALT，等手动 [F25](#f25-split-brain-manual-resolve-victim-survivor) 解决。
- Per-pair：peer A↔peer B 互判 split-brain 不影响 A↔peer C 连接
- 对应 DRBD UUID chain 比对的简化版

### 实现
- `dm_pcache_state.c::pcache_decide_sync_role` 4-case：
  ```c
  if (my_gen == peer_gen && reciprocity OK)        return ESTABLISHED;
  if (my_gen > peer_gen && peer_gen == my_peer_gen) return SYNC_SOURCE;
  if (peer_gen > my_gen && my_gen == peer_my_gen)  return SYNC_TARGET;
  // 否则：incompatible — split-brain
  return HALT;
  ```
- Primary/secondary worker HALT 路径：设 `peer->split_brain = true` + log + 永久 sleep (kthread_should_stop)
- HALT 是 per-peer 状态，不影响其他 peer

### Demo
```bash
# 模拟 split-brain：两边各自 force-promote
bash demo.sh reset && bash demo.sh start
sleep 5

# 切 nft 隔离双方 → 各自 force promote → 两端都写不同数据 → 移除 nft 观察 HALT
ssh root@$NODE2 'nft add table inet iso 2>/dev/null
                          nft add chain inet iso input { type filter hook input priority 0\; } 2>/dev/null
                          nft add rule inet iso input tcp dport 9764 drop'
sleep 55  # wait disconnect

# node-1 已是 Primary (gen 推进)；node-2 force promote
ssh root@$NODE1  'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=1 oflag=direct status=none'
ssh root@$NODE2 'dmsetup message pcache-dist0 0 set-role primary force
                          dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=1 oflag=direct status=none'

# 解除隔离 → handshake 应侦测 split-brain → HALT
ssh root@$NODE2 'nft delete table inet iso'
sleep 20

for n in $NODE1 $NODE2; do
  echo "--- $n ---"
  ssh root@$n 'dmesg | grep -E "split-brain|HALT" | tail -3'
done
# 预期：两端都 log "split-brain detected for peer node_id=X; halted"
```

---

## F25: Split-brain manual resolve (victim, survivor)

### 原理
侦测到 split-brain 后 cluster 进 HALT，不会自动仲裁。Operator 必须决定哪边是「权威」(survivor)、哪边放弃数据 (victim)：
- `victim`：清自己 bitmap + 设 FULL_SYNC_NEEDED → 下次 reconnect 接收对方全套数据
- `survivor`：bump 自己 gen + 设 FULL_SYNC_NEEDED → 下次 reconnect 推送自己数据到对方

### 实现
- `dm_pcache.c::dm_pcache_message` 解析 `resolve-split-brain <node_id> victim|survivor`
- `pcache_resolve_split_brain(pcache, peer_id, victim)`：
  - 找 peer → 清 `peer->split_brain` flag
  - victim: `pcache_bm_clear_all(peer->bm)` + `FULL_SYNC_NEEDED` + 清 device `gen_dirty`
  - survivor: bump `pcache->generation` + `bm_set_all` + `FULL_SYNC_NEEDED`
  - 停 + 重起该 peer 的 kthread 跳出 HALT 循环

### Demo
```bash
# 接续 F24 split-brain 场景

# operator 决策：node-1 是 survivor，node-2 是 victim
ssh root@$NODE2 'dmsetup message pcache-dist0 0 resolve-split-brain 0 victim
                          dmsetup message pcache-dist0 0 set-role secondary force'
ssh root@$NODE1  'dmsetup message pcache-dist0 0 resolve-split-brain 1 survivor'

sleep 20

# 验证：node-2 应从 node-1 接到全套数据
ssh root@$NODE1  'dd if=/dev/mapper/pcache-dist0 bs=1M count=1 iflag=direct status=none | md5sum'
ssh root@$NODE2 'dd if=/dev/mapper/pcache-dist0 bs=1M count=1 iflag=direct status=none | md5sum'
# 预期：MD5 一致
```

---

# Section 7 — Operational Interface

## F26: dmsetup table line config

### 原理
通过 dmsetup table line 一次性设置 device-level 参数。Per-peer 配置走 [F27](#f27-add-peer-remove-peer-list-peers-messages) message。

### Table line 格式
```
<start> <length> pcache-dist <cache_dev> <backing_dev> [cache_opts...] [my_node_id=N] [is_primary=B] [listen_port=P] [quorum=off|majority|N]
```

| 参数 | 说明 | 默认 |
|---|---|---|
| `my_node_id=N` | 本节点 id | 必填（分布式模式） |
| `is_primary=0|1` | 启动角色 | 必填 |
| `listen_port=P` | 监听 port | 必填 |
| `quorum=off|majority|N` | Quorum 门槛 | off (=1) |
| `cache_mode` (cache opts) | writeback (only) | writeback |
| `data_crc` | true/false | false |

### Demo
```bash
ssh root@$NODE1 'LOOP=$(losetup -j /var/lib/pcache_backing.img | cut -d: -f1 | head -1)
SIZE=$(blockdev --getsz $LOOP)
dmsetup create pcache-dist0 --table "0 $SIZE pcache-dist /dev/pmem0 $LOOP 4 cache_mode writeback data_crc false my_node_id=0 is_primary=1 listen_port=9764 quorum=majority"
dmesg | grep "network up" | tail -1'
# 预期：dmesg 显示 "network up: my_node_id=0 listen_port=9764 is_primary=1"
```

---

## F27: add-peer, remove-peer, list-peers messages

### 原理
Runtime peer mesh 管理：不停服务的情况下加减 peer。
- `add-peer <id> <ip:port>`：找空 slot → 起 kthread → handshake
- `remove-peer <id>`：停 kthread → close sock → tombstone in registry
- `list-peers`：iterate `peers[]`，把每 slot 状态写进 **message 结果缓冲**（`dmsetup message` 的 stdout），**不是 dmesg**

### 实现
- `dm_pcache.c::dm_pcache_message` 解析三个 message
- `pcache_add_peer / remove_peer / list_peers` 在 `dm_pcache_state.c`
- Idempotent semantics: [F17](#f17-idempotent-add-peer-registry-recovery)

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# Add 第二 peer
ssh root@$NODE1 'dmsetup message pcache-dist0 0 add-peer 2 '"$NODE3"':9764 && echo add-OK'

# Remove
ssh root@$NODE1 'dmsetup message pcache-dist0 0 remove-peer 2 && echo remove-OK'
sleep 1

# Re-add 同 peer (idempotent)
ssh root@$NODE1 'dmsetup message pcache-dist0 0 add-peer 2 '"$NODE3"':9764 && echo re-add-OK'

# List —— 输出在 message stdout 本身（不是 dmesg）；add/remove 的 "slot=" 日志才在 dmesg
ssh root@$NODE1 'dmsetup message pcache-dist0 0 list-peers'
ssh root@$NODE1 'dmesg | grep -E "add-peer: slot=|remove-peer: slot=" | tail -3'
```

---

## F28: abort-resync, force-resync, reset-generation

### 原理
Resync / generation 的人工干预：
- `force-resync`：设所有 peer 的 `FULL_SYNC_NEEDED` flag + 断 socket → 下次 reconnect 全量重做
- `abort-resync`：清所有 peer 的 `FULL_SYNC_NEEDED` + drop socket（中断进行中的 resync）
- `reset-generation`：bump device generation + drop socket（强制 split-brain 重新评估）

### 实现
- Three messages in `dm_pcache.c::dm_pcache_message`
- Iterate peers，各自操作 `peer->bm->flags` 或 device `pcache->generation`

### Demo
```bash
bash demo.sh reset && bash demo.sh start
sleep 5

# 1. force-resync — 重做全量
ssh root@$NODE1 'dmsetup message pcache-dist0 0 force-resync'
sleep 10
ssh root@$NODE1 'dmesg | grep -E "force-resync|full-sync needed" | tail -3'
# 预期：dmesg 显示 "force-resync requested"；后续 resync 时显示
#       "full-sync needed, marking all <N> bits"（<N> = backing 大小决定的 bit 数，非定值）

# 2. abort-resync — 取消
ssh root@$NODE1 'dmsetup message pcache-dist0 0 force-resync
                        dmsetup message pcache-dist0 0 abort-resync
                        dmesg | grep abort | tail -1'
# 预期："abort-resync: cleared FULL_SYNC_NEEDED on all peers..."

# 3. reset-generation
ssh root@$NODE1 'dmsetup message pcache-dist0 0 reset-generation
                        dmesg | grep reset-gen | tail -1'
# 预期："reset-generation: bumped to gen=N"
```

---

## F29: Status reporting (status and bitmap-status)

### 原理
- `dmsetup status` (info)：device 级状态（cache_dev 信息 + per-device gen + 跨 peer 累积 dirty count）
- `dmsetup message bitmap-status`：per-peer 细节（slot, peer_node_id, peer_gen_at_sync, dirty/total, flags）

### 实现
- `dm_pcache_status` 回 `STATUSTYPE_INFO`：cache 结构 + iterate peers' bm 累积
- `bitmap-status` message handler：`DMEMIT` 每 peer 一行

### Demo
```bash
bash demo.sh reset && bash demo.sh start3
sleep 5

ssh root@$NODE1 'dmsetup status pcache-dist0'
# 预期：含 bm_gen=N bm_dirty=X/Y

ssh root@$NODE1 'dmsetup message pcache-dist0 0 bitmap-status'
# 预期：每行一个 peer，含 slot/node_id/peer_gen_at_sync/dirty/flags
```

---

## F30: Clean PMEM reset script

### 原理
完整重设环境：3 VM 平行清 PMEM (含 24 KiB SB+registry + 32 个 bitmap slot headers, 256 KiB total) + 卸载 module + 拔 loop + 删 backing。约 1 秒完成。

### 实现
- `dm_pcache_phase1_demo.sh::cleanup_node` 内 Python script wipe PMEM 关键 metadata 区段
- `cmd_reset` iterate 3 nodes 并调用 `cleanup_node`

### Demo
```bash
bash tests/dm_pcache_phase1_demo.sh reset
# 预期：3 VM 各 [CLEANUP] done，1 秒内完成
```

---

# Section 8 — Performance / Multi-stream

## F31: Multi-stream replication (`streams=N`)

### 原理
单一 peer 只有**一条** TCP socket，所有 replicated WRITE 在这条 pipe 上串行化，
128K 顺序写吞吐被单 pipe + 单 recv/commit/ACK kthread 限制。
多串流时每 peer 开 **N 条并行 TCP stream**：
- `stream[0]` = **control**：handshake / sync-params / bitmap / resync / role / DEMOTE
- `stream[1..N-1]` = **data-only**：纯 P_DATA / P_ACK
- Primary 用 `hash_64(sector) % nr_streams` 把每笔 WRITE 分流到固定 stream（同 sector
  恒走同 stream → 保持 per-block 写序），多组 sender/receiver kthread 并行送收。
- `streams=1`（默认）= 完全等价旧单串流行为，opt-in 才开多串流。

128K seq qd=8 下 `streams=2` 比 `streams=1` 高约 17%（同一会话交错 A/B，warm-up 后 6 轮）。

### 实现
- Table line 新参数 `streams=N` → `dm_pcache.h::struct dm_pcache.nr_streams`
- `struct pcache_stream { struct pcache_conn *conn; sender; receiver; sock_lock; ... }`，
  per-peer `streams[]` array（取代旧的单一 `peer->sock`）
- Handshake 添加 `pcache_handshake_pkt.stream_id`（原 `reserved` 栏）区分 control / data
- Data 线程：primary `pcache-dsend` / `pcache-drecv`，secondary `pcache-dsec`
- Demux：`pcache_net_submit_write` → `hash_64(sector, 32) % nr_streams` 选 stream
  （早期用 `(sector>>3)%2`，128K/256-sector 2 的幂 stride 会全部塌进 stream 0 → 已改 hash）
- `pcache_accept_thread` 依 `(node_id, stream_id)` 把 inbound sock 路由到对应 stream slot

### Demo
```bash
bash demo_features.sh f31
# 预期：dmesg 显示 "handed inbound sock ... stream=0" 与 "stream=1" 各一；
#       node-1 有 pcache-dsend 线程、node-2 有 pcache-dsec 线程；
#       8 MiB 写入后 node-1 / node-2 MD5 完全一致（hash-by-sector 写序正确）
```

---

## F32: Multi-stream churn robustness (kref socket lifetime)

### 原理
多串流 + 频繁 setup/teardown 暴露 socket 生命周期 race：worker kthread 可能在
socket 已被 `sock_release` 之后再去 `kernel_sock_shutdown` → NULL `->ops` UAF /
crash。重构成 **kref-counted `struct pcache_conn`**：I/O 期间 `conn_get` 持参考，
teardown 走 `conn_drop` 才 detach + shutdown + 放掉 install ref，最后一个 put 才
真正 `sock_release`，杜绝 use-after-free。

### 实现
- `dm_pcache_net.h::struct pcache_conn { struct kref ref; struct socket *sock; }`
- `pcache_conn_get / put / install / drop / shutdown`，以 `stream->sock_lock` 保护
- **重要约束**：conn helper **自己**会拿 `sock_lock`，调用端**不可**在持有 `sock_lock`
  时调用它们，否则 recursive spinlock 自锁 → soft-lockup → watchdog reset。
  （`set_role_swap_workers`、split-brain、force-resync 三处曾有此问题，由 `0ac2cb2a246a` 移除外层
  `sock_lock` 修正；F19–F21 覆盖此路径。）

### Demo
```bash
bash demo_features.sh f32
# 10 轮 recreate(streams=2) + 8 MiB write + teardown
# 预期：每轮节点都保持可连接；结束时 dmesg 无 "soft lockup" / "BUG:" / "Oops"
```

---

## F33: Deferred-write replication (cache-pressure correctness fix)

### 原理
cache 满时进来的 WRITE 会被延后（`defered_req_fn` 重排），旧版 defer-retry 只走
单发 `cache_write` **写本地、不 replicate** → secondary 漏掉这些写 → 静默数据不一致。
修正：抽出统一的 `pcache_submit_req` helper（reserve → send → commit），map 路径与
defer 重试路径**都**调用它，确保被延后的写一样会 fan-out 到 peer。

### 实现
- `dm_pcache.c::pcache_submit_req(pcache, pcache_req)`：connected primary WRITE 走
  reserve + `pcache_net_submit_write` + commit；其余情况走本地写 + mark-dirty
- `dm_pcache_map_bio` 与 `defered_req_fn` 两条路径共用此 helper（`badbe157b60b`）

### Demo
```bash
bash demo_features.sh f33
# 32 MiB 连续 burst（撑出 cache-full → 触发 deferred write）后比对两端
# 预期：node-1 / node-2 MD5 一致 = 被延后的写也完成了复制
```

---

# Appendix A — Wire protocol packet 一览

| Type | Value | 用途 |
|---|---|---|
| `P_HANDSHAKE` | 0x00 | 初次握手（payload 带 `stream_id`，见下注） |
| `P_DATA` | 0x01 | 写数据 |
| `P_ACK` | 0x02 | 写成功 |
| `P_NACK` | 0x03 | 写失败 |
| `P_SYNC_PARAMS` | 0x10 | 交换 (gen, peer_gen_at_sync, bm_nr_bits, flags) |
| `P_BITMAP` | 0x11 | bitmap chunk |
| `P_BITMAP_DONE` | 0x12 | bitmap exchange / resync 结束标记 |
| `P_RS_DATA` | 0x13 | resync 数据 chunk |
| `P_RS_ACK` | 0x14 | resync 确认 |
| `P_OUT_OF_SYNC` | 0x15 | reserved (未用) |
| `P_PING` | 0x20 | reserved (TCP keepalive 代替) |
| `P_PING_ACK` | 0x21 | reserved |
| `P_DEMOTE_REQ` | 0x30 | promote 广播通知 |
| `P_DEMOTE_ACK` | 0x31 | demote 确认 |

`PCACHE_PROTO_VERSION = 3` 为当前协议版本。

> **多串流握手**：`struct pcache_handshake_pkt` 原本的 `reserved` 栏改为
> `stream_id`，`P_HANDSHAKE` 时带上，让 `accept_thread` 依 `(node_id, stream_id)`
> 把 inbound socket 路由到对应的 stream slot（`stream_id=0` 为 control，`1..N-1`
> 为 data）。沿用既有字段、未 bump proto version，故与 `streams=1` 旧节点兼容（[F31](#f31-multi-stream-replication-streamsn)）。

---

# Appendix B — On-disk PMEM layout

```
offset  size      content
─────────────────────────────────────────────────────────────────
0x0000  4K        reserved / zero
0x1000  4K        PCACHE_SB (magic 0x65B05EFA96C596EF + flags + seg_num + CRC)
0x2000  8K        PCACHE_CACHE_INFO  (2 × 4K double-copy)
0x4000  4K        PCACHE_CACHE_CTRL  (4K)
0x5000  8K        PCACHE_PEER_REGISTRY  (2 × 4K double-copy)
                  magic PCACMPR3 + my_node_id + 32 entries × 96 bytes
0x7000  1 GiB     PCACHE_BITMAP_REGION  (32 slots × 32 MiB)
                  each slot: 2× 4K hdr (magic PCACMB1\3) + ~32 MiB - 8K bitmap data
0x40007000 …      PCACHE_SEGMENTS  (variable, 16 MiB/segment, cache data)
```

---

# Appendix C — Demo script commands

`tests/dm_pcache_phase1_demo.sh` 命令一览：

| Command | 用途 |
|---|---|
| `build` | host 上 cross-compile module |
| `deploy` | 把 .ko scp 到 3 VM |
| `start` | 2-node cluster start (node-1 + node-2) |
| `start3` | 3-node mesh start |
| `stop` | 卸载 device + module on all VMs |
| `reset` | 完整清干净 (含 PMEM wipe) |
| `test` | 4 MB write + MD5 verify (基本) |
| `test-resync` | 断线 → 写 → 重连 → 增量 resync 验证 |
| `test-force` | force-resync message 触发 |
| `test-crash` | sysrq+c 强制 crash + 重启节点 + recovery |
| `stress [N]` | N 轮 (默认 20) 随机 write + churn + MD5 check |
| `bm-status` | 两 node 各自 bitmap-status |
| `cleanup` | 完整清理含 backing files |
| `all` | build → deploy → start → test pipeline |
