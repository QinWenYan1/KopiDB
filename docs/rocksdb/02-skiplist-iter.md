# 📘 SkipList 迭代器：能力清单与 Prev 的代价（InlineSkipList::Iterator）

> RocksDB 源码精读 · 02 跳表·迭代器 | 源码版本 [`e6a2ee0`](https://github.com/facebook/rocksdb/tree/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7) | 对应 **Lab 1.2**：`SkipListIterator`（`operator++` / `operator==` / `operator*` / `is_end` / `is_valid`）

**从 01 走来**：[01-skiplist-basic](01-skiplist-basic.md) 把跳表的结构与读写（put/get/remove）拆完了。Lab 1.2 给跳表加光标——本篇讲 RocksDB 侧的唯一对应物 `InlineSkipList::Iterator`：

| 你的 lab 函数 | RocksDB 对应实现 | 本篇位置 |
|---|---|---|
| `operator++` / `is_end` | `Next()` / `Valid()` | 知识点 1 |
| `operator*` | `key()` | 知识点 1 |
| `is_valid` | `Valid()`（`node_ != nullptr`） | 知识点 1 |
| （lab 无反向遍历） | `Prev()` 为什么贵 | 知识点 2 |

---

## 🧠 核心概念总览

- [*知识点1: Iterator 的能力清单——前进免费*](#id1)
- [*知识点2: Prev 的真相——后退昂贵的根*](#id2)

---

<a id="id1"></a>
## ✅ 知识点 1: Iterator 的能力清单——前进免费

**在 [01](01-skiplist-basic.md) 那副只有前向指针的骨架上，"遍历"能做什么、要付什么代价？答案一句话：前进免费，后退昂贵。**

**能力清单**（定义 [:170-227](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L170-L227)）：

- `Next()`：`node_->Next(0)`，第 0 层走一步，O(1)（[:447-457](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L447-L457)）
- `Valid()`：`node_ != nullptr`——走完的标志就是光标落空
- `key()`：返回当前节点 key 区起点（就是 [01 §KP2](01-skiplist-basic.md#id2) 布局图里 header 后面那段字节）
- `Seek(target)`：包装 `FindGreaterOrEqual`（[:511-516](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L511-L516)）——正是 [01 §KP6](01-skiplist-basic.md#id6) 的查找
- `SeekToFirst/SeekToLast`：后者走 `FindLast` 逐层到底（[:545-556](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L545-L556)）

> 📍 **调用位置**：谁造这个迭代器？两条路——① `SkipListRep::Get` 点查时现场建一个再 `Seek`（[skiplistrep.cc:89](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L89)，即 [01 §KP6](01-skiplist-basic.md#id6) 📍 那条链）；② `SkipListRep::GetIterator`（[skiplistrep.cc:192](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L192)）把它包一层交给 MemTable 扫描用——这层包装是 Lab 2.2 的事，见 [06-memtable-iter](06-memtable-iter.md) §KP2。

---

<a id="id2"></a>
## ✅ 知识点 2: Prev 的真相——后退昂贵的根

**Prev 的实现**（[:482-491](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L482-L491)）：

```cpp
void Prev() {
  // Instead of using explicit "prev" links, we just search for the
  // last node that falls before key.
  node_ = list_->FindLessThan(node_->Key(), nullptr);   // 重新搜索，O(log N)
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}
```

注释说得很直白：**不存 prev 链接，退一格就重新查一次**。`SeekForPrev` = `Seek` + 必要时 `Prev` 回退（[:528-538](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L528-L538)）。

**这笔账值不值？** 存反向指针的代价：每节点每层多 8 字节——按平均 1.33 层算，每节点指针开销从 ≈10.7 字节涨到 ≈21.3 字节，**直接翻倍**，而这正是 [01 §KP2](01-skiplist-basic.md#id2) 的布局设计拼命省下来的东西。RocksDB 的取舍：让低频的 Prev 每次付 O(log N) 搜索费，保住高频路径的内存。

> 💡 **理解技巧**：设计取舍的一般形式——**给低频操作付时间，给高频路径省内存**。

---

## 📌 面试速记版

| 面试题 | 一句话答 |
|---|---|
| 跳表迭代器能做什么？ | `Next` O(1) 前进一步；`Seek` = FindGreaterOrEqual 定位；`SeekToFirst/Last` 到头尾；`Valid` = 光标非空 |
| 为什么 Prev() 是 O(log N)？ | 不存反向指针（存了每节点内存翻倍不划算），退一格 = 一次 FindLessThan 重搜 |
| 跳表迭代器为什么不为反向优化？ | 给低频操作付时间、给高频路径省内存——反向遍历在 RocksDB 里处处还债（归并层同样如此，见 [06 §KP3](06-memtable-iter.md#id3)） |

**记忆口诀**：**"前进 O1 后退搜，反向指针不保留。"**

---

**下一站**：单个光标会走了。Lab 1.3 要回答"扫一个前缀/一个区间，在哪停"——前缀提取与边界语义。→ [03-skiplist-scan](03-skiplist-scan.md)
