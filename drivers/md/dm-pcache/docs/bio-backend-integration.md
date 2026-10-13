# BIO cache 后端整合说明

本文说明 BIO（非 DAX）cache 后端补丁系列并入 pcache-dist 的过程：合并冲突的处理、合并后才暴露的
语义问题、复制功能移植到 BIO 后端的设计，以及测试中发现并修正的缺陷。测试结果与性能数据见
[`pcache-dist-test-report.md`](./pcache-dist-test-report.md)，后端的运行时行为见
[`pcache-dist-manual.md`](./pcache-dist-manual.md) §11。

## 目录

- [1. 来源与基准](#1-来源与基准)
- [2. 合并冲突](#2-合并冲突)
- [3. 合并后才暴露的语义问题](#3-合并后才暴露的语义问题)
- [4. 设计决定](#4-设计决定)
- [5. 复制功能移植到 BIO 后端](#5-复制功能移植到-bio-后端)
- [6. 测试中发现的缺陷](#6-测试中发现的缺陷)
- [7. 已知限制](#7-已知限制)
- [8. 测试脚本的适配](#8-测试脚本的适配)

## 1. 来源与基准

| 项目 | 内容 |
|---|---|
| 来源 | 34 个 dm-pcache 补丁（纯文本邮件，无 cover letter，无 `base-commit`） |
| 实际基准 | mainline `b539aeacf8cc`，由补丁 `index` 行中的 pre-image blob 反查得到 |
| 整合基点 | pcache-dist 复制功能（顶端 `9f809dce39aa`） |
| 整合结果 | 补丁系列 33 个 commit（其中一个 `fixup!` 已 autosquash 进对应补丁）+ 整合修正 12 个 commit |

补丁系列的基准早于上游 17 个 dm-pcache 修正（metadata 校验、replay 修正），在当前 mainline 上从第 2 个补丁起
即冲突。复制功能分支已包含这 17 个修正，直接 rebase 到其上，两类冲突一次处理。

每个 commit 都以 `git rebase --exec` 单独编译，出现任何 error 或 warning 即停止。

## 2. 合并冲突

| 补丁 | 文件 | 处理 |
|---|---|---|
| 02 route metadata access through cmedium_* | `cache_gc.c` `need_gc()`、`cache_key.c` `cache_replay()`、`cache_writeback.c` `is_cache_clean()` | 保留上游以 `cache_seg_remain()` 限制 kset 读取长度；读取改走 `cmedium_read()`（出错时返回 `-EIO`） |
| 05 add cache_dev_req bio backend | `Makefile` | 取并集 |
| 08 convert cache_write to async | `cache.h`、`cache_req.c` | 见 §4.1 |
| 13 defer all requests to the workqueue | `dm_pcache.c` `dm_pcache_map_bio()` | 采用补丁版本。`defered_req_fn()` 调用 `pcache_submit_req()`，延后的请求仍经复制路径 |
| 31 expose internal stats via debugfs | `dm_pcache.{c,h}` | 取并集 |

## 3. 合并后才暴露的语义问题

以下问题 git 没有报告冲突，自动合并后通过代码审查发现。

### 3.1 提交者上下文处理绕过复制（`978d46e486eb`）

补丁 29（process requests in the submitter's context）让 `dm_pcache_map_bio()` 直接调用
`pcache_cache_handle_req()`。该段与复制代码没有文本重叠，git 干净合并，但所有未被延后的写入
（即绝大多数写入）因此跳过 `pcache_submit_req()`，也就跳过了：

- 复制到对端；
- 重叠写入的排序 `pcache_wr_admit()`；
- 对端断线时的脏位图标记 `pcache_mark_all_peers_dirty()`。

```c
/* dm_pcache_map_bio(), after the fix */
ret = pcache_submit_req(pcache, pcache_req);	/* not pcache_cache_handle_req() */
if (ret == PCACHE_REQ_PARKED)			/* overlap admission parked it */
	return DM_MAPIO_SUBMITTED;
if (ret == -EBUSY)
	defer_req(pcache_req);
else
	pcache_req_put(pcache_req, ret);
```

### 3.2 secondary 写入的 `data_crc` 为 0（`675ec66806dc`）

补丁 15 改为在 `cache_data_write_submit()` 中由上层 bio 预先计算 `data_crc`，
`cache_key_data_crc()` 直接返回 `key->data_crc`。secondary 的接收路径 `pcache_net_write_to_cache()`
不经过 `cache_data_write_submit()`，`key->data_crc` 未赋值，持久化为 0。启用 `data_crc` 的 secondary
在下次 attach 时 replay 校验失败，cache 无法加载。

```c
/* pcache_net_write_to_cache(), after memcpy_flushcache() */
if (cache_data_crc_on(cache))
	/* key->len is final here: pcache_wait_seg() may have shrunk it */
	key->data_crc = crc32c(PCACHE_CRC_SEED, data + io_done, key->len);
```

### 3.3 接口变更（并入补丁 02/03）

- 补丁 02 删除 `cache_dev_zero_range()`，`dm_pcache_bitmap.c` 与 `dm_pcache_registry.c` 改用
  `cmedium_zero(cdev, cmedium_off(cdev, ptr), len)`。
- 补丁 03 把 `pcache_meta_find_latest()` 改为返回副本 index、以字节偏移为参数；bitmap header 与
  peer registry 的加载函数随之改写。

这两处是纯接口适配，以 `fixup!` 并入补丁 02/03，保证每个 commit 可单独编译。

## 4. 设计决定

### 4.1 写入路径：保留 reserve/commit 两阶段

复制路径把写入拆为 `pcache_cache_write_reserve()`（唯一可能返回 `-EBUSY` 的步骤，成功后才发往对端）与
`pcache_cache_write_commit()`。补丁 08 把 `cache_write()` 的每一轮改为“分配 → 标 WRITING →
`cache_data_write_submit()` → 插树 → append”。

合并方式：reserve 不变；commit 中的 `cache_copy_from_req_bio()` 换成“取 subtree → 标
`PCACHE_CACHE_KEY_FLAGS_WRITING` → `cache_data_write_submit()`”。补丁 08 新增的 `bool fua`
移到实际使用它的 commit 阶段。

### 4.2 commit 的组织

- 纯接口适配（§3.3）以 `fixup!` 并入原 commit，不改动原提交说明。
- 有语义的修正作为独立 commit，紧接在引入问题的补丁之后，便于审查与 bisect。
- 复制最初限制为仅 DAX 后端（`7e1f8d246851`：BIO 上不初始化 registry，带复制参数时 ctr 返回
  `-EOPNOTSUPP`），之后由 §5 的移植取代。

### 4.3 补丁 13 与 29

补丁 13 把全部请求移到 workqueue，补丁 29 又改回在提交者上下文处理。两者保持原样，未合并为一个 commit。

## 5. 复制功能移植到 BIO 后端

设计参照 DRBD：DRBD 不依赖 DAX，secondary 把收到的写入以 bio 提交，lower device 完成后才回 ACK
（Protocol C），只有 FUA/flush 要求落盘。

| commit | 内容 |
|---|---|
| `e3c9e8a64833` | `cache_dev_sync_kmem_io()` 的 bvec pool 只容纳 `PCACHE_CACHE_SUBTREE_SIZE`，而 peer bitmap 最大 32 MiB，会触发 `bio_add_page()` 的 `BUG_ON`。改为按 LBA 对齐分块提交 |
| `21a15ca3b72b` | peer registry 与 bitmap 改以字节偏移经 `cmedium_*` 访问；新增返回错误码的 `cmedium_write_sync()` |
| `de1ff4de6b5d` | secondary 接收路径、primary 的 FUA 处理，去掉仅 DAX 的限制 |
| `fa9b63bdab59` | `cache_verify_dirty_tail()` 改经 `cmedium_read()`（§6.1） |

设计约束：

1. **map 路径中不能对 BIO cache 设备做同步 I/O。** 内层 bio 排在 `current->bio_list` 上，要等外层 bio
   处理完才送出，会死锁。对端断线时 `pcache_submit_req()` 标记脏区，generation 由干净变脏时要写
   bitmap header，这条路径就在 map 中。header 因此改用 `cmedium_stage_write()`：DAX 上仍是
   `memcpy_flushcache` + `pmem_wmb`，BIO 上在下一次 `cmedium_flush()` 落盘，与 dirty bit 本身的持久化时机相同。
2. **`pcache_bm_flush_sync()` 原在 `bm->lock`（spinlock）内写介质。** BIO 写入会睡眠，改为锁内取脏页快照并清空，
   锁外写页；快照后被改动的位会重新标脏，写失败的页放回脏页集合。顺序为：写页 → flush → 写 header → flush，
   保证 header 不先于其描述的页落盘。内存 bitmap 按 4 KiB 整页分配，所有介质访问对齐 LBA。
3. **secondary 写数据**：BIO 上先以同步 bio 写数据，再插入 key，读者看不到数据未落地的 key，不需要 WRITING 标志；
   ACK 在设备完成写入后发送。FUA 写在所有分段之后补 `pcache_cache_flush()`。
4. **primary 复制快路径的 FUA。** `pcache_submit_req()` 直接调用 reserve/commit，跳过了补丁 30 在
   `pcache_cache_handle_req()` 中为 FUA 写补的 `pcache_cache_flush()`，FUA 会在 kset 映射落盘前完成。已在快路径补上。
5. resync 源端在 `pcache_cache_writeback_all()` 之后以 bio 读 backing，与 cache 后端无关，无需修改。

## 6. 测试中发现的缺陷

### 6.1 `cache_verify_dirty_tail()` 使用 DAX 指针（`fa9b63bdab59`）

`cache_verify_dirty_tail()` 是上游后来加入的 load 时校验，晚于补丁系列的基准。补丁系列把 DAX 访问
改为 `cmedium_*` 时该函数尚不存在，rebase 时没有冲突，它仍以 `copy_mc_to_kernel()` 读
`cache_pos_addr()`。BIO 上 `cache_dev->mapping` 为 NULL，重新 attach 带持久化 dirty_tail 的 cache 时出错：

```
BUG: unable to handle page fault for address: 0000000040015c40
 cache_verify_dirty_tail+0xaa/0x1d0 [dm_pcache]
 cache_tail_init+0x117/0x130 [dm_pcache]
 pcache_cache_start+0x2af/0x3f0 [dm_pcache]
 dm_pcache_ctr+0x2f0/0x5d0 [dm_pcache]
```

修正：与其它 kset 读取一致，保留 `cache_seg_remain()` 长度限制，读取改走 `cmedium_read()`。

rebase 一个“替换某类访问方式”的系列时，基准之后上游新增的代码不会产生冲突，需要在结果上重新检索被替换的
原语（此处为 `copy_mc_to_kernel`、`cache_pos_addr`）。

### 6.2 BIO 上 `data_crc` 与页面不稳定，writeback 停滞（`9294f700f7dc`）

**现象**：ext4 上 4K randrw 开始后 I/O 全部停止。debugfs 显示 cache 已满、大量请求延后、设备上没有 I/O，
`dirty_tail == key_tail` 不再前进：

```
cache_dev_type: bio      inflight_reqs: 13667
cache_dev_inflight_reqs: 0   backing_inflight_reqs: 0
cache_segs_used: 63/63   cache_full: 1
key_head: 0:2805616   dirty_tail: 0:50656   key_tail: 0:50656
```

未经整合的补丁系列原样构建在同一测试中以相同状态停滞，问题来自补丁系列本身。

**根因**：writeback 每次重试都在同一个 key 上校验失败：

```
cache_key_decode: key: 269307904:24576 seg 26:638976 data_crc error: 7b3ad2c5, expected: 98e3b1c6
```

`data_crc` 在提交时由上层 bio 的页面计算，而 BIO 上 cache 设备的写入随后才 DMA 同一批页面。设备未声明
stable writes 时，ext4 buffered write 可在写回期间修改页面，crc 与介质数据不符。`cache_writeback_fn()`
在校验失败时只延后重试，`dirty_tail` 不再前进，GC 无可回收，cache 满后设备停滞。DAX 在提交时同步拷贝，不受影响。

**修正**：在 `.io_hints` 中设置 `BLK_FEAT_STABLE_WRITES`，条件为启用复制，或 BIO 后端且启用 `data_crc`。
DRBD 同样一律设置该标志（`drbd_nl.c`），理由是：(1) 同一份 bio 页面同时用于本地写与网络发送，中途改动会使副本分歧；
(2) 启用数据校验时，发送前计算的校验和会与之后的内容不符。理由 (1) 同样适用于 DAX 复制。

验证：BIO + `data_crc=true` 时 `queue/stable_writes` 为 1，`data_crc=false` 时为 0；`test-nvme-mode.sh` 全部通过。

### 6.3 `writeback_all` 过早判定已干净（`b8cf5fe1318b`）

**现象**：BIO 复制回归中 F46（resync 与持续写入并行的 soak）每轮 resync 都报告完成，但 `bm_dirty` 持续上升，
soak 结束后数据不一致。

**根因**：resync 源端读 backing 前调用 `pcache_cache_writeback_all()`，它以 `is_cache_clean()` 判断写回是否完成：
在 `dirty_tail` 读到无效 kset 即视为日志结尾。BIO 上 kset 关闭后经暂存队列异步落盘，`key_head` 先前进、kset 后写上介质；
在此窗口内 `is_cache_clean()` 过早报告干净，resync 从 backing 读到旧数据发给对端并清除 dirty bit。

**修正**：BIO 上每次判断前先 `cache_dev_meta_drain()`，使已关闭的 kset 都已在介质上，无效 kset 只代表仍打开的那个，
与 DAX 语义一致。

### 6.4 未对齐的暂存缓冲区导致元数据丢失（`539822627f9b`）

**现象**：BIO 复制回归的长序列中，primary 的 writeback 在 `dirty_tail` 读到无效 kset（magic 为 0）而停止，
cache 满后设备停滞；设备上没有 I/O 在飞。

```
is_cache_clean: dirty_tail: 0:1348096 invalid kset magic: 0, key_num: 0
need_gc: key tail is equal to dirty tail: 0:1348096
```

扫描 cache 设备的 key log：`dirty_tail` 前一个 kset 正确结束于 `dirty_tail`，`dirty_tail` 处是前一个 kset
写下的空 kset 结束标记，而 `key_head` 远在其后——紧接 `dirty_tail` 的 kset 本体没有出现在介质上。
在暂存 worker 中对每笔写入立即读回比对，确认该 kset 本体从未写入设备，而不是被其它写入覆盖。

**根因**：`bio_split_io_at()` 对每个 bvec 检查 `bv_offset & lim->dma_alignment`，不满足即返回 `-EINVAL`
（loop 设备的 `dma_alignment` 为 511）。暂存项 `struct pcache_meta_stage` 的 `buf` 位于其它字段之后，不按 512 对齐。
offset 与长度都对齐 LBA 的写入跳过 RMW bounce，直接以该 buf 构造 bio 而被拒；`meta_stage_fn()` 未检查返回值，kset 丢失。
出问题的 kset 恰为 25 个 key（24 + 25 × 40 = 1024 字节）。单机大量写入时 kset 多为 128 个 key（5144 字节，不对齐，走 RMW），
很少触发；复制场景下 kset 多由计时器关闭，长度落在 512 倍数的机会大得多。

**修正**：`cache_dev_sync_kmem_io()` 的直接路径在 buffer 不满足 `bdev_dma_alignment()` 时改用对齐的 bounce buffer
（覆盖所有调用者）；`meta_stage_fn()` 写入失败时报告错误。

**验证**：BIO 上单独运行 F46，每轮 resync 都得到结论，soak 停止后数据立即一致，无 “staged metadata write failed”，
`dirty_tail == key_head`。

### 6.5 writeback 错误路径自锁（`857b49b95ff5`）

`cache_key_writeback()` 在持有 `writeback_lock` 时运行，其越界 WARN 分支与 `cache_key_writeback_bio()` 的错误分支
都再次获取 `writeback_lock`，第一次走到即自锁。修正：直接调用 `writeback_ctx_end()`，并以 `lockdep_assert_held()`
标明前提。代码审查发现。

### 6.6 writeback 停止后请求无限等待（`4ff9d6f1f4b4`）

writeback 遇到数据校验失败或 I/O 错误后永久停止（不把完整性未知的数据写回 backing），但请求端仍把 cache 满时的
`-EBUSY` 当作“稍后重试”，所有写入最终处于 D 状态，设备也无法移除。

修正：writeback 停止时打印一次原因并唤醒所有等待空间的请求；cache 也满之后把 `-EBUSY` 转为 `-EIO`
（map 路径、延后队列、`end_req()`；secondary 接收路径不再等待 segment，回 NACK 或断开，由 primary 标记对端欠数据）。
依据是 DRBD 的做法：本地 I/O 错误转为 disk state 变化并向上返回错误，而不是排队等待没有出路的 I/O。

验证：`tests/repro/wb_halt_eio.sh`（BIO，`data_crc` 开启）写入数据后改坏介质上的内容，writeback 打印
“cached data does not match its data_crc” 后停止；继续写到 cache 满时写入得到 `Input/output error`，
无 D 状态进程，`dmsetup remove` 成功。

## 7. 已知限制

| 项 | 说明 |
|---|---|
| bisect 空窗 | 补丁 11（BIO 后端可用）到 `7e1f8d246851` 之间，BIO 设备可能在 registry flush 时出错；补丁 29 到 `978d46e486eb` 之间，复制被绕过；`fa9b63bdab59` 之前，BIO cache 重新 attach 会在 `cache_verify_dirty_tail()` 出错。只影响中间状态 |
| cache 设备最小容量 | 布局在 segment 前固定预留 1 GiB bitmap 区（BIO 上相同），小于约 1.3 GiB 的 cache 放不下任何 segment；`PCACHE_CACHE_DEV_SIZE_MIN` 仍为 512 MB |
| 超级块格式 | `struct pcache_sb` 新增 `block_size`、`cache_type`，crc 覆盖整个结构；旧版本格式化的 cache 校验失败（“corrupted sb”），需要重新格式化 |
| BIO writeback | `cache_key_writeback_bio()` 逐个 key 同步读回再写 backing，单线程串行，是 BIO 单机带宽的瓶颈 |
| BIO secondary 接收 | 每个分段同步写入后才回 ACK，并发越高串行等待越明显；可参照 DRBD 异步提交 peer request、完成时回 ACK |

## 8. 测试脚本的适配

补丁系列自带的单节点测试（`tests/standalone/`）做了以下修改：

| 原样 | 改为 | 原因 |
|---|---|---|
| `modprobe dm-pcache` | `insmod "${KO:-/root/dm-pcache.ko}"` | 确保加载的是待测模块，而不是 `/lib/modules` 中已安装的版本 |
| target `pcache` | `pcache-dist` | 本模块注册的 target 名 |
| cache 512 MiB / 1 GiB | 2 GiB | §7 的 1 GiB bitmap 区 |

多节点功能回归（`demo_features.sh`）以 `/dev/pmem0` 为 cache 设备。在 BIO 后端上运行同一套回归：

- `tests/bench/bio_cachedev.sh up`：在每个节点建立稀疏映像并挂为 loop 设备，以固定名称 `/dev/pcache-cdev` 指向它，
  并安装开机重建的 systemd unit（部分 demo 会使节点重启）；
- `tests/bench/make_bio_harness.sh`：生成把 `/dev/pmem0` 替换为 `/dev/pcache-cdev` 的副本到 `tests/bench/bio-harness/`
  （不提交）。布局相同，demo 中以固定偏移读写元数据的部分照样适用。
