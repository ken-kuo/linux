# dm-pcache / pcache-dist 测试

所有脚本都在主机上运行，通过 SSH（root）操作三台测试 VM。完整说明见
[`../docs/dm-pcache-test-guide.md`](../docs/dm-pcache-test-guide.md)，结果见
[`../docs/pcache-dist-test-report.md`](../docs/pcache-dist-test-report.md)。

| 路径 | 用途 |
|---|---|
| `demo_features.sh` | 功能回归 F01–F51（`./demo_features.sh all`，或指定单项如 `f11`） |
| `dm_pcache_phase1_demo.sh` | 被 `demo_features.sh` 引用的公共函数与早期 demo |
| `bench/common.sh` | 节点 IP、端口、设备名等公共设定，其它 bench 脚本都会 source |
| `bench/bio_cachedev.sh`、`bench/make_bio_harness.sh` | 在 BIO cache 设备上重跑同一套回归（生成 `bench/bio-harness/`） |
| `bench/stress_arm.sh` | 回归期间在各节点持续施加 `stress` |
| `bench/host_health.sh`、`bench/transport_rr.sh` | 性能测试前的主机环境检查 |
| `bench/bio_vs_dax.sh`、`bench/repl_ab.sh`、`bench/ab_cut.sh` | 性能测量（同一 session 内交错 A/B） |
| `bench/run_lat_trace.sh`、`bench/parse_lat_trace.py` | 复制写入的延迟分解（需要 `PCACHE_LAT_TRACE`） |
| `repro/` | 已修缺陷的复现与诊断脚本（说明见测试指南“复现与诊断脚本”） |
| `standalone/` | 单节点 BIO 测试（在节点上直接运行） |

## 前提

- 三个测试节点 node-1/2/3（KVM 虚拟机）。节点 IP 与 libvirt 域名定义在 `bench/common.sh`、
  `demo_features.sh` 与 `dm_pcache_phase1_demo.sh` 开头，更换环境时修改这几处。
- 主机能以 root 免密码 SSH 到各 VM。
- 模块已编译并复制到各 VM 的 `/root/dm-pcache.ko`：

  ```bash
  make M=drivers/md/dm-pcache modules
  for ip in $NODE1 $NODE2 $NODE3; do    # node IP addresses
          scp drivers/md/dm-pcache/dm-pcache.ko root@$ip:/root/dm-pcache.ko
  done
  ```

- DAX 回归需要各 VM 有 fsdax 模式的 `/dev/pmem0`；BIO 回归先运行 `bench/bio_cachedev.sh up`。
- 需要在主机上提权的脚本使用 `$SUDO`（默认 `sudo`）。

## 常用命令

```bash
cd drivers/md/dm-pcache/tests

# DAX regression
./demo_features.sh all

# BIO regression
bench/bio_cachedev.sh up
bench/make_bio_harness.sh
bench/bio-harness/demo_features.sh all
bench/bio_cachedev.sh down

# single-node BIO tests (on a VM)
KO=/root/dm-pcache.ko CACHE_SIZE_MB=2048 ./standalone/test-nvme-mode.sh

# performance, after the environment check
bench/host_health.sh
bench/repl_ab.sh 3
```

结果写在 `bench/results/`，由 `.gitignore` 排除。
