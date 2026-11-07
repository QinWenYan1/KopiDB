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
- [🧵 并发与事务](#concurrency-and-transactions)
- [💻 C++ 使用示例](#cpp-example)
- [🧭 如何选择读写方式](#choosing-read-write-mode)



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

<a id="concurrency-and-transactions"></a>

## 🧵 并发与事务

KopiDB 的并发设计目标是：**同一进程内，多个线程共享同一个数据库实例**，完整并发支持仍在完善中。

每次 `begin_tran()` 创建独立的 `TranContext`，对应一笔事务；同一事务上下文和同一迭代器应串行使用，跨线程交接需由调用方同步。

普通 `put/get/remove` 无需显式开启事务；需要将多次操作组成一笔事务时，再使用 `begin_tran()` 和 `commit/abort`。

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

<a id="choosing-read-write-mode"></a>

## 🧭 如何选择读写方式

独立读写可以直接调用 `db.put/get/remove`。需要把多次操作组成一笔事务，统一提交或回滚时，使用 `begin_tran()` 并选择隔离级别。下表描述 KopiDB 当前实现的行为。

| 方式 | 读取表现 | 写入与结束方式 | 事务版本号如何分配 | 查询版本上限 | 允许读取其他事务未提交的修改吗 | 已回滚记录 |
| --- | --- | --- | --- | --- | --- | --- |
| **直接操作** | 每次独立查询，结果可能变化 | 每次操作独立执行，无需手动提交，没有跨调用的统一回滚 | 每次 `put()` 分配新的版本号 | 每次查询新分配的版本号 | 当前允许 | 排除 |
| **读未提交** `READ_UNOP_COMMITTED` | 每次重新查询，结果可能变化；读到的修改之后可能被回滚 | 写入立即进入数据库；通过 `commit()` 结束事务，或通过 `abort()` 取消修改 | 同一事务的写入共用一个版本号 | `0`，不限制版本号大小 | 允许 | 排除 |
| **读已提交** `READ_OP_COMMITTED` | 自身修改优先；其余只读已提交数据，可以看到后来提交的更新 | 修改先暂存，提交成功后应用；支持回滚 | 同一事务的写入共用一个版本号 | `0`，不限制版本号大小 | 不允许 | 排除 |
| **可重复读** `REPEATABLE_READ` | 同一个 key 保持首次合法结果，包括“不存在”；自身修改优先 | 修改先暂存，提交时检查写入冲突；支持回滚 | 同一事务的写入共用一个版本号 | 首次查询使用本事务的版本号，之后复用缓存 | 不允许 | 排除 |
| **可串行化** `SERIALIZABLE` **（实验性，未完整实现）** | 当前沿用可重复读的单键读取行为 | 修改先暂存；提交时额外验证此前读过的数据是否变化；支持回滚 | 同一事务的写入共用一个版本号 | 首次查询使用本事务的版本号，之后复用缓存 | 不允许 | 排除 |

补充说明：

- **版本号与读取上限不同**：显式事务的版本号在创建时分配，不代表提交顺序。查询传 `0` 只是取消版本上限，仍需比较记录的真实版本并检查回滚状态。表中的版本上限用于查询存储层；自身暂存修改和已有读缓存优先处理。
- **回滚检查始终生效**：内部的 `is_aborted(id)` 判断记录所属事务是否已经回滚。`require_committed = true` 会进一步排除活跃事务的记录；`false` 不表示关闭回滚检查。这里的“未提交”指其他事务，暂存型事务仍可以读取自己的修改。
- **批量接口**：普通 `put_batch()` 整批共用一个新版本号；普通 `get_batch()` 尚无 `require_committed` 参数，当前同样只过滤回滚版本。
- **保证范围**：当前可重复读不承诺跨所有 key 的一致快照。完整可串行化要求成功提交事务的读写效果等价于某种逐笔执行的顺序，不要求线程实际排队；KopiDB 当前尚不提供这一完整保证，完整实现留作后续扩展。
