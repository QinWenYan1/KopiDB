# KopiDB

一个类 RocksDB 的教学级 LSM-tree KV 存储引擎（C++20），
跟随 [tiny-lsm 教程](https://vanilla-beauty.github.io/tiny-lsm/book/introduction.html) 逐模块手写实现。

## 📖 背景：LSM-tree 与 RocksDB

LSM-tree（Log-Structured Merge-Tree，O'Neil et al. 1996）的核心思想是把随机写转化为顺序写：
写入先进内存有序结构（MemTable）并追加 WAL，写满后批量刷盘为不可变 SST 文件，后台 Compaction 逐层合并。

- **优势**：写吞吐显著高于 B+ 树；顺序 IO 对 SSD 友好
- **代价**：读放大 / 写放大——工业界用 Bloom Filter、块缓存、分层 Compaction 策略缓解
- **代表系统**：LevelDB、RocksDB（Meta）；TiKV、Cassandra、HBase、MyRocks、Flink/Kafka Streams 状态后端均基于或受其影响
- **典型场景**：写密集负载——日志/消息、时序数据、元数据、流计算状态、区块链客户端存储

延伸阅读：

- [The Log-Structured Merge-Tree (LSM-tree), Acta Informatica 1996](https://doi.org/10.1007/s002360050048)——LSM-tree 原始论文
- [The RocksDB Experience, FAST 2021](https://www.usenix.org/conference/fast21/presentation/dong)——RocksDB 官方经验论文
- [WiscKey, FAST 2016](https://www.usenix.org/conference/fast16/technical-sessions/presentation/lu)——本项目 vlog 模块（键值分离）的思想来源
- [RocksDB Wiki](https://github.com/facebook/rocksdb/wiki) · [DeepWiki RocksDB 源码导览](https://deepwiki.com/facebook/rocksdb)

## ✨ 特性与进度

| 模块 | 对应 Lab | 状态 | RocksDB 源码对照笔记 |
|------|---------|------|---------------------|
| SkipList 跳表（MemTable 容器） | Lab 1 | 🚧 | 待写 |
| MemTable | Lab 2 | ⬜ | 待写 |
| Block / SST | Lab 3 | ⬜ | 待写 |
| LSM Engine / Compaction | Lab 4 | ⬜ | 待写 |
| MVCC 事务 | Lab 5.x | ⬜ | 待写 |
| WAL 与崩溃恢复 | 待补 | ⬜ | 待写 |
| WiscKey 键值分离 | 待补 | ⬜ | 待写 |
| Redis RESP 协议兼容 | 待补 | ⬜ | 待写 |

> 每个模块完成后，对照笔记（`doc/`）记录 RocksDB 同模块的 workflow、调用链与设计取舍。

## 🛠 技术栈

C++20 · Xmake · GoogleTest · spdlog · toml11 · asio · pybind11 · clangd

（详细环境配置流程见教程 [Lab 0 环境准备](https://vanilla-beauty.github.io/tiny-lsm/book/lab0-env.html)）

## 🚀 构建与测试

```bash
xmake                      # 构建全部目标
xmake run test_skiplist    # 运行指定模块测试
```

## 📁 项目结构

- `src/` —— 引擎源码（skiplist / memtable / block / sst / wal / lsm / redis_wrapper …）
- `test/` —— 单元测试（GoogleTest）
- `doc/` —— RocksDB 源码精读笔记（workflow、调用链、设计取舍）

## 📚 参考资料

- 教程：[tiny-lsm book](https://vanilla-beauty.github.io/tiny-lsm/book/introduction.html)
- [RocksDB](https://github.com/facebook/rocksdb) · [RocksDB Wiki](https://github.com/facebook/rocksdb/wiki) · [DeepWiki](https://deepwiki.com/facebook/rocksdb)
