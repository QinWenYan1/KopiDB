# RocksDB 源码精读笔记

面试导向的 RocksDB 核心源码阅读笔记。聚焦 **workflow、调用链、关键机制**，不做全量通读。

参考版本：facebook/rocksdb @ `e6a2ee0`（本地浅克隆于 `~/Desktop/Lab/rocksdb`，笔记中的行号与 GitHub 链接均锚定此版本）

## 每篇笔记的使用纪律

1. 通读「核心概念总览 → 各知识点 → 面试速记版」
2. 笔记只记录 RocksDB 知识本身，不做 KopiDB 对照、不单列设计取舍章节
3. **编号即推荐阅读顺序**；每篇只深入自己的主题，衔接处互相引用而非重述（workflow 级内容归 04 数据通路）

## 索引

| 文件 | 主题 | 状态 |
|------|------|------|
| [01-skiplist.md](01-skiplist.md) | 跳表：结构原理 + Node 内存布局 + 无锁并发 + Arena | ✅ |
| [02-iterator.md](02-iterator.md) | 迭代器体系：两层接口 + 层层包装 + 可见性过滤 | ✅ |
| [03-scan.md](03-scan.md) | 扫描：SliceTransform 前缀提取器 + 前缀扫描三模式 + iterate bounds | ✅ |
| [04-data-path.md](04-data-path.md) | 数据通路：Put/Get 工作流 + SuperVersion 世界观（Lab 2 前置） | ✅ |
| 05-memtable.md | MemTable：双态结构、编码体系、写读路径、冻结与并发 | 🚧 起草中 |
| 06-sst.md | Block / SST 格式 | 待写 |
| 07-compaction.md | Compaction / LSM Engine | 待写 |
| 08-mvcc.md | MVCC / SequenceNumber | 待写 |
| 09-wal.md | WAL 与崩溃恢复 | 待写 |
| 10-wisckey.md | WiscKey 键值分离 | 待写 |
| 11-redis.md | 网络层 / RESP 协议 | 待写 |

## 信息源优先级

1. 本地真实源码（ground truth，一切结论以其为准）
2. [DeepWiki](https://deepwiki.com/facebook/rocksdb)（模块地图 / 概览 / 定位文件）
3. [RocksDB Wiki](https://github.com/facebook/rocksdb/wiki)（官方设计文档）
4. LevelDB 同模块（设计同源的简化对照）
