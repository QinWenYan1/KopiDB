# KopiDB

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/wiki/faq/cpp20)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)](https://github.com/QinWenYan1/KopiDB)
[![Build](https://img.shields.io/badge/build-Xmake-green.svg)](https://xmake.io)
[![RocksDB DeepWiki](https://img.shields.io/badge/RocksDB-DeepWiki-purple.svg)](https://deepwiki.com/facebook/rocksdb)

一个类 RocksDB 的教学级 LSM-tree KV 存储引擎，跟随 [tiny-lsm 教程](https://vanilla-beauty.github.io/tiny-lsm/book/introduction.html) 逐模块手写实现。

## 📖 什么是 LSM-tree？

LSM-tree（Log-Structured Merge-Tree，O'Neil et al. 1996）的核心思想是**把随机写转化为顺序写**：写入先进内存有序结构（MemTable）并追加 WAL 保证持久化，内存写满后批量刷盘为不可变 SST 文件，后台 Compaction 逐层合并清理。RocksDB（Meta）是这一结构的工业级代表实现，也是本项目对标的对象。

延伸阅读：

- [The Log-Structured Merge-Tree (LSM-tree), Acta Informatica 1996](https://doi.org/10.1007/s002360050048)——LSM-tree 原始论文
- [The RocksDB Experience, FAST 2021](https://www.usenix.org/conference/fast21/presentation/dong)——RocksDB 官方经验论文
- [WiscKey, FAST 2016](https://www.usenix.org/conference/fast16/technical-sessions/presentation/lu)——本项目 vlog 模块（键值分离）的思想来源

## 🤔 为什么需要 LSM-tree？

相比 B+ 树（读优化的原地更新结构）：

1. **写吞吐显著更高**：B+ 树写入伴随大量随机 IO 与页分裂；LSM 先写内存 + 顺序刷盘，写路径几乎全是顺序 IO
2. **对 SSD 友好**：批量顺序写减少擦写放大，延长 SSD 寿命
3. **写入延迟稳定**：无原地更新，前台写路径短

代价是读放大（查询要查多层）与写放大（数据被 Compaction 反复重写）——工业界用 Bloom Filter、块缓存、分层 Compaction 策略来缓解。

## 🎯 应用场景

写密集负载的典型选择：

- 日志 / 消息系统、时序数据
- 元数据存储、区块链客户端存储
- 流计算状态后端（Flink / Kafka Streams 内嵌 RocksDB）
- 作为底层引擎支撑上层数据库（TiKV、MyRocks、Cassandra、HBase 等均基于或受 LSM 影响）

## 📚 学习文档

`doc/` 下是与实现同步推进的 **RocksDB 源码精读笔记**（workflow / 调用链 / 关键机制，面试导向）：

- [doc/README](doc/README.md)——笔记索引与使用方法（一篇笔记对应一个子 lab）
- [01-skiplist-basic](doc/01-skiplist-basic.md)——Lab 1.1：跳表 put/get/remove 的 RocksDB 对应实现
- [02-skiplist-iter](doc/02-skiplist-iter.md)——Lab 1.2：跳表迭代器
- [03-skiplist-scan](doc/03-skiplist-scan.md)——Lab 1.3：前缀与边界扫描
- [04-data-path](doc/04-data-path.md)——数据通路：Put/Get 工作流 + SuperVersion 世界观（Lab 2 前置）
- [05-memtable-rw](doc/05-memtable-rw.md)——Lab 2.1：MemTable 读写、冻结与墓碑
- [06-memtable-iter](doc/06-memtable-iter.md)——Lab 2.2：MemTable 迭代器与归并
- [07-memtable-scan](doc/07-memtable-scan.md)——Lab 2.3：MemTable 前缀扫描

## 🛠 技术栈

C++20 · Xmake · GoogleTest · spdlog · toml11 · asio · pybind11 · clangd

（详细环境配置流程见教程 [Lab 0 环境准备](https://vanilla-beauty.github.io/tiny-lsm/book/lab0-env.html)）

## 🚀 构建与测试

```bash
xmake                      # 构建全部目标（默认 debug）
xmake run test_skiplist    # 运行指定模块测试
```

## 🗺️ Roadmap

- [x] Lab 0 环境准备
- [x] Lab 1 SkipList 跳表｜对照笔记 [01](doc/01-skiplist-basic.md) [02](doc/02-skiplist-iter.md) [03](doc/03-skiplist-scan.md) [04](doc/04-data-path.md)
- [ ] Lab 2 MemTable（🚧 进行中）｜对照笔记 [05](doc/05-memtable-rw.md) [06](doc/06-memtable-iter.md) [07](doc/07-memtable-scan.md)
- [ ] Lab 3 Block / SST
- [ ] Lab 4 LSM Engine / Compaction
- [ ] Lab 5 MVCC 事务
- [ ] WAL 与崩溃恢复
- [ ] WiscKey 键值分离
- [ ] Redis RESP 协议兼容

> 每个模块完成后，对应 RocksDB 源码笔记会更新在 `doc/` 并在此挂链接。

## 📁 项目结构

- `src/` —— 引擎源码（skiplist / memtable / block / sst / wal / lsm / redis_wrapper …）
- `test/` —— 单元测试（GoogleTest）
- `doc/` —— RocksDB 源码精读笔记

## 📚 参考资料

- 教程：[tiny-lsm book](https://vanilla-beauty.github.io/tiny-lsm/book/introduction.html)
- [RocksDB](https://github.com/facebook/rocksdb) · [RocksDB Wiki](https://github.com/facebook/rocksdb/wiki) · [DeepWiki](https://deepwiki.com/facebook/rocksdb)
