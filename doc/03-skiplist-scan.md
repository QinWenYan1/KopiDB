# 📘 SkipList 范围查询：前缀与边界的 RocksDB 对应（Prefix Seek & Bounds）

> RocksDB 源码精读 · 03 跳表·范围查询 | 源码版本 [`e6a2ee0`](https://github.com/facebook/rocksdb/tree/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7) | 对应 **Lab 1.3**：`begin_preffix` / `end_preffix` / `iters_monotony_predicate`

**从 02 走来**：[02-skiplist-iter](02-skiplist-iter.md) 的裸光标会 Seek 会 Next，但扫描的**边界问题**没答：只扫 `user:123:` 这一个前缀，扫到 `user:124:` 凭什么立刻停？本篇讲 RocksDB 侧的对应机制：

| 你的 lab 函数 | RocksDB 对应实现 | 本篇位置 |
|---|---|---|
| `begin_preffix(prefix)` | `SliceTransform` 定义"前缀" + 前缀模式开关 → `Seek(prefix)` 定位 | 知识点 1 |
| `end_preffix(prefix)` | 途中 `PrefixCheck` 越界即停 / `iterate_upper_bound` 硬边界 | 知识点 2-3 |
| `iters_monotony_predicate` | **无对应**——RocksDB 没有谓词下推 | 知识点 4 |

> 💡 **层次提醒**：RocksDB 的前缀/边界机制做在**引擎层**（ReadOptions + DBIter），跳表层只提供 Seek/Next 原料；lab 把边界判断直接做在跳表迭代器上，是教学简化。

---

## 🧠 核心概念总览

- [*知识点1: SliceTransform 与前缀扫描三模式——begin_preffix 对应物*](#id1)
- [*知识点2: 途中检查——prefix_ 记录与 PrefixCheck*](#id2)
- [*知识点3: iterate bounds——upper 开 lower 闭*](#id3)
- [*知识点4: 单调谓词无对应——RocksDB 没有谓词下推*](#id4)

---

<a id="id1"></a>
## ✅ 知识点 1: SliceTransform 与前缀扫描三模式——begin_preffix 对应物

**"前缀"不是引擎猜出来的，是用户通过 `SliceTransform` 定义的：`Transform` 负责切、`InDomain` 负责判资格；三个 ReadOptions 开关决定这次扫描对前缀优化信任到什么程度。**

**前缀提取器（SliceTransform）**，定义在 [slice_transform.h:34](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/slice_transform.h#L34)。两个核心方法：

- `Transform(key)`：从 key 切出前缀（[:51-56](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/slice_transform.h#L51-L56)）——比如按分隔符切出 `user:123:`
- `InDomain(key)`：判断这个 key"有没有资格谈前缀"（[:58-70](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/slice_transform.h#L58-L70)）——返回 false 的 key 不进 prefix bloom、不参与前缀判定（比如太短的 key 没有前缀可切）

两个常用工厂：`NewFixedPrefixTransform(n)`（[:121](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/slice_transform.h#L121)，定长前缀，短于 n 的 key 不在域）和 `NewCappedPrefixTransform(n)`（[:125](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/slice_transform.h#L125)，封顶前缀——前 min(len, n) 字节，所有 key 都在域）。

> ⚠️ **版本差异**：e6a2ee0 的 `SliceTransform` 已没有 `InRange()`（RocksDB 7.x 起移除）——读老博客见到它别去源码里找。

**三个模式开关**（`ReadOptions`，[options.h](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/options.h)），信任度从高到低：

| 开关 | 锚点 | 语义 |
|---|---|---|
| `prefix_same_as_start` | [:2366-2371](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/options.h#L2366-L2371) | 硬保证"只扫与 Seek 目标同前缀的 key"，Seek/SeekForPrev 都走前缀过滤 |
| `auto_prefix_mode` | [:2350-2364](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/options.h#L2350-L2364) | 默认全序，RocksDB 依 seek key 与 upper bound 自动判定能否安全启用前缀模式 |
| `total_order_seek` | [:2343-2348](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/options.h#L2343-L2348) | 全序扫描，放弃一切前缀优化（hash index 类表格式必须开） |

> 💡 **理解技巧**：开关决定的是"这次扫描配不配用前缀优化"。配用的时候，MemTable 迭代器入口处的 prefix bloom 拦截才会挂上——即 [06 §KP2](06-memtable-iter.md#id2) 讲的 `bloom_ = mem.bloom_filter_.get()` 三开关判定（[memtable.cc:521-531](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L521-L531)）：`prefix_same_as_start`，或 `!total_order_seek && !auto_prefix_mode`。入口拦截归 Lab 2.2 的篇目，本篇接下来的问题是：bloom 放行进来了，**途中**怎么保证不越界？

→ **下一站**：开关选定模式、Seek 落了地——扫起来之后每一步怎么盯着不越出前缀？知识点 2。

---

<a id="id2"></a>
## ✅ 知识点 2: 途中检查——prefix_ 记录与 PrefixCheck

**前缀扫描的终止语义 = Seek 时把目标前缀存进 `prefix_`，之后每步 Next/Prev 先做一次 PrefixCheck：`Transform(当前 key)` 不等于记下的前缀，立刻 `valid_ = false`——扫到第一个不同前缀的 key 就停。这正是 lab 里 `end_preffix` 要算的"终结位置"在 RocksDB 里的对应形态：不算出来，而是沿途盯出来。**

**第一步：Seek 时记下前缀**。若该走前缀模式（`ShouldSetPrefix`，[db_iter.h:606-611](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.h#L606-L611)——`prefix_same_as_start_` 为真且目标在域），DBIter 在 Seek 里把目标的前缀存起来（[db_iter.cc:2060-2063](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L2060-L2063)）：

```cpp
if (ShouldSetPrefix(target)) {
  prefix_.emplace();
  prefix_->SetUserKey(prefix_extractor_->Transform(target));
}
```

**第二步：每步移动前检查**。`PrefixCheck`（[db_iter.h:596-600](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.h#L596-L600)）：

```cpp
// 当前 key 切出的前缀 == Seek 时记下的前缀？
return !prefix_.has_value() ||
       (prefix_extractor_->InDomain(key) &&
        prefix_extractor_->Transform(key).compare(prefix_->GetUserKey()) == 0);
```

没记前缀（非前缀模式）则恒真；记了就必须逐 key 对得上。

**第三步：越出即停**。正向主循环里 `PrefixCheck` 失败且 `prefix_same_as_start_` → 直接 break（[db_iter.cc:535-547](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L535-L547)，落到 :763 `valid_ = false`）；反向对称置 invalid（[:1024-1034](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L1024-L1034)）：

```
sorted keys:

  user:123:a  user:123:b  user:123:c | user:124:a
                                     ^
        seek prefix = "user:123:"    | Transform("user:124:a") = "user:124:"
                                     -> PrefixCheck fails -> valid_ = false
```

> 💡 **理解技巧**：为什么需要途中检查，光靠入口 bloom 不够？bloom 是**概率型**过滤器（[06 §KP2](06-memtable-iter.md#id2) 📋）——它拦的是"前缀肯定不存在"的 seek；而一旦开扫，key 是一个个真实流出来的，越界的判定必须**精确**：逐 key 切前缀、逐个比对。一个管"要不要开始"，一个管"什么时候停"。

→ **下一站**：前缀是"按内容切"的边界；用户直接给定的"按区间切"的边界呢？知识点 3。

---

<a id="id3"></a>
## ✅ 知识点 3: iterate bounds——upper 开 lower 闭

**`ReadOptions::iterate_upper_bound / iterate_lower_bound` 给扫描画硬边界：upper 开区间（撞墙即停）、lower 闭区间（可以踩上去）；Seek 入口被钳进界内，途中 DBIter 逐 key 比较，下层自证界内时连比较都省。**

两个指针成员，方向感要记牢：

- `iterate_upper_bound`（[options.h:2317-2335](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/options.h#L2317-L2335)）：正向扫描的上限，**exclusive（开区间）**——bound 本身不是合法条目
- `iterate_lower_bound`（[options.h:2304-2315](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/include/rocksdb/options.h#L2304-L2315)）：反向扫描的下限，**inclusive（闭区间）**——bound 本身是合法条目

DBIter 构造时从 ReadOptions 抄入（[db_iter.cc:93-94](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L93-L94)；成员 [db_iter.h:690-691](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.h#L690-L691)），执行在四个点上：

1. **正向主循环开头**：`key >= upper` → break（[:527-533](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L527-L533)，落到 :763 `valid_ = false`）
2. **反向**：`saved_key < lower` → `valid_ = false`（[:1041-1051](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L1041-L1051)）
3. **入口钳制**：`SeekToFirst` 有 lower 直接 `Seek(lower)`（[:2205-2207](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L2205-L2207)）；`SeekToLast` 有 upper 改 `SeekForPrev(upper)`（[:2259-2261](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L2259-L2261)）——连起点都落在界内
4. **归并层分工**：MergingIterator 的 `iterate_upper_bound_` 只拦区间墓碑哨兵（[merging_iterator.cc:171-181](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/table/merging_iterator.cc#L171-L181)）；点 key 的边界明确划给 DBIter 与 SST 迭代器——成员注释原话（[:667-669](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/table/merging_iterator.cc#L667-L669)："For point keys, DBIter and SSTable iterator take care of boundary checking."）

> 💡 **理解技巧（门控）**：DBIter 的逐 key 比较有开关——`UpperBoundCheckResult() != kInbound` / `MayBeOutOfLowerBound()`（[:527-529](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L527-L529)、[:1041](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_iter.cc#L1041)）。下层迭代器自证"还在界内"时直接跳过比较，只有逼近边界才较真。**热路径零成本，边界处才付费。**

全景图（两道边界与正向扫描）：

```
key space (sorted):

   z1 z2 | a1 a2 a3 ... a9 | b1 b2
         ^                 ^
   lower (inclusive)   upper (exclusive)
         |<-- valid zone -->|

forward scan:
  SeekToFirst            -> Seek(lower)         (clamped)
  key >= upper           -> valid_ = false      (hard stop)

prefix mode (seek prefix = "a", prefix_same_as_start):
  a1 a2 ... a9 | b1
               ^ Transform(b1) = "b" != "a"
               -> PrefixCheck fails -> valid_ = false
```

> 📋 **术语提醒**：`upper 开 / lower 闭` 的记忆法——上界是"**围墙**"（撞墙即停，墙上的砖不算），下界是"**地板**"（可以踩上去）。
> ⚠️ **关键区分**：前缀边界（知识点 2）是**内容语义**——"同一前缀的 key"；iterate bounds 是**位置语义**——"key 的数值区间"。两者可以叠加：前缀扫描再套一个 upper bound，谁先触发谁停。

→ **下一站**：前缀与边界都有对应物了。lab 的谓词查询呢？知识点 4。

---

<a id="id4"></a>
## ✅ 知识点 4: 单调谓词无对应——RocksDB 没有谓词下推

**Lab 1.3 的 `iters_monotony_predicate`（给一个单调谓词，返回满足它的连续区间）在 RocksDB 里没有对应物——RocksDB 的扫描路径上没有任何"用户谓词下推到存储层"的机制。**

- RocksDB 扫描能用的边界工具只有本篇这两件：**前缀**（内容语义）与 **iterate bounds**（位置语义），其余过滤全部发生在迭代器把 key/value 吐出来之后（用户侧自己判）
- 为什么不下推？谓词是任意用户逻辑，存储层无法假设它的性质；而 lab 的谓词带**单调性**前提——单调 ⇒ 满足集是连续区间 ⇒ 二分定位两端即可，这是教学场景才能享受的红利
- 面试里被问"RocksDB 怎么做谓词下推"，正确答案是"不做；它把边界表达力收敛到 prefix extractor 与 iterate bounds 两个可静态验证的机制上"

> 💡 **理解技巧**：对比着记——lab 的 `iters_monotony_predicate` 用"单调"换"一次定位两端"；RocksDB 用"机制收窄"换"任何扫描都安全"。

---

## 📌 面试速记版

| 面试题 | 一句话答 |
|---|---|
| RocksDB 的"前缀"怎么定义？ | 用户给 `SliceTransform`：`Transform` 切前缀、`InDomain` 判资格（不在域的 key 不进 bloom 不参与判定）；工厂有定长 `NewFixedPrefixTransform` 与封顶 `NewCappedPrefixTransform`（e6a2ee0 已无 `InRange`） |
| 前缀扫描三个模式？ | `prefix_same_as_start` 硬保证只扫同前缀；`auto_prefix_mode` 由 RocksDB 依 seek key 与 upper bound 自动判定；`total_order_seek` 全序、放弃一切前缀优化 |
| 前缀扫描怎么停下来？ | Seek 时把目标前缀存进 `prefix_`，之后每步 PrefixCheck：`Transform(key)` 不等于记下前缀就 `valid_ = false`；入口另有 prefix bloom 拦"前缀不存在"的 seek（[06 §KP2](06-memtable-iter.md#id2)）——一个管开始，一个管停 |
| iterate bounds 的开闭？ | `iterate_upper_bound` 开区间（撞墙即停），`iterate_lower_bound` 闭区间（可踩）；`SeekToFirst/SeekToLast` 先被钳进界内 |
| 边界检查的性能怎么保？ | 下层迭代器自证界内（`UpperBoundCheckResult`/`MayBeOutOfLowerBound`）时跳过逐 key 比较；归并层只拿 upper 拦区间墓碑，点 key 边界归 DBIter 与 SST 迭代器 |
| RocksDB 有谓词下推吗？ | 没有——边界表达力只有 prefix extractor 与 iterate bounds 两件，其余过滤在迭代器吐数之后由用户侧做 |

**记忆口诀**：**"Transform 切前缀，InDomain 判域；Seek 记 prefix，越界即止步；upper 围墙 lower 地板，界内自证省比较；谓词从不下推，边界只有两招。"**

---

**下一站**：跳表三件套（读写/迭代器/范围查询）拆完。进 Lab 2 之前，建议先过一眼引擎层的世界观 [04-data-path](04-data-path.md)（Put/Get 工作流）；然后进 MemTable 本体 → [05-memtable-rw](05-memtable-rw.md)（Lab 2.1）
