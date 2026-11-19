# LSM-tree 背景与学习资料

[← 返回项目主页](../README.md#tools-and-docs)

本页介绍 LSM-tree 的基本结构、设计取舍和常见应用，并整理 KopiDB 的学习资料。应用接入、API 示例和当前实现的限制见[使用指南](usage.md)。

## 🧭 目录

- [📖 什么是 LSM-tree？](#what-is-lsm)
- [💡 为什么需要 LSM-tree？](#why-lsm)
- [🎯 应用场景](#use-cases)
- [📚 RocksDB 源码精读笔记](#learning-docs)
- [🧱 项目来源与教程](#source-and-tutorial)
- [📄 参考论文](#references)

<a id="what-is-lsm"></a>

## 📖 什么是 LSM-tree？

LSM-tree（Log-Structured Merge-Tree，O'Neil et al. 1996）的核心思想是**把随机写转化为顺序写**。常见实现的写入流程是：

1. 写入先进入内存中的有序结构 **MemTable**，并追加预写日志 **WAL（Write-Ahead Log）**，为持久化与恢复提供基础。
2. MemTable 写满后，批量刷盘为不可变的 **SST（Sorted String Table）** 文件。
3. 后台 **Compaction** 逐层合并文件、清理旧版本和删除标记，控制文件数量与数据布局。

RocksDB（Meta）是这一结构的工业级代表实现，也是本项目学习和对照的对象。

<a id="why-lsm"></a>

## 💡 为什么需要 LSM-tree？

B+ 树通常面向高效读取，采用页内原地更新；写入可能涉及随机 I/O 和页分裂。LSM-tree 通过内存缓冲与批量刷盘，把更多磁盘写入组织为顺序 I/O，因此适合写入密集的负载。

主要收益包括：

1. **提高写吞吐**：先在内存处理写入，再集中刷盘，减少前台逐条随机更新磁盘的成本。
2. **利用 SSD 的批量写能力**：顺序、批量写有利于减少随机更新带来的擦写开销；实际寿命收益仍取决于总写入量和 Compaction 的写放大。
3. **缩短前台写路径**：不直接修改已有 SST，通常有利于控制写入延迟；刷盘或 Compaction 跟不上时仍可能出现写入等待。

这些收益需要付出代价：

- **读放大**：一次查询可能需要检查内存和多个层级的文件。
- **写放大**：Compaction 会反复重写数据，磁盘实际写入量可能高于用户写入量。

工业实现通过 Bloom Filter、块缓存和分层 Compaction 策略缓解这些成本。LSM-tree 的取舍应结合读写比例、数据规模和资源预算判断，不能只用顺序写解释全部性能。

<a id="use-cases"></a>

## 🎯 应用场景

LSM 架构常见于以下写密集负载：

- 日志、消息系统与时序数据。
- 元数据存储与区块链客户端存储。
- 流计算状态后端，例如 Flink、Kafka Streams 内嵌 RocksDB。
- 上层数据库的底层存储引擎，例如 TiKV、MyRocks、Cassandra、HBase 等基于或受到 LSM 设计影响的系统。

这些是 **LSM 架构的一般应用场景**。KopiDB 是用于学习数据库实现的实验项目；完整并发支持、断电及持久化中途失败的恢复保障仍在完善中，`SERIALIZABLE` 也尚未完整实现。当前 API 行为与保证范围见[使用指南](usage.md#choosing-read-write-mode)。

<a id="learning-docs"></a>

## 📚 RocksDB 源码精读笔记

[`doc/` 笔记索引](../doc/README.md)保留全部七篇已有笔记，涵盖跳表读写、迭代器、前缀与边界扫描、Put/Get 数据通路与 SuperVersion，以及 MemTable 读写、冻结、墓碑、归并和前缀扫描。

笔记聚焦工作流、调用链和关键机制，面向源码学习与面试复习；编号是推荐阅读顺序，具体主题、对应 Lab 和后续计划统一维护在索引中。

<a id="source-and-tutorial"></a>

## 🧱 项目来源与教程

KopiDB 的学习框架来自 [Vanilla-Beauty/tiny-lsm](https://github.com/Vanilla-Beauty/tiny-lsm)，沿 [tiny-lsm 教程](https://vanilla-beauty.github.io/tiny-lsm/book/introduction.html)逐模块实现 LSM-tree KV 引擎。

教程用于理解模块划分与实现路径；RocksDB 源码笔记作为独立的学习副线，用于进一步理解工业实现中的工作流和机制。

<a id="references"></a>

## 📄 参考论文

- [The Log-Structured Merge-Tree (LSM-tree), Acta Informatica 1996](https://doi.org/10.1007/s002360050048)：LSM-tree 原始论文。
- [The RocksDB Experience, FAST 2021](https://www.usenix.org/conference/fast21/presentation/dong)：RocksDB 团队的工业实践经验。
- [WiscKey, FAST 2016](https://www.usenix.org/conference/fast16/technical-sessions/presentation/lu)：本项目 vlog 模块（键值分离）的思想来源。
