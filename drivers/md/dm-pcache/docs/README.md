# dm-pcache / pcache-dist 文档

| 文件 | 内容 |
|---|---|
| [`pcache-dist-manual.md`](./pcache-dist-manual.md) | 设计与运维手册：架构、读写路径、peer 连接、脏位图与 resync、on-disk 布局、操作命令；§11 为 DAX 与 BIO 两种 cache 后端 |
| [`pcache-dist-test-report.md`](./pcache-dist-test-report.md) | 测试报告：分支组成、回归结果、性能、整合中修正的问题、已知限制、代码规模 |
| [`dm-pcache-test-guide.md`](./dm-pcache-test-guide.md) | 测试指南：环境、F01–F51 每项测试的定义与预期、测试工具 |
| [`FEATURES.md`](./FEATURES.md) | 功能参考：每项功能的原理、实现与验证命令 |
| [`bio-backend-integration.md`](./bio-backend-integration.md) | BIO cache 后端补丁系列的整合说明 |
| [`pcache-dist-fix-history.md`](./pcache-dist-fix-history.md) | 复制功能的缺陷修复记录（#1–#23）：症状、根因、修正、验证 |
| [`nvme-backend-design.md`](./nvme-backend-design.md) | BIO/NVMe cache 后端设计（英文） |
| [`assets/`](./assets/) | 文档引用的图 |

测试脚本见 [`../tests/`](../tests/)，用法见 [`../tests/README.md`](../tests/README.md)。
