# 📘 MemTable 迭代器与归并：begin/end 的 RocksDB 对应实现（MemTableIterator & MergingIterator）

> RocksDB 源码精读 · 06 MemTable·迭代器 | 源码版本 [`e6a2ee0`](https://github.com/facebook/rocksdb/tree/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7) | 对应 **Lab 2.2**：`MemTable::begin(tranc_id)` / `end()`（返回归并 current + frozen 的 `HeapIterator`）

**从 05 走来**：[05-memtable-rw](05-memtable-rw.md) 拆完了单表的读写与冻结。Lab 2.2 要给整个 MemTable（current + 所有 frozen）造一个归并迭代器——本篇讲 RocksDB 侧的对应实现：

| 你的 lab 函数 | RocksDB 对应实现 | 本篇位置 |
|---|---|---|
| `begin()` 单表部分 | `MemTable::NewIterator` → `MemTableIterator` | 知识点 1-2 |
| `begin()` 归并多表部分（`HeapIterator`） | `MemTableListVersion::AddIterators` + `MergingIterator` | 知识点 1、3 |
| `end()` / 迭代终止 | `Valid()` = 堆空 | 知识点 3 |

---

## 🧠 核心概念总览

- [*知识点1: 迭代器契约——MemTable 层给上层什么*](#id1)
- [*知识点2: MemTableIterator——memtable 的第一层包装*](#id2)
- [*知识点3: MergingIterator——N 路归并与方向切换的代价*](#id3)

---

<a id="id1"></a>
## ✅ 知识点 1: 迭代器契约——MemTable 层给上层什么

**MemTable 对扫描只承诺一件事：按 InternalKeyComparator 序吐出全部条目（含所有版本与墓碑），其余语义都是上层的。**

- **接口背景**：MemTable 对上层吐数据走 `InternalIterator` 抽象（[internal_iterator.h:24-215](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/table/internal_iterator.h#L24-L215)）——所有数据源（memtable、SST、归并器）统一实现的内部迭代器接口，搬运的是带 seq/type 尾巴的 internal key 字节流
- **造迭代器**：`MemTable::NewIterator`（[memtable.cc:742-751](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L742-L751)）在 arena 里 placement-new 一个 `MemTableIterator`——包装与解码细节见本篇知识点 2
- **imm 列表同等待遇**：`MemTableListVersion::AddIterators`（[memtable_list.cc:289](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable_list.cc#L289)）给每张 imm 各造一个迭代器，和 active 的一起交给归并器（本篇知识点 3）——这正是 lab 里 `HeapIterator` 归并 current + frozen 的对应物
- **吐出的形态**：编码后的 internal key 字节流（[05 §KP3](05-memtable-rw.md#id3) 的格式），**不做**版本去重、**不消化**墓碑——那是引擎层 DBIter 的活（本系列不展开）
- **边界语义**：前缀提前止损靠入口 bloom（本篇知识点 2），区间截断靠上层谓词——MemTable 层只管"有序吐全量"

> 🔄 **知识关联**：点查与扫描在 MemTable 层共用同一条 Seek 通道（[01-skiplist-basic](01-skiplist-basic.md) §KP6 📍）——Get 是"Seek 一次 + 逐版本回调"，扫描是"Seek 定位 + 一路 Next"，底层都是跳表迭代器。

---

<a id="id2"></a>
## ✅ 知识点 2: MemTableIterator——memtable 的第一层包装

**MemTableIterator 包的不是跳表裸迭代器，而是 `MemTableRep::Iterator`；它在裸光标上补三件 memtable 级的能力：prefix bloom 提前止损、varint32 长度前缀解码、Seek 的编码分工。**

[05 §KP6](05-memtable-rw.md#id6) 讲过数据进跳表时的节点格式：`[varint32 key 长度][internal key][varint32 value 长度][value]`。读出来就得按同一格式解——[02 §KP1](02-skiplist-iter.md#id1) 的裸迭代器只会返回整段 buffer 起点，解格式是包装层的事。

**包装关系**。`MemTableIterator : public InternalIterator`（[memtable.cc:493-740](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L493-L740)），成员是抽象的 `MemTableRep::Iterator* iter_`（[:716](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L716)）。构造函数三分支（[:519-535](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L519-L535)）：区间墓碑表走 `range_del_table_->GetIterator`；配了前缀提取器且 ReadOptions 允许时走 `GetDynamicPrefixIterator`；默认走 `table_->GetIterator`。对默认跳表 rep，拿到的是 `SkipListRep::Iterator`（[skiplistrep.cc:192](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L192)）——它里面才包着 [02-skiplist-iter](02-skiplist-iter.md) 的 `InlineSkipList::Iterator`（[inlineskiplist.h:170-227](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L170-L227)）。

> 📍 **调用位置**：谁 new 出 `MemTableIterator`？`MemTable::NewIterator`（本篇知识点 1）；引擎扫描时由 `super_version->mem->NewIterator(...)`（[db_impl.cc:2627](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/db_impl/db_impl.cc#L2627)）触发，imm 侧对应 `AddIterators`。

**Seek 的编码分工**（与 [05 §KP7](05-memtable-rw.md#id7) 对照着看）。点查 Get 是调用方编好 LookupKey 直接进跳表；迭代器 Seek 收到的 `k` 已经是上层编好的 internal key，MemTableIterator **原样透传**（[:592](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L592) `iter_->Seek(k, nullptr)`），到 rep 层才由 `EncodeKey` 补上 varint32 长度前缀（[skiplistrep.cc:228-234](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L228-L234)，EncodeKey 本体 [memtable.cc:486-491](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L486-L491)）：

```cpp
const char* EncodeKey(std::string* scratch, const Slice& target) {
  scratch->clear();
  PutVarint32(scratch, static_cast<uint32_t>(target.size()));
  scratch->append(target.data(), target.size());
  return scratch->data();
}
```

> 💡 **理解技巧**：点查与迭代器殊途同归——LookupKey 一步到位编出"长度前缀 + internal key"，迭代器把同一动作拆给两层（上层编 internal key、rep 层补长度前缀）。能殊途同归的原因：**跳表节点里躺着的格式只有一个**（[05 §KP3](05-memtable-rw.md#id3)）。

**prefix bloom 提前止损**（[:573-586](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L573-L586)）。构造时若满足三开关（[:521-527](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L521-L527)：`prefix_same_as_start`，或 `!total_order_seek && !auto_prefix_mode`——即"本次扫描限定在同一前缀内"，或"非全序扫描且非自动前缀模式"）就把 `mem.bloom_filter_` 挂到 `bloom_`；之后每次 Seek 先问过滤器：

```cpp
if (bloom_) {
  // iterator should only use prefix bloom filter
  Slice user_k_without_ts(ExtractUserKeyAndStripTimestamp(k, ts_sz_));
  if (prefix_extractor_->InDomain(user_k_without_ts)) {
    Slice prefix = prefix_extractor_->Transform(user_k_without_ts);
    if (!bloom_->MayContain(prefix)) {
      valid_ = false;
      return;          // prefix 不可能有，跳表一趟都不用跑
    }
```

> 📋 **术语提醒**：`bloom filter（布隆过滤器）` = 概率型存在性过滤器：它说"没有"就一定没有，说"有"可能只是误报；`prefix extractor（前缀提取器）` = 从 key 中切出前缀的函数（如按 `user:123:` 切段），让"同前缀的 key"共享一次过滤判断。

**解码**：节点 buffer 的格式决定了解码就是两次长度前缀切片（[:675-698](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L675-L698)，`GetLengthPrefixedSlice` 在 [util/coding.h:320](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/util/coding.h#L320)）：

```cpp
Slice key() const override {
  return GetLengthPrefixedSlice(iter_->key());                   // 切出 internal key
}
Slice value() const override {
  Slice key_slice = GetLengthPrefixedSlice(iter_->key());
  return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());  // 跳过 key 再切 value
}
```

**Prev 直通**（[:664-674](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L664-L674)）：`iter_->Prev()` 一路透传到跳表——代价即 [02 §KP2](02-skiplist-iter.md#id2) 的 FindLessThan 重搜，包装层不增加也不减少这笔账。

> 💡 **理解技巧**：`IsKeyPinned()` 恒 true（[:702-705](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L702-L705)，注释 "memtable data is always pinned"）——[01 §KP3](01-skiplist-basic.md#id3) 的 Arena 只推指针不回收，节点的 key 天然钉得住（pinned = 迭代器存活期内底层内存保证不动，`key()` 返回的 Slice 可以长期用）。

→ **下一站**：一张表能迭代了。但 MemTable 是"1 个 active + 一串 imm"——多条有序流怎么归并成一条？知识点 3。

---

<a id="id3"></a>
## ✅ 知识点 3: MergingIterator——N 路归并与方向切换的代价

**N 个各自有序的迭代器归并成一条流 = 一个堆：正向用最小堆、反向用懒建的最大堆；前进几乎零成本，代价集中在换方向——每个子迭代器都要重新 Seek。lab 的 `HeapIterator` 就是这套思路的教学版。**

k 路归并（k-way merge）是归并排序的标准零件：N 路输入各自有序，每轮取当前最小者输出；堆把每轮代价压到 O(log N)。

**数据结构**（[merging_iterator.cc:648-664](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/table/merging_iterator.cc#L648-L664)）：

```cpp
// Invariant: at the end of each InternalIterator API,
// current_ points to minHeap_.top().iter (maxHeap_ if backward scanning)
IteratorWrapper* current_;
...
MergerMinIterHeap minHeap_;   // 正向

// Max heap is used for reverse iteration, which is way less common than
// forward. Lazily initialize it to save memory.
std::unique_ptr<MergerMaxIterHeap> maxHeap_;   // 反向，懒建
```

两个不变式值得记住：**① 每个 API 结束时 `current_` 指向堆顶**（用户看到的 key 就是堆顶的 key）；**② 最大堆懒建**——反向遍历少见，不建省内存（💡 又是"低频操作后付费"）。

> 📋 **术语提醒**：`IteratorWrapper` = 给子迭代器包一层的外壳，缓存 key()/Valid() 等状态，避免归并时反复虚函数调用。

**Next 的账本**（[:349-385](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/table/merging_iterator.cc#L349-L385)）：

```cpp
void Next() override {
  if (direction_ != kForward) {
    SwitchToForward();          // 方向不对，先花大钱换方向
  }
  current_->Next();             // 堆顶孩子自己前进一步
  if (current_->Valid()) {
    minHeap_.replace_top(minHeap_.top());   // 堆顶下沉，恢复堆序
  } else {
    minHeap_.pop();             // 这孩子耗尽，出堆
  }
  ...
}
```

`replace_top` 处的注释点破了为什么便宜（[:367-369](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/table/merging_iterator.cc#L367-L369)）：**同一个孩子连续出 key 时，堆顶原地调整即可**——扫描局部性越强，归并越近乎白送。

直观图（3 个孩子，各自有序）：

```
memtable : a2 c1 e9
imm      : a1 b7 e3
L0 sst   : b2 c0 d4

min-heap of current heads:

        a1 (imm)
        /      \
   a2 (mem)   b2 (sst)

Next() => a1 输出, imm 前进一步到 b7, replace_top 重整堆序
```

**换方向的代价**。正向转反向（`SwitchToBackward`，[:1389-1442](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/table/merging_iterator.cc#L1389-L1442)）或反向转正向（`SwitchToForward`，[:1321-1385](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/table/merging_iterator.cc#L1321-L1385)）做的是同一件事：清堆 → 以当前 key 为目标，对**每个**孩子重新 `Seek`/`SeekForPrev` → 重建堆。N 个孩子 = N 次 seek，外加堆重建。

> 💡 **理解技巧**：把两层"反向税"串起来——跳表层 Prev = FindLessThan 重搜（[02 §KP2](02-skiplist-iter.md#id2)）；归并层换方向 = N 路全量重 seek（本篇）。**RocksDB 的迭代器为正向扫描优化，反向每一步都在还债。**

> 📋 **术语提醒**：`range tombstone（区间墓碑）` = 一次删除 `[start, end)` 整片 key 的标记（`DeleteRange`）。MergingIterator 把它的起止哨兵也放进堆里，被区间盖住的 key 直接跳过——细节归 compaction 篇，这里只需知道归并器不只归并点 key。

---

## 📌 面试速记版

| 面试题 | 一句话答 |
|---|---|
| MemTable 的迭代器吐出什么？ | 按 InternalKeyComparator 序的全部条目（含所有版本与墓碑）的编码字节流；版本去重与墓碑消化是引擎层的活 |
| MemTableIterator 比裸跳表迭代器多了什么？ | 三件套：prefix bloom 提前止损（prefix 不在域直接置 invalid）、varint32 长度前缀解码、Seek 编码分工（上层编 internal key、rep 层 EncodeKey 补长度前缀） |
| 多表（active + imm）怎么归并？ | 每表一个迭代器，MergingIterator 正向最小堆归并；不变式：API 结束时 current_ = 堆顶 |
| 归并的 Next 为什么便宜？ | 堆顶孩子走一步 + replace_top 原地调整；同一孩子连续出 key 时近乎白送（扫描局部性） |
| 反向遍历为什么贵？ | 两层反向税：跳表 Prev = FindLessThan 重搜；归并换方向 = N 路全量重 seek 重建堆 |
| key()/value() 的 Slice 能存着慢慢用吗？ | 默认下次移动即失效；memtable 恒 pinned（Arena 不回收），可以长期用 |

**记忆口诀**：**"一表一迭代，堆里比堆顶；前进白送换向贵，前缀 bloom 门口醒。"**

---

**下一站**：归并迭代器会走了。Lab 2.3 要回答"扫一个前缀时 memtable 层能省什么"——前缀 bloom 与 memtable 层的前缀落点。→ [07-memtable-scan](07-memtable-scan.md)
