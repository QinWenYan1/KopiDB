# RocksDB 源码精读笔记

面试导向的 RocksDB 核心源码阅读笔记。聚焦 **workflow、调用链、设计取舍（为什么）**，不做全量通读。

参考版本：facebook/rocksdb @ `e6a2ee0`（本地浅克隆于 `~/Desktop/Lab/rocksdb`，笔记中的行号以此版本为准）

## 每篇笔记的使用纪律

1. 通读「模块职责 / workflow / 关键数据结构 / 设计取舍」
2. **待验证点**：按 `文件:行号` 到本地 RocksDB 源码核实草稿结论——训练源码导航能力，同时纠正草稿可能的错误
3. **KopiDB vs RocksDB 对照**：自己填写，这是面试叙事的核心
4. **自测题**：合上笔记，用自己的话作答后再对照

## 索引

| # | 主题 | 对应 KopiDB 模块 | 状态 |
|---|------|-----------------|------|
| 01 | SkipList（内存有序结构） | src/skiplist | 待写 |
| 02 | MemTable | src/memtable | 待写 |
| 03 | Block / SST 格式 | src/block, src/sst | 待写 |
| 04 | Compaction / LSM Engine | src/lsm | 待写 |
| 05 | MVCC / SequenceNumber | tranc_id 相关 | 待写 |
| 06 | WAL 与崩溃恢复 | src/wal | 待写 |
| 07 | WiscKey 键值分离 | src/vlog | 待写 |
| 08 | 网络层 / RESP 协议 | src/redis_wrapper | 待写 |

## 信息源优先级

1. 本地真实源码（ground truth，一切结论以其为准）
2. [DeepWiki](https://deepwiki.com/facebook/rocksdb)（模块地图 / 概览 / 定位文件）
3. [RocksDB Wiki](https://github.com/facebook/rocksdb/wiki)（官方设计文档）
4. LevelDB 同模块（设计同源的简化对照）
