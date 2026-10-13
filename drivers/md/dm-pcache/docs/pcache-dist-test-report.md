# pcache-dist 测试报告

本报告汇总本分支的组成、测试环境、功能回归结果、性能数据、整合中发现并修正的问题，以及已知限制。

相关文档：

- 测试方法与每项测试的定义：[`dm-pcache-test-guide.md`](./dm-pcache-test-guide.md)
- 设计与运维：[`pcache-dist-manual.md`](./pcache-dist-manual.md)
- BIO 后端整合说明：[`bio-backend-integration.md`](./bio-backend-integration.md)
- 复制功能的缺陷修复记录：[`pcache-dist-fix-history.md`](./pcache-dist-fix-history.md)

## 目录

- [1. 结论](#1-结论)
- [2. 分支组成](#2-分支组成)
- [3. 测试环境](#3-测试环境)
- [4. 功能回归](#4-功能回归)
- [5. 性能](#5-性能)
- [6. 整合中发现并修正的问题](#6-整合中发现并修正的问题)
- [7. 已知限制与后续工作](#7-已知限制与后续工作)
- [8. 代码规模](#8-代码规模)
- [9. 文件位置](#9-文件位置)

## 1. 结论

| 项 | 结果 |
|---|---|
| 功能回归（DAX 后端） | 51 项、180 个检查：180 PASS / 0 FAIL；施加背景压力时同为 180 / 0 |
| 功能回归（BIO 后端） | 180 PASS / 0 FAIL；施加背景压力时 179 PASS / 0 FAIL（F46 soak 在固定时限内少完成一个周期，非失败） |
| 单节点 BIO 测试 | `test-minimal.sh`、`test-nvme-mode.sh` 全部通过 |
| writeback 停止后的行为 | `wb_halt_eio.sh` 通过：需要 cache 空间的请求返回 `-EIO`，不再无限等待 |
| DAX 复制性能 | 与整合前相比：streams=2 qd=32 约低 3%，streams=1 qd=8 无差别；p99.9 明显改善 |
| BIO 复制性能 | 128K 顺序写 qd=8 约 1,100 MiB/s，受 BIO 后端串行 writeback 限制（§5.3） |

BIO 后端补丁系列合并时，git 未报告冲突但语义已被破坏的有 3 处；测试与代码审查中另发现 6 处缺陷。全部已修正（§6）。

## 2. 分支组成

```
9b87fdc9af2f              mainline 基点
  └─ 9f809dce39aa         pcache-dist 复制功能（67 个 commit，顶端为 virtio_net 修正）
       └─ 本分支          + BIO 后端补丁系列 33 个 + 整合修正 12 个 + 测试工具与文档 2 个
```

`9f809dce39aa`（virtio_net: don't refill the RX ring from a zero-budget poll）修正了 virtio_net 在零 budget poll
中补充接收环，导致 page_pool 页被释放两次的问题（[`pcache-dist-fix-history.md`](./pcache-dist-fix-history.md) #23）。
该问题不在 dm-pcache 中，但在复制负载下可使 VM 崩溃，需要单独提交到 netdev。

整合修正 commit：

| commit | 内容 | 类型 |
|---|---|---|
| `675ec66806dc` | secondary 接收路径计算 `data_crc` | 合并问题修正 |
| `7e1f8d246851` | 复制暂时限制为 DAX 后端（由 `de1ff4de6b5d` 取代） | 过渡 |
| `978d46e486eb` | 提交者上下文的请求经 `pcache_submit_req` 处理 | 合并问题修正 |
| `e3c9e8a64833` | 大块 kernel buffer 的 cache-device I/O 分块提交 | BIO 复制前置 |
| `21a15ca3b72b` | peer registry 与 bitmap 经 `cmedium_*` 访问 | BIO 复制前置 |
| `de1ff4de6b5d` | 在 BIO cache 设备上复制 | 功能 |
| `fa9b63bdab59` | `cache_verify_dirty_tail()` 经 `cmedium_*` 读取 | 缺陷修正 |
| `9294f700f7dc` | 复制或校验 `data_crc` 时要求 `BLK_FEAT_STABLE_WRITES` | 缺陷修正 |
| `b8cf5fe1318b` | `writeback_all` 之前先 drain 暂存元数据 | 缺陷修正 |
| `539822627f9b` | 未对齐 buffer 使用 bounce，并报告暂存写错误 | 缺陷修正 |
| `857b49b95ff5` | writeback 错误路径不再重复获取 `writeback_lock` | 缺陷修正 |
| `4ff9d6f1f4b4` | writeback 停止后，需要 cache 空间的请求返回 `-EIO` | 缺陷修正 |

## 3. 测试环境

| 项 | 配置 |
|---|---|
| 节点 | 3 台 KVM 虚拟机（node-1/2/3），内核由本分支编译 |
| DAX cache 设备 | 模拟 nvdimm（`/dev/pmem0`，fsdax 模式），由宿主机内存文件支撑 |
| BIO cache 设备（功能测试） | 稀疏映像挂载的 loop 设备 |
| BIO cache 设备（性能测试） | 由宿主机内存文件支撑的 virtio 磁盘 |
| backing 设备 | 功能测试为 loop 设备；性能测试为 `null_blk`（memory_backed） |
| 复制网络 | 同一宿主机上虚拟机之间的 virtio-net |

两种 cache 设备都是内存速度，性能数字反映的是代码路径的开销，不是介质。真实 NVMe 上 BIO 后端的数字会更低。

## 4. 功能回归

### 4.1 测试集

`tests/demo_features.sh all` 包含 F01–F51 共 51 项、180 个检查，覆盖：同步复制与 fan-out、peer 连接与断线、
ACK 超时与重连、脏位图与增量/全量 resync、quorum 与脑裂防护、角色切换与 promote、崩溃恢复、重叠写排序、
长时间 soak 等。每项的定义与预期见测试指南。

- **BIO 版本**：`tests/bench/make_bio_harness.sh` 生成以 BIO cache 设备替换 `/dev/pmem0` 的副本，其余不变。
- **压力版本**：`tests/bench/stress_arm.sh` 在回归期间于各节点持续运行 `stress`（一半 CPU 核、2 × 256 MiB 内存、
  sync I/O），节点重启后自动重新施加。

### 4.2 结果

| 测试 | PASS | FAIL | 备注 |
|---|---|---|---|
| DAX `all` | 180 | 0 | |
| BIO `all` | 180 | 0 | 各节点无暂存元数据写失败、无 writeback 停止 |
| DAX `all` + 压力 | 180 | 0 | |
| BIO `all` + 压力 | 179 | 0 | F46 soak 在固定时限内少完成一个周期，检查项少一个 |
| `tests/standalone/test-minimal.sh`（BIO） | 全部 | 0 | cache 2 GiB（§7） |
| `tests/standalone/test-nvme-mode.sh`（BIO） | 全部 | 0 | `CACHE_SIZE_MB=2048` |
| `tests/repro/wb_halt_eio.sh` | 通过 | | 改坏 cache 数据使 writeback 停止，写满后请求得到 `-EIO`，设备可移除 |

## 5. 性能

backing 为 `null_blk`。fio：libaio、`direct=1`、`--ramp_time=3 --runtime=12 --time_based`。
A/B 对比在同一测试会话内交错进行 3 轮。

### 5.1 DAX 后端：单机与复制

`tests/bench/bio_vs_dax.sh`，3 轮中位数：

| 工作负载 | 单机 | 复制 |
|---|---|---|
| 4K 随机写 qd=1 | 215,546 IOPS | 18,753 IOPS（p99 128 µs） |
| 4K 随机写 qd=8 | 207,977 IOPS | 63,794 IOPS |
| 128K 顺序写 qd=8 | 5,609 MiB/s | 1,660 MiB/s（p99 3.06 ms） |
| 4K 随机读 qd=32（命中） | 580,581 IOPS | 574,656 IOPS |

读命中只走本地，复制与否不影响。复制写入的上限是虚拟机之间的请求-应答往返速率，不是 cache 介质的带宽。

### 5.2 整合前后对比（DAX 复制，128K 顺序写）

`tests/bench/repl_ab.sh`，整合前（`9f809dce39aa`）与整合后模块交错 3 轮：

| 形态 | 整合前 MiB/s | 整合后 MiB/s | 整合前 p99.9 | 整合后 p99.9 |
|---|---|---|---|---|
| streams=2 qd=32 | 2890 / 2866 / 2875 | 2789 / 2806 / 2758 | ~104 ms | 5.5–6.8 ms |
| streams=1 qd=8 | 1702 / 1733 / 1675 | 1689 / 1661 / 1767 | ~4.1 ms | ~4.1 ms |

整合后 streams=2 qd=32 约低 3%，streams=1 qd=8 无差别，p99.9 的尖峰消失。§5.1 的 1,660 MiB/s 对应 streams=1 qd=8。

### 5.3 BIO 后端

`tests/bench/repl_ab.sh`，DAX 与 BIO（virtio 磁盘）交错 3 轮，128K 顺序写：

| 配置 | MiB/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| DAX 复制 s1q8 | 1713 / 1748 / 1632 | ~350 µs | ~3.0 ms | ~4.1 ms |
| DAX 复制 s1q8，限速 1,100 MiB/s | 1100 / 1100 / 1100 | ~200 µs | ~2.9 ms | ~4.0 ms |
| BIO 复制 s1q8 | 1122 / 1058 / 1109 | ~870 µs | ~1.25 ms | 1.6–2.2 ms |
| BIO 复制 s2q8 | 1123 / 1128 / 1135 | ~460 µs | ~1.03 ms | ~106 ms |

同一设备上 BIO 单机约 1,370 MiB/s，设备本身裸写约 10 GB/s。

分析：

- BIO 单机的瓶颈是 writeback：`cache_key_writeback_bio()` 对每个 key 先同步 `cmedium_read()` 读回，再写 backing，
  单线程串行。DAX 的 writeback 直接以 pmem 上的数据构造 backing bio，不需要读回。
- BIO 复制的 p99 低于 DAX，原因是本地路径限制了进入复制路径的速率，发送队列几乎不需要等待
  （最慢 1% 请求的排队时间 71 µs，DAX 为 905 µs），不是复制路径本身更快。
- DAX 限速到同样的 1,100 MiB/s，p99 仍约 3 ms：fio 限速时仍成批发出请求，尾延迟由在途深度决定。
- BIO s2q8 的 p99.9 约 106 ms 与 cache 写满的采样同时出现，是 writeback 跟不上造成的等待。

### 5.4 DAX 复制尾延迟分解

DAX 复制 128K、streams=1，qd=1/2/4/8 的 p99 分别为约 0.97 / 1.2 / 2.4 / 3.0 ms。

不经 dm-pcache 的同形态网络往返（128K 请求 + 32 B 应答，qd=1，`pingpong_srv.py`/`pingpong_cli.py`）：
p50 77 µs、p99 136 µs、p99.9 199 µs。DAX 复制 qd=1：p50 84 µs、p99 971 µs、p99.9 2,343 µs。

`PCACHE_LAT_TRACE` 分解 qd=1 最慢 1% 的请求（`run_lat_trace.sh`、`parse_lat_trace.py`）：

| 阶段 | 时间 |
|---|---|
| primary：发送到收到 ACK | 1,301 µs |
| 同一批请求在 secondary 上的处理 | 85 µs |
| primary：空队列时的排队时间 | 139 µs |

多出的约 1.1 ms 位于 primary 的发送与 ACK 接收一侧（线程间交接），不在网络，也不在 secondary。尚未进一步定位（§7）。

### 5.5 测量方法要点

- A/B 必须在同一会话内交错进行；虚拟机之间的往返速率会随时间漂移，跨会话的数字不可比。
- 测量前等待初始全量同步结束（`bm_dirty=0`），`bio_vs_dax.sh` 与 `repl_ab.sh` 已自动等待。
- 不要用 brd 作 BIO cache 设备：brd 占用虚拟机自身内存，与 `null_blk` 同时使用时会导致内存耗尽。
  应使用由宿主机内存文件支撑、热插入虚拟机的 virtio 磁盘（`virsh attach-disk --live`）。
- 测量前运行 `tests/bench/host_health.sh` 检查宿主机的网络带宽与往返速率。

## 6. 整合中发现并修正的问题

| # | 问题 | 后果 | 修正 |
|---|---|---|---|
| 1 | 提交者上下文处理请求时绕过 `pcache_submit_req` | 大多数写入不经复制即完成 | `978d46e486eb` |
| 2 | secondary 接收路径未计算 `data_crc` | 持久化的 `data_crc` 为 0，重新 attach 时校验失败 | `675ec66806dc` |
| 3 | `cache_dev_zero_range()`、`pcache_meta_find_latest()` 接口变更 | 复制代码无法编译 | 并入补丁 02/03 |
| 4 | `cache_verify_dirty_tail()` 仍使用 DAX 指针 | BIO cache 重新 attach 时 page fault | `fa9b63bdab59` |
| 5 | BIO 上 `data_crc` 按可变页面计算 | 校验失败，writeback 停滞，cache 满后设备停滞 | `9294f700f7dc` |
| 6 | `writeback_all` 在暂存元数据落盘前判断日志结尾 | resync 读到旧数据并清除 dirty bit，副本不一致 | `b8cf5fe1318b` |
| 7 | 暂存元数据 buffer 不满足 `bdev_dma_alignment`，`-EINVAL` 被忽略 | kset 丢失，writeback 停滞 | `539822627f9b` |
| 8 | writeback 错误路径重复获取 `writeback_lock`（代码审查发现） | 错误路径第一次触发即自锁 | `857b49b95ff5` |
| 9 | writeback 停止后，需要空间的请求无限等待 | I/O 挂起，设备无法移除 | `4ff9d6f1f4b4` |

各项的分析与验证见 [`bio-backend-integration.md`](./bio-backend-integration.md) §3、§6。
复制功能本身的缺陷（#1–#23）见 [`pcache-dist-fix-history.md`](./pcache-dist-fix-history.md)。

## 7. 已知限制与后续工作

| 项 | 说明 | 建议 |
|---|---|---|
| BIO writeback 串行同步读回 | BIO 单机带宽约 1,370 MiB/s，远低于设备能力 | 读回改为异步并发，或在 cache 设备与 backing 之间直接以 bio 搬运 |
| BIO secondary 同步写入 | secondary 对每个分段同步写入后才回 ACK，并发越高越明显 | 参照 DRBD：异步提交 peer request，完成时回 ACK |
| DAX 复制尾延迟 | qd=1 时最慢 1% 多出约 1.1 ms，位于 primary 发送 / ACK 接收侧 | 在发送完成与收到 ACK 处增加时间戳继续定位 |
| `PCACHE_LAT_TRACE` 默认开启 | `dm_pcache.h`、`dm_pcache_net.h` 中默认值为 1，每笔复制写两端各一次 `trace_printk()` | 默认关闭 |
| cache 设备最小容量 | 布局在 segment 前固定预留 1 GiB bitmap 区，小于约 1.3 GiB 的 cache 放不下 segment；`PCACHE_CACHE_DEV_SIZE_MIN` 仍为 512 MB | 修正最小容量检查，或按 backing 大小计算 bitmap 区 |
| 超级块格式变化 | `struct pcache_sb` 新增 `block_size`、`cache_type`，旧版本格式化的 cache 校验失败 | 升级时重新格式化 cache 设备 |
| bisect 空窗 | 部分中间 commit 存在已知问题（见整合说明 §7） | 只影响 bisect |
| commit 签署 | 复制功能最初的 19 个 commit 带有 `Signed-off-by`，其余 commit（包括 BIO 后端补丁系列与整合修正）缺少；BIO 后端补丁系列的作者信息需原作者确认 | 提交上游前由各 commit 的作者补充 `Signed-off-by` 与正式邮箱（见 `Documentation/process/submitting-patches.rst`） |
| virtio_net 修正 | `9f809dce39aa` 尚未提交上游 | 提交到 netdev |

## 8. 代码规模

`drivers/md/dm-pcache/` 下 `.c`/`.h` 的代码行数（不含空行与注释）：

| 部分 | 行数 |
|---|---|
| mainline dm-pcache（`9b87fdc9af2f`） | 3,820 |
| 复制功能 | +4,704（其中新文件 4,138） |
| BIO 后端补丁系列 | 新增 1,354、修改 124、删除 67，净增约 1,287 |
| 整合修正 | +353 / −109 |
| 合计 | 9,914 |

## 9. 文件位置

| 路径 | 内容 |
|---|---|
| `docs/` | 文档，索引见 [`README.md`](./README.md) |
| `tests/demo_features.sh` | 多节点功能回归（F01–F51） |
| `tests/bench/` | 性能测量、BIO 回归、背景压力、环境检查 |
| `tests/repro/` | 缺陷复现与诊断脚本 |
| `tests/standalone/` | 单节点 BIO 测试 |
