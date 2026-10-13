# pcache-dist 测试指南

本文档是 `tests/demo_features.sh`（功能测试套件）的测试参考，并在[测试工具总览](#测试工具总览)里说明
`tests/` 下的其它工具（BIO 后端回归、压力回归、单机 BIO 测试、性能测量、复现与诊断脚本）。
所有路径相对于 `drivers/md/dm-pcache/`。最终版本的测试结果汇总见
[`pcache-dist-test-report.md`](./pcache-dist-test-report.md)。

功能测试套件部分仿 [`FEATURES.md`](./FEATURES.md) 排版：**每个 test case 一节**，含

- **描述**：测什么 / 为什么
- **实现**：关键 `file.c:func`（详见 [`pcache-dist-manual.md`](./pcache-dist-manual.md) 与 `FEATURES.md`）
- **命令**：`bash demo_features.sh fNN`（官方跑法）+ 关键底层 recipe + `# 预期`
- **结果**：3 节点环境下的验收结果与判据

> 测试对象：`pcache-dist` 分布式复制 DM target。测试中对测试逻辑本身的两项修正见 [附录 A](#附录-a--关键发现-findings)。

## 测试环境 (Rig)

| 角色 | 节点 | 地址变量 | node_id |
|---|---|---|---|
| Primary | node-1 | `$NODE1` | 0 |
| Secondary | node-2 | `$NODE2` | 1 |
| Secondary | node-3 | `$NODE3` | 2 |

下文命令中的 `$NODE1`/`$NODE2`/`$NODE3` 为各节点的 IP 地址。

- 每节点：`/dev/pmem0`（2 GiB fsdax，宿主机内存文件支撑的模拟 nvdimm）作 DAX cache_dev、loop 文件 `/var/lib/pcache_backing.img` 作 backing、`dm-pcache.ko` 部署在 `/root/`。节点内核由本分支编译。
- BIO 后端：功能测试用 `tests/bench/bio_cachedev.sh` 在每个节点建立的 loop 设备 `/dev/pcache-cdev`；性能测试用宿主机内存文件支撑、热插入的 virtio 磁盘（见[性能测量](#性能测量)）。
- 脚本以 root 在各节点上执行（ssh 免密）；需要 host 端 `virsh` 的地方用 `$SUDO`（默认 `sudo`，需对 `virsh`、`systemd-inhibit` 免密，或设 `SUDO="sudo -n"`）。节点 IP 与 libvirt 域名定义在 `tests/demo_features.sh`、`tests/dm_pcache_phase1_demo.sh` 与 `tests/bench/common.sh` 开头，更换环境时修改这几处。
- DM 设备 `pcache-dist0`（即 `/dev/mapper/pcache-dist0`），端口 `9764`。
- 前置条件：3 个节点已启动且 pmem0 为 fsdax；辅助脚本 `dm_pcache_phase1_demo.sh reset|start|start3` 负责建/清集群。

## 怎么运行

```bash
cd drivers/md/dm-pcache/tests
bash demo_features.sh list          # 列出所有 demo
bash demo_features.sh f01           # 跑单项
bash demo_features.sh smoke          # 快速子集（22 项，不含破坏性）
bash demo_features.sh section1..11   # 跑某 section
bash demo_features.sh all            # 全部 51 项（含破坏性 F35），共 180 个检查
```

## 加固后的断言助手

| 助手 | 作用 |
|---|---|
| `assert_md5_eq REF VAL...` | 每个 VAL 须是合法 md5（32 hex）且等于 REF；空/无效/不符即 FAIL（杜绝 `空==空` 假 PASS） |
| `assert_dmesg NODE PAT LABEL` | 仅当 dmesg 出现证据行才 PASS |
| `wait_md5_eq / wait_dmesg` | 轮询到收敛/证据出现（替代固定 `sleep`，抗 flake） |
| `require_nodes` | 节点不可达即中止该 demo（VM crash policy） |
| `block_inbound/unblock` + trap | nft 规则注册到 trap，Ctrl-C/失败也清理，不残留 DROP |
| `ssh_nt` | 远端 I/O 套 120s timeout，卡住的 fsync 不挂死整跑 |
| 结尾 SUMMARY | 统计 PASS/FAIL，有失败则 `exit 1` |

**Section 10 引入的助手**（移植自 DRBD 9 测试套件 `drbd9-tests`）：

| 助手 | 来源 | 作用 |
|---|---|---|
| `busy_start / busy_wait / busy_errors` | `python/busywrite.py` | 后台 fio 写手。`busy_wait` **带超时**：写手没在自己的 runtime 内退出 = 有 bio 从未完成，这就是挂死断言 |
| `forbidden_mark / assert_no_forbidden` | `drbdtest.forbidden_patterns` | 反向断言：`BUG:`/`WARNING:`/`Oops`/`list_*corruption`/`soft lockup`/`free_peer: drained`。**只扫标记点之后的 dmesg** |
| `dmesg_since` | 同上思路 | 既有 `assert_dmesg`/`wait_dmesg` 全量 grep dmesg，会把**上一轮遗留的行**当本轮证据；新 demo 一律用 scoped 版本 |
| `link_shape / link_reset` | `python/trafficcontrol.py` | tc `prio` qdisc + u32 filter 按目的 IP 分流，netem 只挂 peer 那条 band —— **SSH 不被整形**，否则测试框架自己先断 |
| `assert_module_fresh` | — | 比对 `/sys/module/dm_pcache/srcversion` 与 `/root/dm-pcache.ko`。`dm_pcache_phase1_demo.sh` 是 `rmmod \|\| true` + `insmod \|\| true`，设备没干净移除时**旧模块会静默留用**，任何 A/B 结论就此失效 |
| `wait_established` | — | 轮询到 `bm_dirty=0` 再开始负载。此前入队的写走脏位图而非上线，测出来的是欠债不是被测项 |
| `dump_divergence` | — | md5 不一致的**当下**输出逐 8 MiB 分块、两端 device-vs-backing、`bm_dirty`、以及限定本次 demo 的内核日志 |
| `fault_arm / fault_disarm / fault_count` | `set_fault_injection`（`io-errors`）| 按需让 Secondary 拒收写，不必等真实故障 |

> **`forbidden_mark` 用的是单调时间戳，不是行号。** 早期版本记 `dmesg | wc -l` 的
> 行号偏移，在内核环形缓冲**回绕**时会失效 —— 旧行被丢弃、行号前移，标记落到目标行
> 之后，扫描范围变空。这不只让某条断言误报，还让 `assert_no_forbidden` **永远 PASS**。
> 故障注入的日志量正是压破它的那根稻草。详见修复记录附录 B 第 5 条。

---

## 目录

**Section 1 — Replication Path**：[F01](#f01-同步写复制-protocol-c) · [F02](#f02-多-peer-fan-out-广播写) · [F03](#f03-零拷贝网络传输-msg_splice_pages) · [F04](#f04-ack-超时回收--自动重连) · [F05](#f05-移除-peer-重加入恢复-force-resync) · [F06](#f06-req_preflush-跨集群-flush-barrier)
**Section 2 — Network & Mesh**：[F07](#f07-全-mesh-accept-派发) · [F08](#f08-基于-id-的连接方向) · [F09](#f09-so_keepalive-死链检测)
**Section 3 — Dirty Tracking & Resync**：[F10](#f10-每-peer-pmem-持久脏位图) · [F11](#f11-基于-bitmap-的增量-resync) · [F12](#f12-双向-bitmap-merge) · [F13](#f13-generation-计数--peer_gen_at_sync)
**Section 4 — Persistence & Crash Recovery**：[F14](#f14-pmem-双拷贝-crc-header) · [F15](#f15-持久化-peer-注册表-32-槽) · [F16](#f16-resync_in_progress-崩溃检测-破坏性) · [F17](#f17-幂等-add-peer)
**Section 5 — HA / Role Management**：[F18](#f18-角色写围栏) · [F19](#f19-set-role-在线切角色) · [F20](#f20-协调式-demote-握手) · [F21](#f21-worker-kthread-切换)
**Section 6 — Quorum & Split-brain Safety**：[F22](#f22-quorum-检查) · [F23](#f23-可配置-quorum) · [F24](#f24-split-brain-安全无静默收敛) · [F25](#f25-split-brain-人工裁决)
**Section 7 — Operational Interface**：[F26](#f26-dmsetup-table-配置) · [F27](#f27-add-peer--remove-peer--list-peers) · [F28](#f28-resync-控制消息) · [F29](#f29-状态报告) · [F30](#f30-干净-pmem-reset)
**Section 8 — Performance / Multi-stream**：[F31](#f31-多-stream-复制-streams2) · [F32](#f32-多-stream-churn-鲁棒性) · [F33](#f33-cache-压力下的-deferred-write-复制)
**Section 9 — Fault Injection & Recovery**：[F34](#f34-元数据双拷贝损坏恢复) · [F35](#f35-primary-崩溃--failover--重加入-破坏性)
**Section 13 — Resync bookkeeping**：[F47](#f47-被打断的-resync欠债必须活过重连) · [F48](#f48-短-resync-不得在写还在飞时宣告完成) · [F49](#f49-打断到两个-peer-的-resync两条都必须自行恢复) · [F50](#f50-一个-peer-先完成不得动到另一个-peer-的欠债) · [F51](#f51-resync-启动那一刻已有在飞的写)

**Section 10 — Concurrency（控制面操作 × 实时写负载）**：[F36](#f36-写负载下的-peer-churn) · [F37](#f37-写负载下的-secondary-重启) · [F38](#f38-写负载下的角色切换) · [F39](#f39-resync-期间持续写) · [F40](#f40-跨流重叠并发写保序)
**Section 11 — Fault Injection**：[F41](#f41-注入的-secondary-写失败不得传到用户) · [F42](#f42-注入的-resync-写失败不得被-ack)
**Section 12 — Load / abnormal flow**：[F43](#f43-写负载下的-resync-进展) · [F44](#f44-非对齐与奇数长度-io) · [F45](#f45-内存压力下的复制) · [F46](#f46-soak--反复-resync--变速写负载)

**其它工具**：[测试工具总览](#测试工具总览)（BIO 后端回归 · 压力回归 · 单节点 BIO 测试 · 性能测量 · 复现与诊断脚本） · [验收结果](#验收结果)

验收结果：DAX 与 BIO 后端的 `all` 各 180 项检查全部通过，施加背景压力时为 180/0 与 179/0，见[验收结果](#验收结果)。

---

# Section 1 — Replication Path

## F01: 同步写复制 (Protocol C)

### 描述
每笔 WRITE 必须在**所有连接的 peer** ACK 后才向上层回应（DRBD Protocol-C 严格同步），确保任一节点宕机数据不丢。

### 实现
- `dm_pcache.c::dm_pcache_map_bio` → `pcache_submit_req`：本地 reserve/commit 与 `pcache_net_submit_write` fan-out 重叠。
- `dm_pcache_state.c::pcache_handle_ack`：每 ACK `pcache_req_put`，refcount 归零 → `bio_endio`。

### 命令
```bash
bash demo_features.sh f01            # 官方跑法
# 底层关键步骤：
ssh root@$NODE1  'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct conv=fsync'
ssh root@$NODE1  'dd if=/dev/mapper/pcache-dist0 bs=1M count=4 iflag=direct | md5sum'
ssh root@$NODE2 'dd if=/dev/mapper/pcache-dist0 bs=1M count=4 iflag=direct | md5sum'
# 预期：node-1/node-2 md5 与输入完全一致
```

### 结果
✅ **PASS** — 输入、node-1、node-2 的 md5 一致。

---

## F02: 多 peer fan-out 广播写

### 描述
Primary 对所有 connected peer 同时 fan-out 一笔 bio，每 peer 独立 ACK，全部 ACK 才完成。验证 3 节点广播一致。

### 实现
- `pcache_net_submit_write` 迭代 `for_each_active_peer`：每 connected peer 一个 `pcache_pending_req`，第 2+ peer 追加 `pcache_req_get`；断开的 peer 走 `pcache_bm_set_range` 标脏（[F05](#f05-移除-peer-重加入恢复-force-resync)）。

### 命令
```bash
bash demo_features.sh f02            # 官方跑法（3 节点）
# 底层：Primary 写 4 MiB，三节点各自读回 md5
ssh root@$NODE1  'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct conv=fsync'
for n in $NODE1 $NODE2 $NODE3; do ssh root@$n 'dd if=/dev/mapper/pcache-dist0 bs=1M count=4 iflag=direct|md5sum'; done
# 预期：node-1/2/3 md5 全一致
```

### 结果
✅ **PASS** — node-1/2/3 的 md5 一致（node-3 正常参与 mesh）。

---

## F03: 零拷贝网络传输 (MSG_SPLICE_PAGES)

### 描述
复制 payload 用 `MSG_SPLICE_PAGES` 零拷贝送出，吞吐应 ≫ 100 MB/s。

### 实现
- `dm_pcache_net.c::pcache_send_bio_pages`：`bio_for_each_segment` + `iov_iter_bvec(ITER_SOURCE)` + `msg_flags = MSG_NOSIGNAL | MSG_SPLICE_PAGES` + `sock_sendmsg`。

### 命令
```bash
bash demo_features.sh f03            # 官方跑法
# 底层：Primary 写 200 MiB，看 dd 速度
ssh root@$NODE1 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=1M count=200 oflag=direct'
# 预期：200 MiB 写完成，速度 >> 100 MB/s（吞吐证据型，不做 md5 断言）
```

### 结果
✅ **PASS** — 200 MiB 写完成（证据型）。

---

## F04: ACK 超时回收 + 自动重连

### 描述
peer 失联后，已送出等 ACK 的写在 `PCACHE_ACK_TIMEOUT_SECS`(30s) 后被回收（标脏 + 完成 bio 为 EIO），并触发重连。

### 实现
- `pcache_primary_worker` send-loop：对 `streams[0].pending_list` 中 `send_time` 超 30s 的 req → `pcache_bm_set_range` 标脏 + `pcache_req_put(-ETIMEDOUT)`；`any_timed_out` → `pcache_conn_shutdown` → peer 转 CONNECTING 重连。

### 命令
```bash
bash demo_features.sh f04            # 官方跑法（~30s；用 nft）
# 底层：nft 挡 node-2 入站 9764 → Primary 写 → 等 ACK 超时
ssh root@$NODE2 'nft add table inet block; nft add chain inet block input { type filter hook input priority 0\; }; nft add rule inet block input tcp dport 9764 drop'
ssh root@$NODE1  'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct'
ssh root@$NODE1  'dmesg | grep -E "ACK timeout|ACK recv failed"'
ssh root@$NODE2 'nft delete table inet block'
# 预期：dmesg 出现 ACK timeout / ACK recv failed
```

### 结果
✅ **PASS** — dmesg 出现 `ACK timeout|primary recv: ACK recv failed`。

---

## F05: 移除-peer 重加入恢复 (force-resync)

### 描述
被 `remove-peer` 的节点重加入后，如何追平其缺席期间的写。**关键发现见 [附录 A](#附录-a--关键发现-findings)**。

### 实现
- `remove-peer`（`pcache_remove_peer`）会**销毁该 peer 的脏位图**；重加入的 peer 位图全新（干净）→ 增量 resync 传 0 run。
- 正确恢复是 `pcache_net_force_resync`（置 FULL_SYNC → 全量）。瞬时断连（[F11](#f11-基于-bitmap-的增量-resync)）因 peer 仍在册才走增量。

### 命令
```bash
bash demo_features.sh f05            # 官方跑法（3 节点）
# 底层：remove-peer → 缺席时写 delta → add-peer → force-resync → 轮询收敛
ssh root@$NODE1 'dmsetup message pcache-dist0 0 remove-peer 2'
ssh root@$NODE1 'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=2 seek=4 oflag=direct conv=fsync'
ssh root@$NODE1 'dmsetup message pcache-dist0 0 add-peer 2 '"$NODE3"':9764; dmsetup message pcache-dist0 0 force-resync'
# 预期：force-resync 后 node-1/node-3 的 8 MiB md5 收敛（~5s）
```

### 结果
✅ **PASS** — 重加入并 `force-resync` 后，node-1 与 node-3 的数据约 5 秒内一致。背景见 [Finding 1](#附录-a--关键发现-findings)。

---

## F06: REQ_PREFLUSH 跨集群 flush barrier

### 描述
`REQ_PREFLUSH`（fsync）是跨集群 durability barrier：Primary 本地 flush 后，把 flush 复制给每个 peer，等全部 ACK 才完成，保证 Secondary 元数据也持久。

### 实现
- `dm_pcache.c::pcache_submit_req`：本地 flush 后，若 `is_primary && peer_connected` → `pcache_net_submit_flush`（仅 stream[0]，size=0 + `PCACHE_F_FLUSH`）。
- secondary `pcache_handle_data_recv` 拦 `F_FLUSH` → `pcache_cache_flush` → 回 ACK。

### 命令
```bash
bash demo_features.sh f06            # 官方跑法
# 底层：conv=fsync 写，必须完成、无 "Connection timed out"
ssh root@$NODE1 'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 bs=1M count=4 oflag=direct conv=fsync'
# 预期：fsync 完成（peer ACK 了 flush），无 ETIMEDOUT
```

### 结果
✅ **PASS** — fsync flush 被复制 + ACK，无 timeout。

---

# Section 2 — Network & Mesh

## F07: 全 mesh accept 派发

### 描述
单一 listen socket + accept 线程按 `(node_id, stream_id)` 把入站连接派发到对应 peer/stream slot。

### 实现
- `dm_pcache_state.c::pcache_accept_thread`：`kernel_accept` → `recv/send_handshake` → 按 node_id 找 peer → `pcache_conn_install` 到 stream slot；stream 0 置 `PCACHE_C_CONNECTED`。

### 命令
```bash
bash demo_features.sh f07            # 官方跑法（3 节点）
for n in $NODE1 $NODE2 $NODE3; do ssh root@$n 'dmesg | grep "accept: handed inbound sock"'; done
# 预期：≥1 节点 dmesg 见 accept 派发
```

### 结果
✅ **PASS** — 2/3 节点见 `accept: handed inbound sock`。

---

## F08: 基于 id 的连接方向

### 描述
低 node_id 主动 connect、高 node_id 被动 accept，避免双向重复建连。

### 实现
- `pcache_peer_is_initiator(peer) = (my_node_id < peer->node_id)`；primary/secondary worker 与 `exchange_sync_params/bitmaps` 均按 initiator 分支。

### 命令
```bash
bash demo_features.sh f08            # 官方跑法（3 节点）
for n in $NODE1 $NODE2 $NODE3; do ssh root@$n 'dmesg | grep -E "handshake complete \((initiator|responder)\)"'; done
# 预期：≥1 节点见 initiator/responder 角色
```

### 结果
✅ **PASS** — 3/3 节点见 handshake initiator/responder。

---

## F09: SO_KEEPALIVE 死链检测

### 描述
连接静默中断（非 RST）时，靠 TCP keepalive 在 ~50s 内检出死链。

### 实现
- `dm_pcache_net.c::pcache_apply_socket_opts`：`TCP_KEEPIDLE=20s` / `TCP_KEEPINTVL=10s` / `TCP_KEEPCNT=3` → 最坏 ~50s。

### 命令
```bash
bash demo_features.sh f09            # 官方跑法（~55s；用 nft）
# 底层：挡端口 → 等 ~55s → 看 dmesg 死链
ssh root@$NODE2 'nft add table inet block; ...; nft add rule inet block input tcp dport 9764 drop'
sleep 55; ssh root@$NODE1 'dmesg | grep -E "ACK recv failed|connection lost"'
ssh root@$NODE2 'nft delete table inet block'
# 预期：dmesg 出现 ACK recv failed / connection lost
```

### 结果
✅ **PASS** — dmesg 出现 `ACK recv failed|connection lost`。

---

# Section 3 — Dirty Tracking & Resync

## F10: 每-peer PMEM 持久脏位图

### 描述
每个 peer 拥有独立的 4 KiB/bit 脏位图，持久化在 PMEM 的专属 slot，重启不丢。

### 实现
- `struct pcache_bitmap`（per-peer，`slot_idx` + `pmem_base`）；`pcache_peer_bitmap_init` 在 `pcache_add_peer` 内调用；slot header magic `PCACHE_BM_MAGIC`（`PCACMB1\3`）。

### 命令
```bash
bash demo_features.sh f10            # 官方跑法
# 底层：读 slot 0 PMEM 偏移 0x7008 的 8 字节 magic
ssh root@$NODE1 'dd if=/dev/pmem0 bs=1 skip=$((0x7008)) count=8 status=none | xxd'
# 预期：小端 03 31 42 4d 43 41 43 50 = "PCACMB1\3"
```

### 结果
✅ **PASS** — slot 0 magic 命中。

---

## F11: 基于 bitmap 的增量 resync

### 描述
peer 断线期间的写被标脏；重连后只同步脏区（delta），而非全量。

### 实现
- `dm_pcache_state.c::pcache_do_resync`：`while pcache_bm_find_next_dirty` → 读 backing → 送 `P_RS_DATA` → 收 `P_RS_ACK` → `pcache_bm_clear_range`；chunk = 256 KiB。

### 命令
```bash
bash demo_features.sh f11            # 官方跑法（~75s；用 nft）
# 底层：挡端口 55s → 离线写 1 MiB → 解封 → 看 resync starting
ssh root@$NODE1 'dmesg | grep "resync starting"'
# 预期：出现 "dirty=256 / 131072"（增量）而非 "dirty=131072"（全量）
```

### 结果
✅ **PASS** — dmesg 前两轮 `dirty=131072`（初次全量），第 3 轮 `resync starting (dirty=256 / 131072 bits)` = 增量。

---

## F12: 双向 bitmap merge

### 描述
重连时双方交换各自脏位图并合并（`P_BITMAP` + RUN 编码），保证任一侧的脏区都被覆盖。

### 实现
- `pcache_send_bitmap_full`（walk find_next_dirty → 每 run 一个 `pcache_bitmap_pkt`，`PCACHE_BM_ENC_RUN`）+ `pcache_recv_bitmap_merge`（`pcache_bm_set_range`）；`pcache_exchange_bitmaps` 按 initiator 收发顺序。

### 命令
```bash
bash demo_features.sh f12            # 官方跑法
ssh root@$NODE1 'dmsetup message pcache-dist0 0 force-resync'
for n in $NODE1 $NODE2; do ssh root@$n 'dmesg | grep bitmap-exchange'; done
# 预期：≥1 节点见 bitmap-exchange
```

### 结果
✅ **PASS** — 2/2 节点见 `bitmap-exchange`。

---

## F13: Generation 计数 + peer_gen_at_sync

### 描述
device 级单调 generation + 每-peer `peer_gen_at_sync`，是 resync 方向与 split-brain 判定的依据。

### 实现
- `struct dm_pcache { u64 generation; bool gen_dirty; spinlock_t gen_lock; }`；`pcache_bm_set_range` bump gen；`pcache_bm_mark_synced` 写 `peer->bm->peer_gen_at_sync` 并 reset `gen_dirty`；`decide_sync_role` 4-case 比较。

### 命令
```bash
bash demo_features.sh f13            # 官方跑法
ssh root@$NODE1 'dmsetup message pcache-dist0 0 bitmap-status'
ssh root@$NODE1 'dmsetup message pcache-dist0 0 force-resync; dmesg | grep -E "gen="'
# 预期：bitmap-status 显示 gen / peer_gen_at_sync，force-resync 后 gen 推进
```

### 结果
✅ **PASS** — gen + peer_gen_at_sync 可见。

---

# Section 4 — Persistence & Crash Recovery

## F14: PMEM 双拷贝 CRC header

### 描述
所有 on-media 元数据块用双拷贝 + seq + CRC：交替写 2 个槽、递增 seq，读时取 CRC 有效且 seq 最新的。

### 实现
- `pcache_internal.h`：`struct pcache_meta_header{crc,seq,version}`、`pcache_meta_crc()`(crc32c)、`pcache_meta_seq_after()`(8-bit 环回)、`pcache_meta_find_latest()`。
- 写入：`memcpy_flushcache` → `pmem_wmb()`（如 `pcache_bm_header_flush`）。

### 命令
```bash
bash demo_features.sh f14            # 官方跑法
# 底层：读 bitmap header 两份拷贝
ssh root@$NODE1 'dd if=/dev/pmem0 bs=1 skip=$((0x7000)) count=16 status=none | xxd'
ssh root@$NODE1 'dd if=/dev/pmem0 bs=1 skip=$((0x8000)) count=16 status=none | xxd'
# 预期：两份 header CRC 均有效但 seq 不同（交替写者）
```

### 结果
✅ **PASS** — 两份 header 可见。

---

## F15: 持久化 peer 注册表 (32 槽)

### 描述
`(node_id, addr, generation)` 按 bitmap slot 持久化在 PMEM，使 slot↔bitmap 归属跨重启保持。

### 实现
- `dm_pcache_registry.c`：`pcache_registry_init/add/remove/find_slot_by_node_id`；on-media `pcache_peer_registry_onmedia`（32 entry，`IN_USE|TOMBSTONE` flags，magic `PCACMPR3`）。

### 命令
```bash
bash demo_features.sh f15            # 官方跑法
# 底层：remove device + 模块重载 + 重建（不 re-add-peer）
ssh root@$NODE1 'dmsetup remove pcache-dist0; rmmod dm_pcache; insmod /root/dm-pcache.ko; \
    dmsetup create pcache-dist0 --table "0 $SIZE pcache-dist /dev/pmem0 $LOOP my_node_id=0 is_primary=1 listen_port=9764"; \
    dmesg | grep "registry: loaded"'
# 预期：registry 从 PMEM 载回，记得旧 peer
```

### 结果
✅ **PASS** — 模块重载后 registry 仍在。

---

## F16: RESYNC_IN_PROGRESS 崩溃检测 `[破坏性]`

### 描述
resync 进行中崩溃时，重启应从 PMEM 上残留的 `RESYNC_IN_PROGRESS` 标志检出，强制下次全量重做。**结果见发现说明**。

### 实现
- `dm_pcache_bitmap.h`：`PCACHE_BM_F_RESYNC_IN_PROGRESS = BIT(1)`。
- `pcache_do_resync` 入口 set + flush；`pcache_bm_mark_synced` 出口 clear。
- `pcache_peer_bitmap_init`（`dm_pcache_bitmap.c`）load 到该标志 → log `previous resync was interrupted; forcing FULL_SYNC` + 置 `FULL_SYNC_NEEDED`。

### 命令
```bash
bash demo_features.sh f16-run        # 破坏性：调 companion test-crash
# 底层：Primary 写 + force-resync → sysrq+c 硬崩 → 恢复 → 重载 → 查 dmesg
ssh root@$NODE1 'echo c > /proc/sysrq-trigger'   # 崩溃
# 预期：崩溃恢复通；若崩在 resync 进行中，dmesg 见 "previous resync was interrupted; forcing FULL_SYNC"
# 注：要可靠触发需 256 MiB+ workload（4 MiB resync < 1s，标志来不及 flush）
```

### 结果
⚠️ **证据型** — node-1 sysrq+c 崩溃后恢复到工作态（重启 → pmem0 fsdax → 模块重载 → 设备重建 → `bitmap[slot=0 node_id=1]: loaded gen=2 ... dirty=0` → resync clean）。小 workload 下 resync 在标志落盘前已完成，`previous resync was interrupted` 日志不一定出现；可靠触发需要 256 MiB 以上的 workload（见 FEATURES.md）。

---

## F17: 幂等 add-peer

### 描述
registry 已有某 peer 时再 `add-peer` 应复用旧 slot（幂等），而非 EEXIST 或分配新槽。

### 实现
- `pcache_add_peer` 开头 `find_slot_by_node_id`：slot 存在且 `peers[]` 空 → 重建 in-memory peer；存在且非空 → `-EEXIST`；不存在 → `registry_add` 新槽。

### 命令
```bash
bash demo_features.sh f17            # 官方跑法
# 底层：remove + 重载 + 重建，registry 已有旧 peer 时 add-peer
ssh root@$NODE1 'dmsetup message pcache-dist0 0 add-peer 1 '"$NODE2"':9764; echo exit=$?'
# 预期：exit=0（幂等复用槽），非 EEXIST
```

### 结果
✅ **PASS** — 复用 registry 槽，非 EEXIST。

---

# Section 5 — HA / Role Management

## F18: 角色写围栏

### 描述
分布式模式下只有 Primary 能服务 WRITE；Secondary 写被拒（否则重连制造 split-brain）。

### 实现
- `dm_pcache.c::dm_pcache_map_bio` 前置 guard：`net && !is_primary && WRITE → bio_io_error`。

### 命令
```bash
bash demo_features.sh f18            # 官方跑法
ssh root@$NODE1  'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct; echo $?'
ssh root@$NODE2 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct; echo $?'
# 预期：Primary 写 exit=0，Secondary 写 exit≠0（EIO）
```

### 结果
✅ **PASS** — Primary 写成功 / Secondary 写被拒。

---

## F19: set-role 在线切角色

### 描述
运行时降/升角色：降 Primary、force-升 Secondary，写权限随之对调。

### 实现
- `dm_pcache_state.c::pcache_set_role`（+`pcache_set_role_swap_workers` 起停 primary/secondary worker 线程对）。

### 命令
```bash
bash demo_features.sh f19            # 官方跑法
ssh root@$NODE1  'dmsetup message pcache-dist0 0 set-role secondary'
ssh root@$NODE2 'dmsetup message pcache-dist0 0 set-role primary force'
ssh root@$NODE2 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct; echo $?'  # 新主
ssh root@$NODE1  'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct; echo $?'  # 旧主
# 预期：新主(node-2)写=0，旧主(node-1)写≠0
```

### 结果
✅ **PASS** — 新主 node-2 写 `0`、旧主 node-1 写 `1`（角色对调）。

---

## F20: 协调式 demote 握手

### 描述
promote 前用 `P_DEMOTE_REQ` 请对方先 demote，对方回 `P_DEMOTE_ACK`，避免两个 Primary 并存。

### 实现
- 协议 `PCACHE_P_DEMOTE_REQ/ACK`(0x30/0x31)；`dm_pcache_net.c::pcache_send_demote_req/ack/recv_demote_body`。

### 命令
```bash
bash demo_features.sh f20            # 官方跑法
ssh root@$NODE1  'dmsetup message pcache-dist0 0 set-role secondary'
ssh root@$NODE2 'dmsetup message pcache-dist0 0 set-role primary'
ssh root@$NODE2 'dmesg | grep -E "broadcast DEMOTE_REQ|DEMOTE_ACK"'
ssh root@$NODE1  'dmesg | grep DEMOTE_REQ'
# 预期：node-2 广播 DEMOTE_REQ，node-1 收到
```

### 结果
✅ **PASS** — 两侧见 DEMOTE_REQ/ACK。

---

## F21: Worker kthread 切换

### 描述
降角色后，节点的 worker 线程从 primary 停、secondary 起（或反之）。

### 实现
- `pcache_set_role_swap_workers`：停旧角色 worker、起新角色 worker + 重新握手。

### 命令
```bash
bash demo_features.sh f21            # 官方跑法
ssh root@$NODE1 'dmsetup message pcache-dist0 0 set-role secondary'
ssh root@$NODE1 'dmesg | grep -E "set-role: demoted|handshake complete"'
# 预期：dmesg 显示 worker swap（primary OFF → secondary up）
```

### 结果
✅ **PASS** — demote worker swap 可见。

---

# Section 6 — Quorum & Split-brain Safety

## F22: Quorum 检查

### 描述
DRBD 式本地 quorum 检查（无选举）：连接数(含自己) ≥ 阈值才允许写。委托 [F23](#f23-可配置-quorum) 端到端验证。

### 实现
- `dm_pcache_state.c::pcache_has_quorum`：`connected(含自己) ≥ threshold`（`quorum_min` 或 majority `(total/2)+1`）。

### 命令
```bash
bash demo_features.sh f22            # = 跑 F23
```

### 结果
✅ **PASS** — 见 F23。

---

## F23: 可配置 quorum

### 描述
`quorum=off|majority|N`：majority 下失去多数即拒写（write fencing）。

### 实现
- `pcache_parse_dev_args` 解析 `quorum=`；`dm_pcache_map_bio` 前置 `!pcache_has_quorum → bio_io_error`。

### 命令
```bash
bash demo_features.sh f23            # 官方跑法（~60s）
# 以 quorum=majority 重建 node-1；两 peer 在线(2/2) 写应成功
ssh root@$NODE1 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct; echo $?'
# 挡对端出站等 55s 掉到 1/2 → 写应 EIO
ssh root@$NODE1 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct; echo $?'
# 预期：2/2 写=0；1/2 写≠0
```

### 结果
✅ **PASS** — 2/2 写成功、1/2 写 EIO。

---

## F24: Split-brain 安全（无静默收敛）

### 描述
双 Primary 分歧后重连时，系统必须**不静默覆盖**任一侧数据、**复制停摆**，等人工裁决。**关键发现见 [附录 A](#附录-a--关键发现-findings)**。

### 实现
- `dm_pcache_state.c::pcache_decide_sync_role`：同 gen 但互证断裂 → HALT + log `split-brain detected`。
- **但** `set-role primary force` 置 `FULL_SYNC_NEEDED` → decide 走 full-sync 分支（SYNC_SOURCE）而非 HALT，双主 livelock `SYNC_SOURCE→OFF`。

### 命令
```bash
bash demo_features.sh f24            # 官方跑法（~2.5min；用 nft）
# 底层：建 baseline → 隔离 node-2 55s → 两侧写不同数据(node-2 force-primary) → 解封
ssh root@$NODE1  'dd ... count=1'                                   # node-1 写
ssh root@$NODE2 'dmsetup message pcache-dist0 0 set-role primary force; dd ... count=1'  # node-2 写
# 预期：分歧被保留（node-1≠node-2 md5）且复制停摆（repl 非 ESTABLISHED）
```

### 结果
✅ **PASS** — 分歧数据被保留（node-1 与 node-2 的 md5 不同），两侧 `repl_state SYNC_SOURCE -> OFF`（复制停止），等待人工裁决。断言基于可观测的安全保证，而不是 `split-brain detected` 日志，原因见 [Finding 2](#附录-a--关键发现-findings)。

---

## F25: Split-brain 人工裁决

### 描述
`resolve-split-brain <peer> victim|survivor` 指定牺牲/幸存方后，数据收敛。

### 实现
- `dm_pcache_state.c::pcache_resolve_split_brain`：victim 清 split_brain 标志 + 全量收，survivor 全量发。

### 命令
```bash
bash demo_features.sh f25            # 官方跑法（接 F24 之后）
ssh root@$NODE2 'dmsetup message pcache-dist0 0 resolve-split-brain 0 victim; dmsetup message pcache-dist0 0 set-role secondary force'
ssh root@$NODE1  'dmsetup message pcache-dist0 0 resolve-split-brain 1 survivor'
# 预期：node-1/node-2 md5 收敛相等
```

### 结果
✅ **PASS** — 裁决后 node-1 与 node-2 数据一致。

---

# Section 7 — Operational Interface

## F26: dmsetup table 配置

### 描述
table 行参数（`my_node_id/is_primary/listen_port/quorum/streams`）被正确解析。

### 实现
- `dm_pcache_state.c::pcache_parse_dev_args`；`pcache_net_init` 打印 `network up`。

### 命令
```bash
bash demo_features.sh f26            # 官方跑法
ssh root@$NODE1 'dmsetup table pcache-dist0; dmesg | grep "network up"'
# 预期：参数解析正确 + network up 日志
```

### 结果
✅ **PASS** — 参数解析 + `network up`。

---

## F27: add-peer / remove-peer / list-peers

### 描述
运行时增删列 peer；re-add 幂等。

### 实现
- `dm_pcache_state.c::pcache_add_peer` / `pcache_remove_peer` / `pcache_list_peers`。

### 命令
```bash
bash demo_features.sh f27            # 官方跑法
ssh root@$NODE1 'dmsetup message pcache-dist0 0 add-peer 2 '"$NODE3"':9764'
ssh root@$NODE1 'dmsetup message pcache-dist0 0 list-peers'
ssh root@$NODE1 'dmsetup message pcache-dist0 0 remove-peer 2; dmsetup message pcache-dist0 0 add-peer 2 '"$NODE3"':9764'
# 预期：add/list/remove 均成功，re-add 幂等
```

### 结果
✅ **PASS** — add/remove/list + 幂等 re-add。

---

## F28: resync 控制消息

### 描述
`force-resync` / `abort-resync` / `reset-generation` 三个 resync 控制消息。

### 实现
- `pcache_net_force_resync` / `pcache_abort_resync` / `pcache_reset_generation`。

### 命令
```bash
bash demo_features.sh f28            # 官方跑法
for m in force-resync abort-resync reset-generation; do ssh root@$NODE1 "dmsetup message pcache-dist0 0 $m"; done
# 预期：三个消息均执行，dmesg 有对应日志
```

### 结果
✅ **PASS** — 三个 resync 控制消息均执行。

---

## F29: 状态报告

### 描述
`dmsetup status` 输出 log 三指针等；`bitmap-status` 输出 per-peer gen/脏计数。

### 实现
- `dm_pcache.c::dm_pcache_status`（INFO 附 `bm_gen/bm_dirty`）；`bitmap-status` 分支。

### 命令
```bash
bash demo_features.sh f29            # 官方跑法（3 节点）
ssh root@$NODE1 'dmsetup status pcache-dist0'
ssh root@$NODE1 'dmsetup message pcache-dist0 0 bitmap-status'
# 预期：status 显示 seg/指针 + bm_gen/bm_dirty；bitmap-status 列 per-peer 行
```

### 结果
✅ **PASS** — status（`… bm_gen=1 bm_dirty=0/131072`）+ per-peer bitmap-status。

---

## F30: 干净 PMEM reset

### 描述
一键清 3 VM 的 PMEM + 拆设备，回到干净态。

### 实现
- companion `dm_pcache_phase1_demo.sh::cmd_reset`（3-node aware：stop + zero SB + detach loop）。

### 命令
```bash
bash demo_features.sh f30            # = time bash dm_pcache_phase1_demo.sh reset
# 预期：3 VM 秒级清空完成
```

### 结果
✅ **PASS** — reset 完成。

---

# Section 8 — Performance / Multi-stream

## F31: 多 stream 复制 (streams=2)

### 描述
每个 peer 可开多条 TCP 数据流，按 `hash_64(sector)` 分流（同区恒定同流，保序），提升并发。

### 实现
- `dm_pcache_net.h::struct pcache_stream`（streams[0]=控制，1..=数据）；`pcache_net_submit_write` 按 `hash_64(sector)%nr_streams` 选流（`dm_pcache_state.c`）；per-stream `pcache_data_sender/receiver`。

### 命令
```bash
bash demo_features.sh f31            # 官方跑法（以 streams=2 重建）
# 底层：查 per-stream 线程 + 写 8 MiB 比对
ssh root@$NODE1  'ps -eL -o comm | grep -c pcache-dsend'   # 数据流发送线程
ssh root@$NODE2 'ps -eL -o comm | grep -c pcache-dsec'    # 数据流落盘线程
# 预期：两侧 stream 线程各 ≥1，且 8 MiB md5 一致
```

### 结果
✅ **PASS** — node-1 有 1 个 dsend 线程、node-2 有 1 个 dsec 线程，两端 md5 一致。

---

## F32: 多 stream churn 鲁棒性

### 描述
快速重连/拆建下 socket 生命周期安全（kref refcounted conn），不 deadlock、不 UAF、不 oops。

### 实现
- `dm_pcache_net.c::struct pcache_conn{kref,sock}` + `pcache_conn_get/put/install/drop/shutdown`：最后一个引用归零才释放 socket。

### 命令
```bash
bash demo_features.sh f32            # 官方跑法（~3min）
# 底层：10 轮 recreate(streams=2)+写 8 MiB+拆卸，每轮检查节点存活
# 预期：10 轮后节点仍响应，dmesg 无 soft lockup/BUG:/Oops
```

### 结果
✅ **PASS** — 存活 10 轮 churn，无 soft-lockup/oops。

---

## F33: cache 压力下的 deferred-write 复制

### 描述
cache 满触发 deferred-write（`-EBUSY` 重试）时，复制仍正确（deferred 写在重试成功时才复制，不双发不漏发）。

### 实现
- `dm_pcache.c::pcache_submit_req`：reserve 是唯一 `-EBUSY` 步骤且在任何网络发送之前 → `defered_req_fn` 重试时经同一路径复制。

### 命令
```bash
bash demo_features.sh f33            # 官方跑法
# 底层：32 MiB fsync 突发写（迫使 cache-full → deferred）→ 比对
ssh root@$NODE1 'dd if=/tmp/p.bin of=/dev/mapper/pcache-dist0 bs=1M count=32 oflag=direct conv=fsync'
# 预期：node-1/node-2 md5 一致（deferred 写也被复制）
```

### 结果
✅ **PASS** — 32 MiB 突发写后两端 md5 一致。

---

# Section 9 — Fault Injection & Recovery

## F34: 元数据双拷贝损坏恢复

### 描述
归零某个双拷贝元数据块的一份拷贝后，设备仍能经存活的另一份加载、数据零丢失（验证 CRC 冗余的容错）。

### 实现
- `pcache_internal.h::pcache_meta_find_latest`：跳过 CRC 失败的拷贝、取 seq 最新的有效副本。cache_info 双拷贝于 PMEM `0x2000`/`0x3000`。

### 命令
```bash
bash demo_features.sh f34            # 官方跑法（smoke 内）
# 底层：remove device → 归零 cache_info 拷贝 0（0x2000）→ 重建
ssh root@$NODE1 'dmsetup remove pcache-dist0; dd if=/dev/zero of=/dev/pmem0 bs=4096 seek=2 count=1 conv=notrunc; sync'
# 重建后读回 4 MiB md5
# 预期：设备成功加载（用存活拷贝 1）且数据 byte-identical
```

### 结果
✅ **PASS** — 归零拷贝 0 后设备经拷贝 1 加载（`device loaded=1`），数据完好，重新 add-peer 成功。断言为“设备加载 + 数据完好”，见附录 A 备注。

---

## F35: Primary 崩溃 → failover → 重加入 `[破坏性]`

### 描述
端到端 HA 故障恢复：Primary 硬崩溃后，(a) 数据在幸存者上不丢、(b) Secondary 升主继续服务、(c) 崩溃节点重加入并收敛。分 3 个子断言。

### 实现
- 崩溃：`echo c > /proc/sysrq-trigger`；恢复：`recover_vm`（virsh destroy+start）。
- failover：`pcache_set_role(primary, force)`；重加入：以 `is_primary=0` 重建 + `add-peer` + `pcache_net_force_resync`。

### 命令
```bash
bash demo_features.sh f35            # 破坏性：crash node-1，用 virsh 恢复
# 底层：写 4 MiB fsync → 记 node-2 md5 → sysrq+c 崩 node-1 → 恢复
#       → 升 node-2 主 + 写新数据 → node-1 以 Secondary 重建 + force-resync
# 预期：F35a node-2 仍持数据；F35b node-2 能写；F35c node-1 收敛到 node-2
```

### 结果
- ✅ **F35a 持久性** — 崩溃后 node-2 仍持有 fsync 过的数据。
- ✅ **F35b failover** — node-2 升为 Primary 后写入成功。
- ✅ **F35c 重加入 resync** — 崩溃的 node-1 以 Secondary 重加入并 force-resync，数据与新 Primary 一致。

---

# Section 10 — Concurrency（控制面操作 × 实时写负载）

F01–F35 里**每个 I/O 都是阻塞式的顺序 `dd`**（全文件没有一个后台 `&`）：写在控制面
动作开始前就已经结束，所以"入队却永不完成的请求"根本观察不到。这正是协商窗口
bio 悬挂能躲过全部 35 项的原因。

本节补上这条轴 —— 后台写手贯穿控制面动作。统一断言：

| | 断言 | 用的助手 |
|---|---|---|
| (a) | **不挂死** —— 写手必须在自己 runtime 内退出 | `busy_wait`（带超时）|
| (b) | **数据收敛到已知模式** —— 不是"两端相等"，避免平凡通过 | `primary_write` + `assert_md5_eq` |
| (c) | **无用户可见 I/O 错误** —— peer 丢失应降级，不该 EIO | `busy_errors` |
| (d) | **内核日志干净** | `assert_no_forbidden` |

场景取自 DRBD 9 测试套件（`concurrent-disconnect` / `switch-primaries` /
`write-during-resync` 等）；DRBD 那 145 项里大部分针对 pcache-dist 没有的功能
（diskless、two-phase commit、protocol A、online verify、多 volume 等），可迁移的
是**测试轴**而非测试项。

## F36: 写负载下的 peer churn

### 描述
写 I/O 进行中反复 `remove-peer` / `add-peer`。取自 `drbd9-tests/tests/concurrent-disconnect`
—— DRBD 那条测试循环 10 次并注明"the potential deadlock is a race"，这里同理：
"socket 已连"到"协商完成"之间的窗口只有毫秒级。

### 实现
- `busy_start`（后台 fio randwrite 64k/qd32）→ 4 轮 remove/add-peer → `busy_wait`。
- 收尾按 `drbd9-tests/tests/split-brain` 的形状：churn → 可能 latch → 运维裁决 → 数据收敛。
- 裁决必须对**每个**报告 split-brain 的节点做（Primary=`survivor`、Secondary=`victim`）；
  HALT 的 worker 只有 `resolve-split-brain` 能重启，`force-resync` 打不破。

### 命令
```bash
bash demo_features.sh f36            # 官方跑法（~2min）
# 底层：后台写手 + 4 轮 churn；失败时看两端 dmesg 的 split-brain / drop dup
ssh root@$NODE1 'dmsetup message pcache-dist0 0 remove-peer 1; sleep 2; \
    dmsetup message pcache-dist0 0 add-peer 1 '"$NODE2"':9764'
# 预期：写手正常退出、io_errors=0、数据收敛到 churn 后写入的已知模式
```

### 结果
✅ **PASS**（8 项断言）。该用例发现了修复记录 #9（`b69033fc9b66`）：重加入的 peer 会造成
假脑裂，且 HALT 循环占住连接，使对端得不到诊断信息。修复后本用例报告 `did not latch`。

---

## F37: 写负载下的 Secondary 重启

### 描述
写 I/O 进行中，Secondary 侧按**手册 §8.1 的标准顺序**重启：`dmsetup create` 先把
listen socket 拉起，`add-peer` 稍后才下。这个间隙里 accept 打
`no peer slot for node_id; drop`，Primary 侧 sync_params 收 ECONNRESET。

### 实现
- 这个窗口**不是人造的**：节点重启时脚本或 systemd 天然存在这个间隙。12 秒间隙内命中 11 次。
- 用 `assert_dmesg` 确认窗口确实被命中，否则断言毫无意义。

### 命令
```bash
bash demo_features.sh f37            # 官方跑法（~2min）
# 底层：后台写手 → node-2 dmsetup remove → 仅 create（不 add-peer）→ 等 12s → add-peer
ssh root@$NODE1 'dmesg | grep -c "sync_params exchange failed"'
# 预期：窗口被命中（计数 > 0）、写手正常退出、io_errors=0、数据收敛
```

### 结果
✅ **PASS**（6 项断言）。本用例是修复记录 #5（`6b9406f862a4`，协商窗口 bio 悬挂）的回归测试，
并发现了 #13（`0d6e720d70e9`，NACK 使用户写入返回 EIO）。

---

## F38: 写负载下的角色切换

### 描述
写 I/O 进行中 demote → promote 往返。取自 `drbd9-tests/tests/switch-primaries`。

### 实现
- **与 F36/F37 不同，这里不能断言"零 I/O 错误"**：降级后写被围栏拒绝是设计行为
  （手册 §6.1）。所以断言改为：围栏**确实生效**（探测写必须失败）、角色往返
  不悬挂、之后设备完全可用且内容到达 peer。

### 命令
```bash
bash demo_features.sh f38            # 官方跑法（~2min）
# 底层：后台写手 → set-role secondary → 探测写必须失败 → set-role primary force
ssh root@$NODE1 'dmsetup message pcache-dist0 0 set-role secondary'
ssh root@$NODE1 'dd if=/dev/zero of=/dev/mapper/pcache-dist0 bs=4K count=1 oflag=direct; echo $?'
# 预期：降级后探测写 exit≠0（围栏生效）；升回后设备可用且内容到达 peer
```

### 结果
✅ **PASS**（6 项断言）。

---

## F39: resync 期间持续写

### 描述
resync 进行中应用持续写入。取自 `drbd9-tests/tests/write-during-resync` —— 那条测试
的重点不是 resync 本身而是**重叠**，DRBD 用 tc 整形让重叠可复现而非碰运气。

### 实现
- **tc 整形是关键**：不限速时测试环境中的 resync 只需 0.3 秒，重叠窗口无法稳定出现；
  `link_shape` 把 peer 方向延迟拉到 10ms 把 sweep 拉长。
- **写手必须限速**（`--rate_iops=400`，可用 `F39_RATE_IOPS` 覆盖）。逐 chunk 的 writeback drain
  要求**整个缓存**干净才读 backing，不限速的写手（约 530 MiB/s、8474 IOPS）不给这个机会：
  预循环超时 30 秒、chunk 0 再超时 30 秒，sweep 一个 chunk 都没发就结束（`incomplete, 131072 bit(s)
  still owed`）。此时最后的 md5 一致来自 resync 放弃后的内联复制，验证的是内联路径而不是重叠。
- 写手 runtime 必须**长过** resync，否则尾段跑在静止设备上、平凡收敛。
- **断言 sweep 真的搬了东西**（`owed < total`），否则整条用例什么也没证明 ——
  与 F41 "先确认故障确实被注入"同一个理由。
- 断言的是真正的不变式：**写手停后两端必须自行一致，不靠补一次 force-resync**。

### 命令
```bash
bash demo_features.sh f39            # 官方跑法（~5-6min；用 tc）
F39_RATE_IOPS=800 bash demo_features.sh f39   # 调写手速率
# 底层：link_shape 10ms → 后台写手 200s @400 IOPS → force-resync → 等 resync 裁决
ssh root@$NODE1 'dmesg | grep -E "resync (done|incomplete)"'
# 预期：resync 给出裁决、不挂死、io_errors=0、两端 64 MiB md5 一致
```

### 结果
✅ **PASS**。该用例的间歇性失败对应修复记录 #14（`b4f4d9672490`）：逐 chunk 的 drain 看不见仍在
未关闭 kset 中的 key，于是清除了脏位却发送了陈旧的 backing 数据。修复前 2/2 分歧，修复后 3/3 一致。

**#14 的专项回归在 [`tests/repro/f39_silent_loss.sh`](../tests/repro/f39_silent_loss.sh)**，
不在本用例里。它比 F39 灵敏得多：在裁决瞬间杀掉写手（断掉事后的内联修复窗口）、
先用一次静置 resync 还清**已记录**的欠债，剩下的差异才是"既没送到也没记录"的。
F39 本身断言写手停止后两端自行一致，并断言 sweep 确实搬运了数据。

本用例内建 `dump_divergence`：检测到不一致的当下自动输出逐 8 MiB 分块、两端
device-vs-backing、`bm_dirty`、以及**限定本次 demo 范围**的内核日志。
判读规则：Secondary 的 device 与 backing **都**不同 → 数据从未送达；
只有 device 不同 → Secondary 缓存层问题。

---

## F40: 跨流重叠并发写（保序）

### 描述
验证手册 §3.3 的**每区保序**不变式：写按 `hash_64(sector)` 分流，同一区恒定走同一
stream。此前无人测过 —— 其余 demo 都是顺序写，任何排序都会给出相同结果。

### 实现
- `recreate_streams 2` 建多流设备，先确认 `pcache-dsend` 线程存在（否则本用例毫无意义）。
- 4 个 job 反复重写**同一 8 MiB 区域**，让同块碰撞频繁发生。
- **刻意不做 force-resync**：要证明的是普通复制就保持一致，而非 resync 能事后修补。
- 先 `wait_established` 再开始负载，否则测到的是脏位图欠债而非保序。
- 结束时断言 `bm_dirty=0`，确保比对的确实是"已上线的写"。

### 命令
```bash
bash demo_features.sh f40            # 官方跑法（~1min；streams=2）
# 底层：4 job × 4k randwrite × 同一 8 MiB 区域 × 25s，然后直接比对
ssh root@$NODE1 'dmsetup status pcache-dist0 | grep -oE "bm_dirty=[0-9]+"'
# 预期：bm_dirty=0（全部内联复制）且两端 md5 一致
```

### 结果
✅ **PASS**（6 项断言）。该用例发现了修复记录 #10（`281a75149622`）：Secondary 的背压只覆盖
`cache_data_alloc`，未覆盖 `cache_kset_close`，`-EBUSY` 泄漏为 NACK → Primary 标脏 → 多流稳态下
静默欠债（修复前 37 次 `-16`、`bm_dirty=36`、数据分歧；修复后均为 0、数据一致）。

---

# Section 13 — Resync Bookkeeping

`resync incomplete, N bit(s) still owed ... left dirty for the next pass` 只有在
"下一趟真的会来、也真的会还"时才是诚实的。DRBD 有两条用例专门盯这件事：
`resync-finish`（掐掉 resync 最后那个包，问下次连接**还知不知道欠着**；DRBD 到
9.0.24 会忘）和 `brief-resync-oos`（注释第一行就是 *"Demonstrate the necessity of
flushing requests before finishing a resync"*）。本节照此实现。

## F47: 被打断的 resync，欠债必须活过重连

### 描述
移植自 `drbd9-tests/tests/resync-finish`（+ `resync-missed-end-no-bits`）。
pcache-dist 对应那个"最后一个包"的是 `P_BITMAP_DONE`，对应"错过结尾"的是 sweep
自报 incomplete。断言的是 `pcache_do_resync` 那句承诺本身。

### 实现
- 写手**不限速**（与 F39 相反）：这里要的就是让 sweep 放弃，那是本例的前置条件而非断言。
- 第一轮用 **`force-resync`** 而不是断链重连：它会置 `FULL_SYNC_NEEDED` 并 bump
  generation，这是让两端代数走上分岔的那条路径。用断链驱动第一轮的话，对端代数不动，
  第一次静置重连就把一切结清，用例什么也证明不了。
- 之后**在写手仍在跑时**再打断两次 —— 每次重连都会交换位图，收到对端的脏区间会标脏
  本地位图，从而 bump 本节点的 generation。
- 然后才停写手、静置，要求**仅靠重连**结清欠债（`force-resync` 是运维兜底，它置全脏位，
  无论记账对错都能把数据修好，用在这里等于把被测缺陷盖住）。
- 另外断言两件事：Primary 不得把自己判成 `SYNC_TARGET`；不得出现假脑裂 HALT。

### 命令
```bash
bash demo_features.sh f47            # ~7min
ssh root@$NODE1 'dmesg | grep -E "sync_params: my|decided role"'
```

### 结果
✅ **PASS**（6 项）。连续三趟 incomplete（每趟 131072 位全部欠下）之后，一次静置重连结清
全部欠债，角色未反转。

> **局限**：本用例的序列较短，停在修复记录 #15 所述缺陷链（`eb15c0a9bc22`）的触发点之前。
> 回归这一类缺陷以 F48 为准。

---

## F48: 短 resync 不得在写还在飞时宣告完成

### 描述
移植自 `drbd9-tests/tests/brief-resync-oos`。DRBD 到 9.2.13 为止会让 resync 在
"out-of-sync 包还在路上"时就完成。pcache-dist 的同一处是 `pcache_do_resync` 的收尾：
最后一个 chunk → `P_BITMAP_DONE` → `mark_synced` → ESTABLISHED。不变式还是那条：
**每一笔写要么被 resync 带走，要么留在位图上欠着，不能两者都不。**

### 实现
- 不去掐单笔写的时机，而是驱动多次**短** resync（`abort-resync` 几秒就重连，且只同步
  位图里那点东西），让整段运行都是"收尾窗口"。
- 每轮 `abort-resync` 之后紧跟一次 4 MiB 直写：它落在链路断开的窗口里，保证这一轮
  确实有脏位要搬。没有它，多数重连会判成"无需 resync"，这一轮就白跑。
- 写手**限速**（`F48_RATE_IOPS`，默认 200），理由同 F39：饱和写手会让逐 chunk 的
  drain 超时，sweep 一个 chunk 都发不出去。
- 判读分三种，避免把别人的缺陷记到本例头上：两端一致 → 通过；不一致但结清位图后一致
  → 诚实欠债，通过；不一致且 `bm_dirty=0` → **既没送到也没记录**，红灯。

### 命令
```bash
bash demo_features.sh f48                    # ~7min
F48_CYCLES=12 F48_RATE_IOPS=400 bash demo_features.sh f48
```

### 结果
✅ **PASS（6 项）**。

本用例是 `eb15c0a9bc22` 的验收用例。修复前该用例 2/2 失败，属于第四种情况：**欠债已记录，却无法偿还**。
末尾 `bm_dirty=131072/131072`，一次静置重连无法结清。dmesg 中的完整链条：

```
resync starting (dirty=7632) → incomplete, 57760 bit(s) still owed
my(gen=2,peer_at=1) peer(gen=3,peer_at=2) → decided role = SYNC_TARGET   ← 角色反转
BITMAP_EXCHANGE → ESTABLISHED                                            ← 一个字节没传
my(gen=3,peer_at=3) peer(gen=4,peer_at=2) → incompatible generations,
                                            halting replication → HALT   ← 假脑裂
```

即：incomplete 之后照发 `P_BITMAP_DONE`（对端照样 `mark_synced`）、位图交换又 bump
接收方的 generation，几轮之后 Primary 判成 SYNC_TARGET、两端谁也不传，再一轮代数彻底
错开就被判为脑裂。这说明修复记录 #15 的第一次修正（`b3db50472e0a`）不完整。
修复后走到的分支为 "nodes agree with nothing left to settle"。

---

## F49: 打断到两个 peer 的 resync，两条都必须自行恢复

### 描述
移植自 `drbd9-tests/tests/resync-multiple-interrupt`，DRBD 那条的注释点明了重点：
*"test the case where 2 nodes are both SyncTarget and they first connect to each
other on reconnection."*

**哪些搬得过来、哪些搬不过来**（如实记，不假装等价）：

| DRBD 的步骤 | pcache-dist |
|---|---|
| 两个 peer 各自停在 resync 的中途 | ✅ 同样，且两条欠债由**各自的** per-peer 位图分别记账 |
| 两个半同步的 SyncTarget 先彼此重连 | ✅ 同样 |
| 之后 node_c 从 node_b 同步（下游串联）| ❌ **没有对应物** —— 只有 Primary 会驱动 `pcache_do_resync`。因此这一段改为断言两个 Secondary **不对彼此做任何事** |

### 实现
- 单向挡住 Primary → 各 Secondary（`block_outbound_to`），这样**不影响 Secondary
  之间的链路**，第四阶段需要它还活着。
- 挡完之后必须等 **55 秒**：对端只能靠 TCP keepalive 发现链路没了
  （`TCP_KEEPIDLE=20` + 3 × `KEEPINTVL=10`，最坏 50 秒，见 `pcache_setup_socket`）。
  等待不足就重连，会被对端 accept 当成 dup 拒绝。
- `dmsetup status` 的 `bm_dirty` 是**所有 peer 的和**（`total_dirty`），
  所以"这一条被打断了"必须与另一个 peer 还欠的量比较才成立：
  aggregate 掉到对端欠债量以下就说明这条其实是**跑完了**，不是被打断。
  只比较“变小且大于零”会把“跑完”误判为“被打断”。
- 结尾要求两条欠债**仅靠重连**结清，不给 `force-resync` 兜底。

### 命令
```bash
bash demo_features.sh f49            # ~5min，3 节点，用 nft
ssh root@$NODE2 'dmesg | grep -c "drop dup"'
```

### 结果
✅ **PASS（9 项）**。两次打断都发生在中途（分别完成 26% 与 38% 时切断），两条欠债随后仅靠重连
各自结清，三个节点数据一致；两个半同步的 Secondary 先彼此重连时，彼此不搬运数据、不判脑裂。

该用例发现了修复记录 [#16](./pcache-dist-fix-history.md)（`1f59b35fdafb`）：非对称链路故障之后
Secondary 持续拒绝 Primary 的重连（`already connected; drop dup`），而它自认 `bm_dirty=0` 已同步。
本用例在结算失败时打印 dup 计数并指向 #16；确定性复现脚本为
[`tests/repro/wedge_half_close.sh`](../tests/repro/wedge_half_close.sh)
（修复前 3/3 卡住、300 秒不自愈；修复后 3/3 在 10 秒内恢复）。

### 实现要点
- **等 55 秒再期待干净重连**：对端只能靠 TCP keepalive 发现链路断开（最坏 50 秒），
  等待不足会被它的 accept 当成 dup 拒绝。
- **assertion 必须能区分"被打断"和"跑完了"**：`bm_dirty` 是所有 peer 的和，
  只比"变小且大于零"会把"这条跑完了"当成"被打断"。改为与另一个 peer 的欠债量比较。
- **`link_shape` 一次只能整形一个目的地**：它会重建 root qdisc，
  对同一节点连调两次，第一个 peer 的 filter 会被**静默丢掉**。
  症状是"peer node-2 每次都秒完、peer node-3 每次都能打断"。改为每阶段各整形一次。
  helper 处已加注释。

---

## F50: 一个 peer 先完成，不得动到另一个 peer 的欠债

### 描述
移植自 `drbd9-tests/tests/resync-finish2`：「一个源同时向两个 target 做 resync，
快的那个先完成，**不得把另一个 target 的盘标成 Outdated**」。DRBD 到 9.0.24 为止是坏的。

**为什么这条对 pcache-dist 同样成立** —— 耦合就写在 `pcache_bm_mark_synced()` 里：

```c
/* gen_dirty is device-wide; reset on any peer's sync. */
if (pcache->gen_dirty) { pcache->gen_dirty = false; ... }
```

位图是 **per-peer** 的，generation 却是 **per-device** 的 —— 先完成的那个 peer 会伸手改
共享状态。修复记录 [#15](./pcache-dist-fix-history.md) 就是这一族（一个 peer 的完成
污染了另一个的账），而此前没有任何用例守着它。

### 实现
- 单向挡住 Primary → 两个 peer（`block_outbound_to`），再 `abort-resync` 丢掉活 socket，
  然后**等 55 秒**让对端 keepalive 反应过来（同 F49，等不够会被 accept 判 dup）。
- 写 96 MiB，两个 peer 各欠 24576 位 —— 断言 aggregate 是偶数且等分，
  否则后面"少了多少"的判断没有意义。
- 只放 node-2 进来跑完，node-3 仍隔离。
- **核心 assertion**：aggregate 必须从 `2N` 恰好降到 `N`，**不能再少**。
  少了就说明 node-2 的完成吃掉了 node-3 的欠债 —— 这是 DRBD 那句"不得把另一个标
  Outdated"的直接对应。`dmsetup status` 报的是所有 peer 的和，所以只能这样比。
- 再写 8 MiB：node-2 内联复制、node-3 必须继续记账 —— 验的是 node-2 的 `mark_synced`
  清掉 device-wide `gen_dirty` 之后，node-3 是否还会被标记。
- 最后放 node-3 进来，要求**仅靠重连**完成 resync，不给 `force-resync` 兜底。

DRBD 循环 40 次是因为它那两条 resync 在赛跑；本例不需要 —— demo 自己决定谁何时连上，
"一个远早于另一个完成"是安排出来的，不是碰运气。

### 命令
```bash
bash demo_features.sh f50            # ~2min（3 节点，用 nft）
ssh root@$NODE1 'dmsetup status pcache-dist0 | grep -oE "bm_dirty=[0-9]+/[0-9]+"'
```

### 结果
✅ **PASS（9 项）**：

| assertion | 实测 |
|---|---|
| 两 peer 欠债等量 | 49152 = 2 × 24576 |
| node-2 独自跑完 | 3 秒 |
| **node-3 欠债原封不动** | **24576，一位没动** |
| 此后的写仍为 node-3 记账 | 24576 → 26624 |
| node-3 自行 resync 完成 | 无 force-resync |
| Primary 全程未被扰动 | 无 SYNC_TARGET / HALT |
| 三节点一致 | md5 相同 |

---

## F51: resync 启动那一刻已有在飞的写

### 描述
移植自 `drbd9-tests/tests/busy-resync-start`。那条测试真正巧妙的不是"resync 期间写"
（F39/F43 已经在做），而是**第三个被故意拖慢的 peer**：

```python
writing_tc.slow_down(other_n, delay='50ms')
# Including a delayed peer ensures that there are pending writes while
# starting the resync.
```

慢 peer 让 Primary 的复制队列始终不空，于是**向另一个 peer 的 resync 是在"写已经在飞"
的状态下开始的**。

**这对 pcache-dist 比对 DRBD 更要紧**，因为修复记录
[#14](./pcache-dist-fix-history.md) 末尾记着一个**未修**的洞：另有 peer 处于
ESTABLISHED 时，写走快路径，`pcache_net_submit_write()` 对**不能复制的那个 peer**
调 `pcache_bm_set_range()`，而这发生在 `pcache_cache_write_commit()` **之前** ——
位脏早于数据存在。两节点永远走不到（单 peer 时走的是"先 commit 再标脏"的慢路径），
**三节点才会**。

### 实现
- 只整形 node-1 → node-3（50ms）当慢 peer；只有一个目的地，所以不踩
  `link_shape` 一次只能整形一个目的地的那个坑（见 helper 注释与 F49）。
- 隔离 resync 目标 node-2 时**不用 `abort-resync`** —— 它会把慢 peer 的 socket 一起丢掉，
  而慢 peer 正是本例的机制。改为只挡链路、等 55 秒让对端 keepalive 回收。
- 写手**顺序且限速**（`--rw=write --size=256M --rate=10m`，fio 后置选项覆盖
  `busy_start` 的默认值）。DRBD 特意注明写太快会覆盖掉"resync 启动瞬间写下的那些块"，
  从而把丢写掩盖过去。
- 先断言**场景确实成立**（netem 在、写手在跑）才继续 —— 同 F41 的
  `fault_count > 0` 那条理由。
- 判读同 F39 的配方：不一致时先用一次静置 resync 还清**已记录**的欠债，
  还不一致且 `bm_dirty=0` 才叫"既没送到也没记录"。

### 命令
```bash
bash demo_features.sh f51            # ~3min（3 节点，nft + tc）
```

### 结果
✅ **PASS（8 项）**：resync 在写入在飞时 3 秒内给出裁决（`resync done (dirty=0)`），写手正常退出，
`io_errors=0`，欠债不经 force-resync 自行结清，三节点数据一致。

> **局限**：`bm_set_range` 与 `write_commit` 之间的窗口是微秒级，本用例对它是概率性检测，
> 不是确定性复现。要确定性地验证，需要像 #14 那样人为放大窗口，或在代码中断言这两步的顺序。

---

# Section 11 — Fault Injection

`#12`（resync 落盘失败却回 ACK）与 `#13`（NACK 使用户写入返回 EIO）最初是在 `dmsetup remove`
与实时 I/O 竞争时偶然触发（约 8% 概率）而发现的，没有可按需复现的测试。

DRBD 不等真实故障：`drbd9-tests/tests/io-errors` 用 `set_fault_injection` **按需索取**。
本节照此实现（模块参数见手册 §8.2.1），注入点正是唯二会让 Secondary 拒收一笔写的路径。

## F41: 注入的 Secondary 写失败不得传到用户

### 描述
Secondary 落盘失败（NACK）时，该区已被标脏、数据已本地 commit —— 与"没有 peer"
状态相同，**不该**变成文件系统的 I/O 错误。这是修复记录 #13 的正证。

### 实现
- `fault_arm $NODE2 30 $PCACHE_FAULT_DATA_WRITE` → Secondary 30% 的 `P_DATA` 落盘返回 `-EIO`。
- 断言链：先确认**故障确实被注入**（`fault_count > 0`，否则测试什么也没证明），
  再断言不挂死、无用户 I/O 错误、欠债记入脏位图、停注入后 resync 修复。

### 命令
```bash
bash demo_features.sh f41            # 官方跑法（~2min）
# 底层：
ssh root@$NODE2 'echo 1 > /sys/module/dm_pcache/parameters/fault_types
                         echo 30 > /sys/module/dm_pcache/parameters/fault_rate'
ssh root@$NODE2 'cat /sys/module/dm_pcache/parameters/fault_count'
# 预期：写手正常退出、io_errors=0、bm_dirty≠0、停注入 + force-resync 后收敛
```

### 结果
✅ **PASS**（7 项）— 注入 **33456** 次写失败、**用户 I/O 错误 0**、
`bm_dirty=130144`（欠债完整记录）、停注入后 resync 完全修复。

---

## F42: 注入的 resync 写失败不得被 ACK

### 描述
resync 数据落盘失败时，Secondary **不得回 `P_RS_ACK`** —— 源端在 #8 之后是
**读之前就清脏位**的，一旦收到假 ACK，那份数据既没送到也不再被记为欠账。
这是修复记录 #12 的正证。

### 实现
- `fault_arm $NODE2 20 $PCACHE_FAULT_RS_WRITE`，随后 `force-resync`。
- 关键断言用 `dmesg_since` 确认 Secondary 打出了"拒绝 ACK 并断开连接"的证据。

### 命令
```bash
bash demo_features.sh f42            # 官方跑法（~2min）
ssh root@$NODE2 'dmesg | grep "dropping connection rather than ACKing"'
# 预期：出现拒绝 ACK 的日志；停注入后收敛到已知基线模式
```

### 结果
✅ **PASS**（6 项）— 实测日志：

```
injecting secondary resync write failure (count=29)
RS_DATA write_to_cache failed: -5 (sector=1536 size=262144) -- dropping connection rather than ACKing
secondary: RS_DATA recv failed: -5 -- dropping connection
   ... 重连、重试，同一窗口内 29 次
```

> 修复记录 [#18](pcache-dist-fix-history.md#18-secondary-worker-遇到可恢复错误就永久退役) 之前，
> secondary worker 在第一次 RS_DATA 失败后即永久退出，同一 30 秒窗口内只能观察到 1 次注入；
> 修复后为 29 次。
>
> refusal 断言使用的 `dmesg_since` 内部是 `... | grep -q`，在 `set -o pipefail` 下，匹配行较多时
> `grep -q` 提前退出会使管道返回失败，已在测试脚本中修正（见修复记录附录 B 第 7 条）。

---

# Section 12 — Load / Abnormal Flow

移植自 DRBD 的高负载与异常流用例。DRBD 那 145 项里可迁移的约 20–25 项，本节按
"缺口最大"排序逐个补。

## F43: 写负载下的 resync 进展

### 描述
取自 `drbd9-tests/tests/busy-resync`：断开→单侧写大量数据→重连触发 resync→
**重连后持续写**。DRBD 的断言是 *"slowly but surely, the resync should finish in
finite time"*。

**pcache-dist 不承诺这个结论**：[#8](./pcache-dist-fix-history.md) 的多趟扫描
**允许放弃**（"无进展即停"），正是为了防止被写手拖死。所以本用例断言的是**必须给出
诚实的裁决**：

| 结局 | 必须满足 |
|---|---|
| `resync done (dirty=0)` | 积压清空，两端一致 |
| `resync incomplete` | 诚实放弃，且**欠债仍记在脏位图上** |
| 声称成功却仍有欠账 | **绝不允许** —— 这是修复记录中每个静默分歧缺陷的形态 |

### 实现
- `block_inbound` 隔离 peer → 等 55s 让 keepalive 判死 → 写 96 MiB 积压 →
  先确认积压真的进了位图（否则测试什么都没证明）。
- **先起写手再解封**：resync 必须**进入一个已经很忙的设备**，那才是饿死它的条件。
- 收尾：写手停后一次静置 `force-resync` 必须收敛；不一致则自动 `dump_divergence`。

### 命令
```bash
bash demo_features.sh f43            # 官方跑法（~7min；用 nft）
ssh root@$NODE1 'dmesg | grep -E "resync (done|incomplete)"'
# 预期：给出 done 或 incomplete 的裁决；incomplete 时 bm_dirty≠0
```

### 结果
✅ **PASS**（8 项）。该用例发现了修复记录 #15（`b3db50472e0a`）：drain 超时（`-110`）导致 resync
中止并断开连接，重连后被 `decide_sync_role` 误判为脑裂，两端 HALT，Secondary 未收到任何积压数据。

修复后走的是设计中的路径，日志如下：

```
primary: resync incomplete, 131072 bit(s) still owed (writer outran resync); left dirty for the next pass
```

这验证了 #8 的“无进展即停”分支：欠债 `bm_dirty=131072` 如实保留，写手停止后静置 resync 立即收敛。

---

## F44: 非对齐与奇数长度 I/O

### 描述
取自 `drbd9-tests/tests/unusual-bios`。其余所有 demo 都按 4K/64K/128K/1M 的整数倍
写，**从没有一次 bio 只覆盖位图位的一部分** —— 而标脏时 `set_range` 对这种范围是
**向外取整**的。若那个取整曾漏掉边缘位，该次写就不会被复制，且没有任何记录。

### 实现
- 24 次 512 字节写，奇数块数、奇数扇区偏移，保证每次都只覆盖某个位的一部分。
- 分两段：**在线**（直接复制）与**离线**（断连 55s 后再写 → 逼这些范围走完整条
  脏位图路径 → 重连增量 resync）。第二段才是关键。
- 先断言"设备内容确实变了"，避免两端相等的平凡通过。

> **不适用的部分**：DRBD 那条还测 `BLKDISCARD` / `BLKZEROOUT`。实测本设备
> `discard_max_bytes=0`、`write_zeroes_max_bytes=0` —— pcache-dist 不声明这两种支持，
> 块层不会下发，故**未模拟**（测一个设备不支持的操作只会产生虚假信心）。

> **不验证 `clear_range` 向内取整**：`clear_range` 只有两个调用者（resync 循环、
> Secondary 收 RS_DATA），**都传位对齐范围**；非对齐写走的是 `set_range`。
> 那个改动是纯防御性的，没有调用者能触发。

### 命令
```bash
bash demo_features.sh f44            # 官方跑法（~2min；用 nft）
# 底层：奇数块数 + 奇数扇区偏移的 512B 直写
ssh root@$NODE1 'dd if=/dev/urandom of=/dev/mapper/pcache-dist0 \
    bs=512 count=3 seek=140 oflag=direct conv=fsync'
# 预期：内容改变、两端一致；离线写标出脏位；重连后增量 resync 收敛
```

### 结果
✅ **PASS**（6 项）。24 次非对齐写标出 **43 个脏位**（每次跨 1–2 个位，覆盖保守、
无遗漏），增量 resync **5 秒收敛**。

---

## F45: 内存压力下的复制

### 描述
取自 `drbd9-tests/tests/bitmap-oom`，但**换了靶子**。DRBD 那条把位图撑到需要大部分
内存、验证 `attach` 能否优雅失败；pcache-dist 本设备的位图 RAM 副本只有 **16 KiB**
（131072 位 ÷ 8），那个角度没有风险。

真正未验证的是 **fan-out 的 `GFP_NOIO`**（修复记录 #11）：它**阻塞而非跳过 peer**，
因为跳过等于把写从"已复制"静默降级成"欠债"，而链路健康时没有偿还机制。
代价是内存压力下可能阻塞 —— 当初只是推理，从未测过。本用例压这笔交易的两端。

### 实现
- 两节点各占 **60% 可用内存**（无 swap），压力进程设 `oom_score_adj=1000`：
  万一真 OOM，内核先杀它而不是 sshd / fio。
- 照 DRBD 的做法**轮询 RSS 直到内存真正到位**再继续 —— 否则测的是一段从未被占用的
  内存。

| 断言 | 对应交易的哪端 |
|---|---|
| 管理命令仍能完成（`list-peers`，≤60s）| **代价**：`GFP_NOIO` 持读锁等待时，add/remove-peer 需要写锁 |
| 写手不挂死 | 代价 |
| **用户 I/O 错误 = 0** | **买到的**：不静默丢复制 |
| 压力解除后数据一致 | 买到的 |

### 命令
```bash
bash demo_features.sh f45            # 官方跑法（~2min；占 60% RAM）
ssh root@$NODE1 'grep MemAvailable /proc/meminfo'
# 预期：两端各持约 60% 可用内存；管理命令完成；io_errors=0；数据一致
```

### 结果
✅ **PASS**（6 项）。两节点各持 **1972 / 1936 MiB**，管理命令 **0 秒**完成，
写手正常退出，**用户 I/O 错误 0**，压力解除后一致。

> **结论**：阻塞的代价没有出现。mempool 有 1024 个预留元素，而单 peer、iodepth 32 的在飞量只有几十，
> 阻塞路径实际走不到，与 [#11](./pcache-dist-fix-history.md) 的机制分析一致。

---

## F46: soak — 反复 resync × 变速写负载

### 描述
取自 `drbd9-tests/python/endurance.py::run_resync`。照搬其两个设计：

1. **速率扫描** `1M → 4M → 16M → 64M → 256M → 1G` —— 让 resync 时而跑赢写手、
   时而输，从而覆盖多趟扫描的**两种裁决**，而不是只测到本机碰巧产生的那一种。
2. **zipf 热点分布** `--rw=randrw --rwmixwrite=70 --random_distribution=zipf:2.0
   --blocksize_range=4k-64k --numjobs=2` —— DRBD 的注释写明理由：
   *"to trigger bugs which occur with repeated IO to the same sectors"*。
   其余所有 demo 都是均匀写。

运行时长由 `F46_MINUTES` 控制（默认 8）。

### 实现
每周期：起写手 → `force-resync` → 等裁决（done / incomplete）→ 等写手退出 →
检查用户 I/O 错误。收尾静置后必须收敛，不一致则自动 `dump_divergence`。

> **一个刻意不设的断言**：`verdict==done` **不**要求随后 `bm_dirty==0`。
> resync 完成到 `repl_state` 切 ESTABLISHED 之间的写走标脏路径，那些位是**如实
> 记录**的欠债（手册 §9.6.1），与 #10/#12 那种"既没送到也没记录"性质不同。
> 真正能区分两者的断言是**静置后的收敛**。这类情况会记录到日志而不失败。

### 结果
✅ **PASS**（10 项，4 分钟 6 周期）。

**§9.6.1 的残留缺口**：cycle 4 与 cycle 5 在 done 裁决后分别遗留 **28** 和 **109** 个脏位，
即过渡窗口内转为欠债的写入量级。

> **覆盖分工**：本轮 6 个周期**全部是 `done`**，速率扫描未触及饿死区间（每周期
> 写手仅 45 秒、设备 512 MiB，resync 总能赢）。用例会明确提示
> `only one verdict seen — widen the sweep or run longer` 而**不假装两条路径都测过**。
> `incomplete` 由 [F43](#f43-写负载下的-resync-进展) 稳定覆盖（96 MiB 离线积压 +
> 180 秒不限速写手）。两者是分工，不是重叠。

---

# 测试工具总览

`tests/` 下的工具：

| 路径 | 用途 |
|---|---|
| `demo_features.sh`、`dm_pcache_phase1_demo.sh` | 功能测试套件（本文 F01–F51），后者负责建/清集群 |
| `bench/bio_cachedev.sh`、`bench/make_bio_harness.sh` | 在 BIO cache 设备上跑同一套功能测试 |
| `bench/stress_arm.sh` | 回归期间在各节点持续施加 `stress` |
| `standalone/test-minimal.sh`、`standalone/test-nvme-mode.sh` | 单节点 BIO 后端测试 |
| `bench/pmem_prep.sh`、`bench/host_health.sh`、`bench/transport_rr.sh` | 测量前的环境准备与健康检查 |
| `bench/bio_vs_dax.sh`、`bench/repl_ab.sh`、`bench/ab_cut.sh`、`bench/run_verify.sh` | 性能测量 |
| `bench/run_lat_trace.sh`、`bench/parse_lat_trace.py` | 复制写延迟分解（`PCACHE_LAT_TRACE`） |
| `repro/*.sh` | 复现与诊断脚本 |

## BIO 后端回归

```bash
bash bench/bio_cachedev.sh up          # 每个节点：2.5 GB 稀疏镜像 -> loop -> /dev/pcache-cdev，并装开机服务
bash bench/make_bio_harness.sh         # 生成 bench/bio-harness/：把 /dev/pmem0 换成 /dev/pcache-cdev 的套件副本
bash bench/bio-harness/demo_features.sh all
bash bench/bio_cachedev.sh down        # 结束后移除 loop、镜像与开机服务
```

- 布局相同，demo 里用 `dd` 读写固定偏移的部分照样适用。
- `pcache-cdev.service` 在开机时重建 loop 与链接，所以 F35 等会让节点崩溃/重启的 demo 照样能跑。
- 修改原套件后要重新执行 `make_bio_harness.sh`；`bench/bio-harness/` 是生成物。

## 压力回归

```bash
export STOP_FLAG=/tmp/stress.stop; rm -f $STOP_FLAG
bash bench/stress_arm.sh &             # 每 30 s 在三个节点重新武装 stress --cpu N/2 --vm 2 --vm-bytes 256M --io 2
bash demo_features.sh all
touch $STOP_FLAG; wait                 # 停止并清除 stress
```

F46 的 soak 有固定时长，压力下完成的 cycle 较少；每个 cycle 计一次 PASS（输出导到 `/dev/null`），
因此总 PASS 数可能比无压力时少 1–2，这不代表有检查漏跑。

## 单节点 BIO 测试

```bash
KO=/root/dm-pcache.ko bash standalone/test-minimal.sh                       # 建立、写、读、移除，各步骤有超时
KO=/root/dm-pcache.ko CACHE_SIZE_MB=2048 bash standalone/test-nvme-mode.sh  # mkfs、remount、replay、fio、移除
```

两者都在 loop 文件上建立 BIO cache（`data_crc true`），在任一节点上执行。cache 至少 2 GiB（1 GiB bitmap 区）。

## 性能测量

先准备并检查环境，否则数字不可信：

```bash
bash bench/pmem_prep.sh                # 确保 pmem0 为 fsdax（VM 关机时会启动 VM）
bash bench/host_health.sh              # iperf3 带宽 + 128K ping-pong 往返速率，给出可信与否的判定
```

| 脚本 | 测什么 |
|---|---|
| `bench/bio_vs_dax.sh [rounds]` | DAX/BIO × 单机/复制，w1–w4（4K 随机写 qd1/qd8、128K 顺序写 qd8、4K 随机读 qd32），同一会话内交错，等初始全量同步结束才开始测量 |
| `bench/repl_ab.sh [rounds]` | 复制 128K 顺序写：模块 × 后端 × streams/qd 组合，交错；同时每秒采样两端 debugfs，报告 cache 满的时间比例。配置名 `<pre\|int>-<dax\|vdb\|bioNNNN>-s<streams>q<qd>[r<MiB/s>]` |
| `bench/ab_cut.sh <ko_a> <ko_b> [rounds]` | 两个模块的交错 A/B（streams=2 qd=32 128K） |
| `bench/run_lat_trace.sh` + `parse_lat_trace.py` | 每笔复制写的分段延迟（`PCACHE_LAT_TRACE` 构建） |

注意事项：

- **同一会话内交错比较**；虚拟机之间的往返速率会随时间漂移，跨会话的数字不可比。
- **BIO cache 不要用 brd**（`/dev/ram0`）：它占用虚拟机自身内存，与 null_blk 同时使用会导致 secondary
  内存耗尽。应使用宿主机内存文件支撑、热插入的 virtio 磁盘：

  ```bash
  # on the host, one image per node
  truncate -s 2048M <tmpfs-dir>/biocache-<node>.img
  virsh attach-disk <domain> <tmpfs-dir>/biocache-<node>.img vdb \
          --targetbus virtio --driver qemu --subdriver raw --live
  ```

  然后配置名用 `vdb`。`--live` 不持久，虚拟机重启后消失。
- 复制配置要等 `bm_dirty=0`（初始全量同步结束）才测，脚本已处理。
- `fio --direct=1` 不经 page cache，stable writes 对这些数字没有影响。

## 复现与诊断脚本

| 脚本 | 用途 |
|---|---|
| `repro/wb_halt_eio.sh` | 改坏 cache 上的数据，验证 writeback 停止后写入得到 `-EIO` 而非挂起、设备可移除（单节点 BIO） |
| `repro/bio_wb_stall.sh` | 单节点 BIO 长时间随机写，cache 满且 `dirty_tail` 不动超过阈值即报 stall |
| `repro/trace_seg0_writes.sh` | 用 `block_bio_queue` 追踪写到 key log 起始 segment 的所有 bio（含来源进程） |
| `repro/intree_netpoll_repro.sh`、`repro/intree_ab.sh` | 只用内核自带组件复现 virtio_net 零 budget poll 的页重复释放（修正提交见报告），与有无修正的对照 |
| `repro/f39_silent_loss.sh` | #14 的专项检测：resync 与应用写入重叠后的静默分歧 |
| `repro/wedge_half_close.sh` | 单向断网后 Secondary 是否永久拒绝重连的确定性复现 |
| `repro/undrained_debt.sh` | resync 欠账未排空导致静默分歧的确定性复现 |
| `repro/zc_hang.sh <ko> <bs>` | 验证 `MSG_ZEROCOPY` 完成回调，约两分钟给结论 |

---

# 附录 A — 关键发现 (Findings)

以下两项是测试逻辑或预期的错误，不是产品缺陷；断言已据此修正。

## Finding 1 — `remove-peer` 后重加入需 `force-resync`（F05）

- **现象**：`remove-peer 2` → 写入 → `add-peer 2` 后，node-3 的增量 resync 传输 0 run，60 秒内不一致。
- **根因**：`remove-peer` 销毁该 peer 的脏位图；重加入的 peer 位图全新（干净），没有脏区可传。
  瞬时断连（F11）时 peer 仍在册、位图被标脏，才走增量 resync。
- **现状**：修复记录 #9（`b69033fc9b66`）之后，经 `registry_add` 取得槽位的 peer 一律以
  `FULL_SYNC_NEEDED` 起步（不载入 PMEM 上的旧位图），重加入会自动全量同步，不再需要人工 `force-resync`。
  F05 仍执行 `force-resync`，验证该命令的全量路径。

## Finding 2 — split-brain HALT 日志经 force-promote 不可达（F24）

- **现象**：双主分歧后重连，dmesg 从不出现 `split-brain detected`（轮询 75s 无）。
- **根因**：`set-role primary force` 置 `FULL_SYNC_NEEDED` → `decide_sync_role` 走 full-sync 分支（返回 SYNC_SOURCE）而非 generation-冲突 HALT 分支（`dm_pcache_state.c`）。双主陷入 `SYNC_SOURCE→OFF` livelock。
- **安全保证仍成立**：分歧数据**不被静默覆盖**（node-1≠node-2）**且复制停摆**（非 ESTABLISHED），等人工 `resolve-split-brain`（F25 可收敛）。这才是容错要点。
- **备注（F34）**：registry 不会在设备重建后自动恢复 peer 连接，它只持久化 slot↔bitmap 归属（[F15](#f15-持久化-peer-注册表-32-槽)/[F17](#f17-幂等-add-peer)），设备重启后仍需 `add-peer`。已改 F34 断言为"设备加载 + 数据完好"。

# 附录 B — 崩溃/容错专项结论

面向“从异常环境恢复”的覆盖：

| 场景 | 测试 | 结论 |
|---|---|---|
| 单拷贝元数据损坏 | F34 | 设备经存活拷贝加载，数据零丢失 ✅ |
| Primary 硬崩溃（sysrq+c）| F16, F35 | 节点恢复到工作态；fsync 数据幸存 ✅ |
| Primary 崩溃 → 服务连续性 | F35b | Secondary 升主后立即服务写 ✅ |
| 崩溃节点重加入 | F35c | 以 Secondary 重加入 + force-resync 收敛 ✅ |
| 网络分区 / 死链 | F04, F09, F11 | ACK 超时/keepalive 检测 + 断线位图跟踪 + 增量 resync ✅ |
| 脑裂（双主分歧）| F24, F25 | 不静默覆盖、复制停摆、人工裁决收敛 ✅ |
| Quorum 丢失 | F23 | 失去多数即拒写（write fencing）✅ |
| 快速抖动/churn | F32 | 10 轮拆建无 soft-lockup/oops ✅ |

两次 sysrq+c 崩溃后，3 节点均恢复健康（ssh + fsdax + 设备可建）。

# 附录 C — Section 10 找到的产品缺陷

Section 10 起的并发用例暴露了以下缺陷（详见
[`pcache-dist-fix-history.md`](./pcache-dist-fix-history.md)）：

| 发现者 | 缺陷 | 修复记录 | 状态 |
|---|---|---|---|
| churn 场景（F36 前身）| 协商窗口入队的 `pending_req` 永不完成 → bio 悬挂 | #5 | 已修 `6b9406f862a4` |
| 同上 | flush 完成码不一致（运维动作变成 EIO）| #6 | 已修 `6b9406f862a4` |
| F36 | remove/add-peer 造出假脑裂；HALT 死循环扣住连接 | #9 | 已修 `b69033fc9b66` |
| F40 | 多流稳态下 `cache_kset_close` 的 `-EBUSY` 泄漏成 NACK → 静默欠债 | #10 | 已修 `281a75149622` |
| F39 追查途中 | resync 落盘失败却回 ACK | #12 | 已修 `d50fcd955a5e` |
| F37 | NACK 把用户的写打成 EIO | #13 | 已修 `0d6e720d70e9` |
| F39 | resync 读 backing 看不见还在 open kset 里的写 | #14 | 已修 `b4f4d9672490` |
| F48 | incomplete 之后欠债被赦免 → 角色反转 → 假脑裂 HALT | #15 | 已修 `eb15c0a9bc22` |
| F49 | 半开连接把 peer 楔死，且它自认已同步 | #16 | 已修 `1f59b35fdafb` |

这些缺陷都需要“控制面动作与 I/O 并发”才能暴露，F01–F35 的顺序 `dd` 在结构上无法触及。

# 附录 D — DRBD 测试套件覆盖对照

对照对象：DRBD 9 测试套件 `drbd9-tests/tests`，**145 个用例**。
分类依据是每个用例头部的说明与 `vms_all` 节点数，不是文件名猜测。

| 类别 | 数量 |
|---|---|
| 已移植 | 14 个来源 → F36–F51 |
| **直接适用但未覆盖** | **约 22**（见 D.1 / D.2）|
| **invariant 可改写后适用** | 约 15–20（见 D.3）|
| 不适用（无对应概念）| 其余约 90 |

## D.0 为什么"不适用"占这么大比例

按特征统计（类别有重叠，不相加）：

| 特征 | 用例数 | 占比 |
|---|---|---|
| **用到 diskless 节点** | **50** | **35%** |
| fencing / suspend-io / autopromote | 24 | 17% |
| 直接操纵 UUID（`set_gi` / `new-current-uuid` / day0）| 18 | 12% |
| 两阶段提交 | 15 | 10% |
| 需要 4 个节点 | 9 | 6% |
| resize / rename | 8 | |
| 多传输 / 多路径 / netns | 7 | |
| 多卷 | 6 | |

三个原因，按分量：

1. **一个特性吃掉 35%。** diskless 节点（参与复制但本地无存储）在 DRBD 里是基础概念 ——
   diskless Primary、quorum tiebreaker、LINSTOR 的 diskless attach —— 于是每个
   resync / quorum / promote 场景都长出一个 diskless 变体。pcache-dist 每个节点都必须有
   PMEM cache + backing。
2. **相当一部分是 DRBD 历史缺陷的回归测试**，针对 pcache-dist 没有的机制（`day0-uuid-*`、
   `twopc-timeout`、`storage-migration-uuid`）。
3. **产品面差一截**：多卷、resize/rename、三种传输、多路径、netns、fencing handler +
   STONITH、auto-promote、activity log、Protocol A/B/C、thin/discard、快照、
   online verify、suspend-io。pcache-dist 是其中一个子集。

## D.1 高并发 / 高负载场景 —— 未覆盖的，按价值排序

这是缺口最集中、也最容易再挖出缺陷的一块。带 ⚠️ 的表示**已知 pcache-dist 当前行为不满足**。

| DRBD 用例 | 节点 | 它压的是什么 | 为什么值得移 |
|---|---|---|---|
| ⚠️ `c-min-rate` | 2 | resync 被应用 I/O 压慢但**不许压死**，断言总时长落在区间内 | pcache-dist 目前是全有全无：饱和写手下 sweep 一个 chunk 都发不出去（见 #14 记录）。这是 DRBD 对同一问题的正面答案 |
| `resync-interrupt-conflict` | 3 | resync 请求与应用 I/O **冲突**，且同时被断连打断 | 正是 #14（读写重叠）与 #16（断连）交叉的那块 |
| `resync-interrupt-variants` | 3 | 同一件事的多种打断变体，含繁忙写手变体 | F49 只覆盖其中一种；这条是成熟版本 |
| `resync-finish2` | 3 | **一个源同时向两个 target resync，快的先完成** —— 断言快的那个不会把慢的标成 Outdated | 与 pcache-dist 中 Primary→2 peers 的 per-peer 记账完全同构；#15 那条链就是"一个 peer 的完成污染了另一个的账" |
| `resync-target-promote-interrupt` | 3 | resync 跑到 20% 时**发生 promote** | F38 测的是写负载下切角色，没测过"resync 在飞时切角色" |
| `switch-sync-source` | 3 | resync 中途**换源** | 3 节点下的源切换，尚未覆盖 |
| `concurrent-promote-demote` | 3 | 两个节点**同时**做角色变更 | F38 是单点顺序切换，并发竞态没覆盖 |
| `process-wakeup` | 2 | "processes writing to DRBD can lock up forever" —— 丢唤醒导致永久挂死 | 与修复记录 #5（bio 悬挂）同类；F36/F37 只断言"没挂"，没有主动构造丢唤醒 |
| `multi-bio-peer-reqs` | 2 | 一个 peer request 跨多个 BIO | F44 覆盖了奇怪尺寸，没覆盖跨 BIO 的大请求 |
| `quorum-isolate-primary` | 3 | 隔离 Primary，写负载下观察 quorum 反应 | F22/F23 只测了 quorum 配置解析与静态拒写 |
| `quorum-failover-reconnect` | 3 | 网络故障导致 failover，再重连回幸存节点 | 同上；failover 路径只有 F35（崩溃型）覆盖 |

## D.2 其他适用但未覆盖的

| DRBD 用例 | 对应的 pcache-dist 代码 |
|---|---|
| `resync-based-on-disk-states` | `pcache_decide_sync_role` 的方向判定（#15 涉及此处）|
| `initial-uuid-uptodate` | "两端互相判成 SYNC_SOURCE"（#14 旧候选里记过这个症状）|
| `resync-double-target` | 3 节点 per-peer 位图的交叉影响 |
| `resync-write-during-pause` | peer 处于非 ESTABLISHED 时的写路径 |
| `resync-var-bm-bsize` | `decide_sync_role` 的 geometry 检查（`bm_nr_bits`/`bm_block_shift` 不匹配 → HALT），**写了但从没测过** |
| `two-primaries-reject` | 角色围栏（F18–F21 测了单点，没测"两个 Primary 相遇"）|
| `peer-acks` | 3 节点"谁拿到了什么"的确认 |
| `force-resync` / `invalidate` | F28 只测了消息能执行，没测数据语义 |
| `interrupt-detach` / `disk-timeout` | backing 设备停止完成 I/O —— 尚未注入过这种故障 |
| `connect-abort` / `ref-count` / `add-connect-delete` | 连接生命周期与引用计数（修复记录 #7 那类）|
| `verify` / `verify-target-becomes-inconsistent` | **功能缺口**：pcache-dist 没有 online verify |

## D.3 invariant 可改写后适用（次优先）

这些用例的**机制** pcache-dist 没有，但它们**要证明的性质**同样适用。移植时要改写成
pcache-dist 的形状，而不是照搬步骤。

| DRBD 用例 | pcache-dist 没有的机制 | 适用的 invariant |
|---|---|---|
| `suspend-io`、`suspend-io-bdev-freeze`、`quorum-suspended-cancel`、`quorum-suspend-failover-busy`、`quorum-suspend-timeout` | suspend-io | **失去 quorum 时在飞的写怎么办** —— F22/F23 只测了配置解析与静态拒写 |
| `resync-to-older`、`resync-from-weak`、`resync-chain-close`、`resync-downstream-upstream`、`resync-upstream-downstream`、`resync-reconciliation` | target→target 链式同步 | **多代数据下的方向判定** —— 直接映射 `pcache_decide_sync_role`（#15 涉及此处） |
| `attach-concurrent`、`add-disk-concurrent` | attach / 多卷 | 两节点**并发控制面操作**的竞态 |
| `io-errors-diskless` | diskless | 本地没出错的节点上，**应用不该受影响** |
| `protocol-a-io` | Protocol A | 繁忙 I/O 下的崩溃回归（形状通用）|
| `stress-al` | activity log | **元数据热路径在压力下**的行为 —— 对 pcache-dist 是 kset / PMEM 路径 |
| `lost-primary-after-no-writes` | 4 节点 | Primary 消失但期间没有写，可在 3 节点重述 |

## D.4 不适用，及其原因

| 原因 | 大致数量 | 例子 |
|---|---|---|
| diskless 节点模型 | ~24 | `diskless-*`、`io-errors-diskless`、`may-promote`、`unstable-retry`、`quorum-tiebreaker*` |
| UUID 链 / day0 / 存储迁移（pcache-dist 使用单调 generation 计数器，不是 UUID 链）| ~14 | `day0-uuid-*`、`uuid-*`、`recreate-node-day0*`、`pre-existing-data` |
| 多卷 / resize / rename / 命名规则 | ~12 | `add-disk*`、`resize*`、`rename`、`invalid-names` |
| 两阶段提交（pcache-dist 没有 2PC）| 5 | `twopc-*`、`stress-connect-and-2pc` |
| 多传输 / 多路径 / netns | 7 | `lb-tcp*`、`multi-path`、`net-namespaces*` |
| discard / thin（`num_discard_bios` 未设，不支持 discard）| 3 | `discard-granularity`、`resync-discard`、`resync-merge-discards` |
| attach/detach、outdate、fencing handler、autopromote、suspend-io、activity log、Protocol A、快照、版本升级 | 其余 | `attach-*`、`fencing`、`autopromote`、`suspend-io*`、`stress-al`、`protocol-a-io`、`*snapshot*`、`upgrade` |

> 「不适用」不等于「永远不需要」。其中 **online verify** 与 **suspend-io** 是真实的
> 功能缺口 —— 前者是生产上发现静默分歧的手段（#14 那种问题在现场靠它发现，而不是靠测试 rig 的 md5），
> 后者是失去 quorum 时保护数据的手段。要不要做是产品决定，不是测试决定。


# 验收结果

| 项 | 结果 |
|---|---|
| DAX `all`（无压力） | 180 PASS / 0 FAIL |
| BIO `all`（无压力，`bench/bio-harness/`） | 180 PASS / 0 FAIL；各节点无暂存元数据写失败、无 writeback 停止 |
| DAX `all` + `stress_arm.sh` | 180 PASS / 0 FAIL |
| BIO `all` + `stress_arm.sh` | 179 PASS / 0 FAIL（F46 soak 少一个周期） |
| 单节点 BIO | `test-minimal.sh`、`test-nvme-mode.sh` 全部通过 |
| writeback 停止 → `-EIO` | `repro/wb_halt_eio.sh` 通过 |

每个 demo 通过 `assert_module_fresh` 校验节点加载的模块与 `/root/dm-pcache.ko` 一致。
`dm_pcache_phase1_demo.sh` 的 `rmmod || true` + `insmod || true` 在设备未干净移除时会沿用旧模块，
该校验避免了测到旧模块的情况。

性能数据、整合中发现的问题与已知限制见 [`pcache-dist-test-report.md`](./pcache-dist-test-report.md)。
