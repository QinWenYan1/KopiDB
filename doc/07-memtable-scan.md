# 📘 MemTable 前缀扫描：iters_preffix 的 RocksDB 对应（Prefix Scan at MemTable Level）

> RocksDB 源码精读 · 07 MemTable·前缀扫描 | 源码版本 [`e6a2ee0`](https://github.com/facebook/rocksdb/tree/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7) | 对应 **Lab 2.3**：`MemTable::iters_preffix` / `iters_monotony_predicate`

**从 06 走来**：[06-memtable-iter](06-memtable-iter.md) 把"全量归并迭代器"拆完了。Lab 2.3 问的是：只扫一个前缀时，MemTable 层能省什么？

| 你的 lab 函数 | RocksDB 对应实现 | 本篇位置 |
|---|---|---|
| `iters_preffix(prefix)` | 每张表 `Seek(prefix)` + memtable 层 prefix bloom 拦截 | 知识点 1-2 |
| `iters_monotony_predicate` | **无对应**（同 [03 §KP4](03-skiplist-scan.md#id4)，RocksDB 无谓词下推） | 知识点 3 |

---

## 🧠 核心概念总览

- [*知识点1: 前缀扫描在 MemTable 层的落点*](#id1)
- [*知识点2: memtable 的 prefix bloom 什么时候建、什么时候问*](#id2)
- [*知识点3: 单调谓词——无对应*](#id3)

---

<a id="id1"></a>
## ✅ 知识点 1: 前缀扫描在 MemTable 层的落点

**前缀扫描 = 前缀 seek 的特殊化；MemTable 层要做的只有两件事：每张表各 Seek 一次定位起点，入口处用 bloom 挡掉"肯定没有"的表。**

拆给已知零件：

- **"前缀"是谁定义的**：`SliceTransform`（Transform 切 / InDomain 判）——见 [03 §KP1](03-skiplist-scan.md#id1)
- **起点怎么定**：上层编好 internal key 形式的 seek 目标，`MemTableIterator` 透传进跳表 Seek（[06 §KP2](06-memtable-iter.md#id2) 的编码分工）；active 与每张 imm 各 Seek 一次，归并器（[06 §KP3](06-memtable-iter.md#id3)）把多路流揉成一条
- **终点怎么停**：途中逐 key `PrefixCheck`，`Transform(key)` 不等于目标前缀即 `valid_ = false`——见 [03 §KP2](03-skiplist-scan.md#id2)
- **MemTable 层独有的增量**：入口 bloom 拦截，知识点 2

> 💡 **理解技巧**：把 lab 的 `iters_preffix` 映射过来——`begin_preffix` ≈ 每表 Seek(prefix)（[03 §KP1](03-skiplist-scan.md#id1) 的入口），`end_preffix` ≈ PrefixCheck / upper bound（[03 §KP2-3](03-skiplist-scan.md#id2)）。RocksDB 把这些拆在引擎层，lab 收拢在 MemTable 一处，语义相同。

---

<a id="id2"></a>
## ✅ 知识点 2: memtable 的 prefix bloom——什么时候建、什么时候问

**MemTable 在写入时就按前缀喂 bloom；扫描 Seek 时先问过滤器，"肯定没有"则整张表一趟跳表都不用跑。**

- **建**：配了前缀提取器时，`MemTable::Add` 顺手把前缀喂进 `bloom_filter_`（[memtable.cc:1199-1201](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L1199-L1201)：`bloom_filter_->Add(prefix_extractor_->Transform(key))`）——**写入的边际成本 = 一次 Transform + 一次 bloom Add**
- **问（点查）**：`MemTable::Get` 第 3 关的 `MayContain`（[memtable.cc:1611-1616](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L1611-L1616)，[05 §KP7](05-memtable-rw.md#id7)）
- **问（扫描）**：`MemTableIterator` 构造时按三开关挂 `bloom_`，每次 Seek 先 `MayContain(prefix)`，不含有直接置 invalid（[06 §KP2](06-memtable-iter.md#id2)，[memtable.cc:573-586](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L573-L586)）

```
前缀扫描 user:123:* 进 MemTable 层：

  每张表:   bloom.MayContain("user:123:")?
              │ 含            │ 肯定不含
              ▼              ▼
         Seek 定位起点      整表跳过（一次跳表都不用跑）
              │
              ▼
        归并器揉成一条流 → 途中 PrefixCheck 盯终点
```

> ⚠️ **关键区分**：memtable bloom 省的是**进跳表 Seek 的 CPU**，不是 IO——memtable 本来就在内存里。真正省 IO 的 bloom 在 SST 层（Lab 3）。

---

<a id="id3"></a>
## ✅ 知识点 3: 单调谓词——无对应

**`iters_monotony_predicate` 在 RocksDB 里没有对应物，原因与跳表层相同：RocksDB 不做谓词下推。**

扫描路径上可用的边界工具只有 prefix extractor 与 iterate bounds 两件（[03 §KP4](03-skiplist-scan.md#id4) 已论证）；MemTable 层不新增任何谓词机制。lab 利用"单调 ⇒ 满足集连续 ⇒ 定位两端"是教学设计。

---

## 📌 面试速记版

| 面试题 | 一句话答 |
|---|---|
| 前缀扫描在 memtable 层做什么？ | 每张表 Seek(prefix) 定位起点 + 入口 prefix bloom 挡"肯定没有"的表；终点由引擎层 PrefixCheck 盯 |
| memtable bloom 什么时候建？ | 写入时顺手建：Add 里 `Transform(key)` 喂 bloom，边际成本一次哈希 |
| memtable bloom 省什么？ | 省进跳表 Seek 的 CPU，不省 IO（memtable 本来就在内存）；省 IO 的 bloom 在 SST 层 |

**记忆口诀**：**"写入顺手喂 bloom，扫描门口先一问；起点 Seek 终点盯，无谓词来不下推。"**

---

**下一站**：MemTable 三件套（读写/迭代器/前缀）拆完。再往后是落盘的世界——Block/SST 格式（Lab 3，待写）。回看全景：[04-data-path](04-data-path.md)
