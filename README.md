# KopiDB

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/wiki/faq/cpp20) [![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)](https://github.com/QinWenYan1/KopiDB) [![Build](https://img.shields.io/badge/build-Xmake-green.svg)](https://xmake.io) [![RocksDB DeepWiki](https://img.shields.io/badge/RocksDB-DeepWiki-purple.svg)](https://deepwiki.com/facebook/rocksdb)

KopiDB 是一个用于学习数据库实现的 C++20 LSM-tree KV 引擎，包含有序读写、SST 存储、多版本读取和事务接口。

---

## 目录

- [什么是 LSM-tree？](#what-is-lsm)
- [为什么需要 LSM-tree？](#why-lsm)
- [应用场景](#use-cases)
- [学习文档](#learning-docs)
- [构建与测试](#build-and-test)
- [C++ 使用示例](#cpp-example)
  - [普通 CRUD](#basic-crud)
  - [显式事务](#transactions)



--- 

<a id="what-is-lsm"></a>

## 什么是 LSM-tree？

LSM-tree（Log-Structured Merge-Tree，O'Neil et al. 1996）的核心思想是**把随机写转化为顺序写**：写入先进内存有序结构（MemTable）并追加 WAL 保证持久化，内存写满后批量刷盘为不可变 SST 文件，后台 Compaction 逐层合并清理。RocksDB（Meta）是这一结构的工业级代表实现，也是本项目对标的对象。

延伸阅读：

- [The Log-Structured Merge-Tree (LSM-tree), Acta Informatica 1996](https://doi.org/10.1007/s002360050048)——LSM-tree 原始论文
- [The RocksDB Experience, FAST 2021](https://www.usenix.org/conference/fast21/presentation/dong)——RocksDB 官方经验论文
- [WiscKey, FAST 2016](https://www.usenix.org/conference/fast16/technical-sessions/presentation/lu)——本项目 vlog 模块（键值分离）的思想来源


---

<a id="why-lsm"></a>

## 为什么需要 LSM-tree？

相比 B+ 树（读优化的原地更新结构）：

1. **写吞吐显著更高**：B+ 树写入伴随大量随机 IO 与页分裂；LSM 先写内存 + 顺序刷盘，写路径几乎全是顺序 IO
2. **对 SSD 友好**：批量顺序写减少擦写放大，延长 SSD 寿命
3. **写入延迟稳定**：无原地更新，前台写路径短

代价是读放大（查询要查多层）与写放大（数据被 Compaction 反复重写）——工业界用 Bloom Filter、块缓存、分层 Compaction 策略来缓解。

---

<a id="use-cases"></a>

## 应用场景

LSM 架构常见于写密集负载的典型选择：

- 日志 / 消息系统、时序数据
- 元数据存储、区块链客户端存储
- 流计算状态后端（Flink / Kafka Streams 内嵌 RocksDB）
- 作为底层引擎支撑上层数据库（TiKV、MyRocks、Cassandra、HBase 等均基于或受 LSM 影响）

---

<a id="learning-docs"></a>

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

---



<a id="build-and-test"></a>

## 构建与测试

需要 C++20 编译器和 [Xmake](https://xmake.io)，配置见 [config.toml](config.toml)。

```bash
xmake build lsm             # 构建引擎
xmake build test_skiplist
xmake run test_skiplist     # 运行一个模块的测试
```

环境安装步骤见教程的 [Lab 0](https://vanilla-beauty.github.io/tiny-lsm/book/lab0-env.html)。

---

<a id="cpp-example"></a>

## C++ 使用示例

普通读写直接使用 `LSM`；需要把多次操作作为一笔事务处理时，再调用 `begin_tran()`。

> 以下展示公开接口的用法；当前入口初始化尚未完成，暂不能直接运行这两个示例。

<a id="basic-crud"></a>

### 普通 CRUD

`put()` 用于新增或更新，`get()` 查询，`remove()` 删除；不需要手动提交事务。

```cpp
#include "lsm/engine.h"
#include <iostream>

using namespace tiny_lsm;

int main() {
  LSM db("kv-demo");

  // 新增
  db.put("name", "Alice");

  // 查询：key 不存在时返回 std::nullopt。
  if (auto value = db.get("name")) {
    std::cout << *value << '\n';  // Alice
  }

  // 更新：对同一个 key 再次 put。
  db.put("name", "Bob");

  // 删除
  db.remove("name");
  return 0;
}
```

<a id="transactions"></a>

### 显式事务

通过 `begin_tran()` 开启事务，在事务句柄上读写，最后调用 `commit()` 提交。

```cpp
#include "lsm/engine.h"
#include <iostream>

using namespace tiny_lsm;

int main() {
  LSM db("transaction-demo");
  auto tx = db.begin_tran(IsolationLevel::REPEATABLE_READ);

  tx->put("name", "Alice");
  tx->put("city", "Shanghai");

  // 事务能读到自己尚未提交的修改。
  if (auto value = tx->get("name")) {
    std::cout << *value << '\n';  // Alice
  }

  if (!tx->commit()) {
    std::cerr << "事务提交失败\n";
    return 1;
  }
  return 0;
}
```

如果决定放弃这笔事务，将提交操作改为 `tx->abort()`；事务结束后不再使用该句柄读写。

使用非空 key；空 value 表示删除，删除操作使用 `remove(key)`。项目目前用于学习，尚不提供 WAL 崩溃恢复保证。

