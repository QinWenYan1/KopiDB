# RocksDB 源码精读笔记

面试导向的 RocksDB 核心源码阅读笔记。聚焦 **workflow、调用链、关键机制**，不做全量通读。

参考版本：facebook/rocksdb @ `e6a2ee0`（本地浅克隆于 `~/Desktop/Lab/rocksdb`，笔记中的行号与 GitHub 链接均锚定此版本）

## 每篇笔记的使用纪律

1. 通读「核心概念总览 → 各知识点 → 面试速记版」
2. **一篇笔记对应一个子 lab**：篇内只讲该 lab 要实现的函数在 RocksDB 里的对应实现，超纲内容不写
3. 笔记只记录 RocksDB 知识本身，不做 KopiDB 对照、不单列设计取舍章节
4. **编号即推荐阅读顺序**；每篇只深入自己的主题，衔接处互相引用而非重述（workflow 级内容归 04 数据通路）

## 索引

| 文件 | 对应 lab | 主题 | 状态 |
|------|------|------|------|
| [01-skiplist-basic.md](01-skiplist-basic.md) | Lab 1.1 | 跳表 put/get/remove：结构原理 + Node 布局 + Arena + RandomHeight + Insert/Contains | ✅ |
| [02-skiplist-iter.md](02-skiplist-iter.md) | Lab 1.2 | 跳表迭代器：InlineSkipList::Iterator 能力清单 + Prev 的代价 | ✅ |
| [03-skiplist-scan.md](03-skiplist-scan.md) | Lab 1.3 | 跳表范围查询：SliceTransform 前缀 + PrefixCheck + iterate bounds | ✅ |
| [04-data-path.md](04-data-path.md) | 世界观（无 lab 对应） | 数据通路：Put/Get 工作流 + SuperVersion（Lab 2 前置） | ✅ |
| [05-memtable-rw.md](05-memtable-rw.md) | Lab 2.1 | MemTable 读写与冻结：双态 + 编码 + 回放 + 点查 + 冻结 + 墓碑 | ✅ |
| [06-memtable-iter.md](06-memtable-iter.md) | Lab 2.2 | MemTable 迭代器：迭代器契约 + MemTableIterator + MergingIterator 归并 | ✅ |
| [07-memtable-scan.md](07-memtable-scan.md) | Lab 2.3 | MemTable 前缀扫描：prefix bloom 建与问 + 无谓词下推 | ✅ |
| 08-sst.md | Lab 3 | Block / SST 格式 | 待写 |
| 09-compaction.md | Lab 4 | Compaction / LSM Engine | 待写 |
| 10-mvcc.md | Lab 5 | MVCC / SequenceNumber | 待写 |
| 11-wal.md | — | WAL 与崩溃恢复 | 待写 |
| 12-wisckey.md | — | WiscKey 键值分离 | 待写 |
| 13-redis.md | — | 网络层 / RESP 协议 | 待写 |

## 信息源优先级

1. 本地真实源码（ground truth，一切结论以其为准）
2. [DeepWiki](https://deepwiki.com/facebook/rocksdb)（模块地图 / 概览 / 定位文件）
3. [RocksDB Wiki](https://github.com/facebook/rocksdb/wiki)（官方设计文档）
4. LevelDB 同模块（设计同源的简化对照）
