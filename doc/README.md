# RocksDB 源码精读笔记

面试导向的 RocksDB 核心源码阅读笔记。聚焦 **workflow、调用链、关键机制**，不做全量通读。

参考版本：facebook/rocksdb @ `e6a2ee0`（本地浅克隆于 `~/Desktop/Lab/rocksdb`，笔记中的行号与 GitHub 链接均锚定此版本）

## 每篇笔记的使用纪律

1. 通读「核心概念总览 → 各知识点 → 核心要点总结」
2. **待验证点**：按 `文件:行号` 到本地 RocksDB 源码核实笔记结论——训练源码导航能力，同时纠正可能的错误
3. **自测 Checkpoint**：合上笔记，用自己的话作答后再对照
4. 笔记只记录 RocksDB 知识本身，不做 KopiDB 对照、不单列设计取舍章节

## 索引

| 文件 | 主题 | 状态 |
|------|------|------|
| [skiplist-1.md](skiplist-1.md) | 跳表（上）：写入路径地图 + InternalKey 编码 | ✅ |
| skiplist-2.md | 跳表（下）：Node 内存布局 + 无锁并发 | 待写 |
| memtable.md | MemTable | 待写 |
| sst.md | Block / SST 格式 | 待写 |
| compaction.md | Compaction / LSM Engine | 待写 |
| mvcc.md | MVCC / SequenceNumber | 待写 |
| wal.md | WAL 与崩溃恢复 | 待写 |
| wisckey.md | WiscKey 键值分离 | 待写 |
| redis.md | 网络层 / RESP 协议 | 待写 |

## 信息源优先级

1. 本地真实源码（ground truth，一切结论以其为准）
2. [DeepWiki](https://deepwiki.com/facebook/rocksdb)（模块地图 / 概览 / 定位文件）
3. [RocksDB Wiki](https://github.com/facebook/rocksdb/wiki)（官方设计文档）
4. LevelDB 同模块（设计同源的简化对照）
