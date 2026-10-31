# KopiDB

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/wiki/faq/cpp20) [![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)](https://github.com/QinWenYan1/KopiDB) [![Build](https://img.shields.io/badge/build-Xmake-green.svg)](https://xmake.io) [![RocksDB DeepWiki](https://img.shields.io/badge/RocksDB-DeepWiki-purple.svg)](https://deepwiki.com/facebook/rocksdb)

KopiDB 是一个用于学习数据库实现的 C++20 LSM-tree KV 引擎，包含有序读写、SST 存储、多版本读取和事务接口。

---

## 🧭 目录

- [📖 什么是 LSM-tree？](#what-is-lsm)
- [💡 为什么需要 LSM-tree？](#why-lsm)
- [🎯 应用场景](#use-cases)
- [📚 学习文档](#learning-docs)
- [🛠️ 构建与测试](#build-and-test)
- [💻 C++ 使用示例](#cpp-example)



--- 

<a id="what-is-lsm"></a>

## 📖 什么是 LSM-tree？

LSM-tree（Log-Structured Merge-Tree，O'Neil et al. 1996）的核心思想是**把随机写转化为顺序写**：写入先进内存有序结构（MemTable）并追加 WAL 保证持久化，内存写满后批量刷盘为不可变 SST 文件，后台 Compaction 逐层合并清理。RocksDB（Meta）是这一结构的工业级代表实现，也是本项目对标的对象。

延伸阅读：

- [The Log-Structured Merge-Tree (LSM-tree), Acta Informatica 1996](https://doi.org/10.1007/s002360050048)——LSM-tree 原始论文
- [The RocksDB Experience, FAST 2021](https://www.usenix.org/conference/fast21/presentation/dong)——RocksDB 官方经验论文
- [WiscKey, FAST 2016](https://www.usenix.org/conference/fast16/technical-sessions/presentation/lu)——本项目 vlog 模块（键值分离）的思想来源


---

<a id="why-lsm"></a>

## 💡 为什么需要 LSM-tree？

相比 B+ 树（读优化的原地更新结构）：

1. **写吞吐显著更高**：B+ 树写入伴随大量随机 IO 与页分裂；LSM 先写内存 + 顺序刷盘，写路径几乎全是顺序 IO
2. **对 SSD 友好**：批量顺序写减少擦写放大，延长 SSD 寿命
3. **写入延迟稳定**：无原地更新，前台写路径短

代价是读放大（查询要查多层）与写放大（数据被 Compaction 反复重写）——工业界用 Bloom Filter、块缓存、分层 Compaction 策略来缓解。

---

<a id="use-cases"></a>

## 🎯 应用场景

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

## 🛠️ 构建与测试

需要 C++20 编译器和 [Xmake](https://xmake.io)，配置见 [config.toml](config.toml)。

```bash
xmake build lsm             # 构建引擎
xmake build test_skiplist
xmake run test_skiplist     # 运行一个模块的测试
```

环境安装步骤见教程的 [Lab 0](https://vanilla-beauty.github.io/tiny-lsm/book/lab0-env.html)。

---

<a id="cpp-example"></a>

## 💻 C++ 使用示例

下面在同一个数据库实例中演示五类用法，用注释区分；为突出调用流程，省略提交结果处理。

> 以下为公开接口示意；当前入口初始化尚未完成，暂不能直接运行。

```cpp
#include "lsm/engine.h"
#include "lsm/level_iterator.h"
#include <iostream>

using namespace tiny_lsm;

int main() {
  LSM db("api-demo");

  // ---------- 1. 普通 CRUD：无需显式开启事务 ----------
  db.put("name", "Alice");  // 新增
  if (auto value = db.get("name")) {
    std::cout << *value << '\n';  // 查询：Alice
  }
  db.put("name", "Bob");  // 更新同一个 key
  db.remove("name");      // 删除
  if (!db.get("name")) {
    std::cout << "name 不存在\n";
  }

  // ---------- 2. 显式事务：开启、读写、提交 ----------
  auto tx = db.begin_tran(IsolationLevel::REPEATABLE_READ);
  tx->put("user:1", "Alice");
  tx->put("user:2", "Bob");
  if (auto value = tx->get("user:1")) {
    std::cout << *value << '\n';  // 能读到本事务尚未提交的修改
  }
  tx->commit();  // 提交本事务的修改；结束后不再用 tx 读写

  // ---------- 3. 事务回滚：放弃本事务的修改 ----------
  auto cancelled = db.begin_tran(IsolationLevel::REPEATABLE_READ);
  cancelled->put("user:1", "Temporary");
  cancelled->abort();  // 放弃修改；结束后不再用 cancelled 读写
  if (auto value = db.get("user:1")) {
    std::cout << *value << '\n';  // 前一笔事务提交成功时，仍为 Alice
  }

  // ---------- 4. 批量操作：一次传入多个 key ----------
  db.put_batch({{"batch:a", "A"}, {"batch:b", "B"}, {"batch:c", "C"}});
  for (const auto &[key, value] :
       db.get_batch({"batch:a", "batch:b", "missing"})) {
    // 每个结果保留对应 key；不存在的 key 返回 std::nullopt。
    std::cout << key << " = " << value.value_or("<不存在>") << '\n';
  }
  db.remove_batch({"batch:a", "batch:b"});

  // ---------- 5. 有序遍历：按 key 升序读取 ----------
  // begin(0) 的 0 表示不限制读取版本号。
  for (auto it = db.begin(0); it != db.end(); ++it) {
    std::cout << it->first << " = " << it->second << '\n';
  }

  return 0;
}
```

使用非空 key；空 value 表示删除，删除操作使用 `remove(key)`。项目目前用于学习，尚不提供 WAL 崩溃恢复保证。

---