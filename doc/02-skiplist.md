# 📘 SkipList：Node 内存布局与无锁并发（Node Layout & Lock-free Concurrency）

> RocksDB 源码精读 · 02 跳表 | 源码版本 [`e6a2ee0`](https://github.com/facebook/rocksdb/tree/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7) | 本篇涵盖：Node 倒装内存布局、Arena 分配器、随机层高、head_ 设计、无锁读、CAS 无锁写、内存序、迭代器能力边界
>
> 前置：[01-data-path](01-data-path.md)——Put/Get 工作流与 InternalKey 编码

---

## 🧠 核心概念总览

- [*知识点1: Node 的倒装内存布局*](#id1)
- [*知识点2: Arena 分配器*](#id2)
- [*知识点3: RandomHeight 与层高参数*](#id3)
- [*知识点4: head_ 与 max_height_ 的松弛设计*](#id4)
- [*知识点5: 无锁读：FindGreaterOrEqual 与 Contains*](#id5)
- [*知识点6: 无锁写：Splice 与 CAS 链接*](#id6)
- [*知识点7: 内存序四件套——无锁正确的根*](#id7)
- [*知识点8: Iterator 的能力与代价*](#id8)

---

<a id="id1"></a>
## ✅ 知识点 1: Node 的倒装内存布局

**Node 不是传统 struct，而是"指向自定义内存的指针"：高层指针存在 header 前面，key 存在后面。**

源码原话（[inlineskiplist.h:352-356](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L352-L356)）："The Node data type is more of a pointer into custom-managed memory than a traditional C++ struct."

**内存布局图**（一个 height=3 的节点）：

```
低地址                                                          高地址
┌───────────────────────────┬────────────────┬──────────────────┐
│ next_[2]      next_[1]     │ Node header    │ key 字节串        │
│ （高层指针，负索引访问）    │ （next_[0]）   │                  │
└───────────────────────────┴────────────────┴──────────────────┘
                            ↑ Node* 指向这里
```

- 类里只声明了 `Atomic<Node*> next_[1]`（[:417-421](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L417-L421)）——**高层指针通过负索引访问**：level n 的指针在 `(&next_[0] - n)`
- `Key()` 直接返回 `&next_[1]` 的地址（[:374](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L374)）——key 紧贴 header，零偏移计算

**分配时的精确计算**（`AllocateNode`，[:858-880](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L858-L880)）：

```cpp
auto prefix = sizeof(Atomic<Node*>) * (height - 1);
char* raw = allocator_->AllocateAligned(prefix + sizeof(Node) + key_size);
Node* x = reinterpret_cast<Node*>(raw + prefix);
x->StashHeight(height);   // 借用 next_[0] 临时存身高
```

- 一次分配 = `(身高-1)个指针 + Node header + key`，Node 落在 `raw + prefix`
- **StashHeight**：key 写入后、插入跳表前，身高临时藏在 `next_[0]` 的存储位里（[:358-372](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L358-L372)）；插入时 `UnstashHeight` 取出
- 插入时从 key 反推 Node：`(Node*)key - 1`（Insert 开头 [:1030](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1030)）

> 💡 **理解技巧**：为什么搞这么绕？源码注释自己回答了（[:871-874](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L871-L874)）：节点链入跳表后**根本不需要存身高**（你从第 h 层走进这个节点，h 就必然是合法层）——省掉每节点的 height 字段和指针数组的头部开销。
> ⚠️ **关键区分**：StashHeight 只是"传递中的便签"，`SetNext` 之后原值即失效（注释原话 "Undefined after a call to SetNext"）。

→ **下一站**：节点从哪儿分配？为什么不直接 `new`？知识点 2。

<a id="id2"></a>
## ✅ 知识点 2: Arena 分配器

**跳表节点不单独 new，全部从 Arena 的大块里"推指针"扣出来。**

**分配就是推指针**（`Arena::AllocateAligned`，[arena.cc:108-143](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.cc#L108-L143)）：

```cpp
// 算对齐补齐量，然后直接从当前块里扣
size_t slop = (current_mod == 0 ? 0 : kAlignUnit - current_mod);
if (needed <= alloc_bytes_remaining_) {
  result = aligned_alloc_ptr_ + slop;
  aligned_alloc_ptr_ += needed;        // 推指针，完事
  alloc_bytes_remaining_ -= needed;
} else {
  result = AllocateFallback(bytes, true);  // 当前块不够，开新块
}
```

**块管理参数**（[arena.h:31-37](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.h#L31-L37)）：

- 内联块 2KB（`kInlineSize`，小 MemTable 零堆分配）→ 常规块 4KB 起（`kMinBlockSize`）→ 上限 2GB
- 新块用 `new char[block_bytes]` 并记录 `malloc_usable_size` 精确记账（[arena.cc:145](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.cc#L145) 起）
- **无单节点释放**：MemTable 销毁时整个 Arena 一次性释放

**内存统计因此简化**：`ApproximateMemoryUsage() = 已分配块总量 − 当前块剩余`（[arena.h:66-69](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.h#L66-L69)）——`SkipListRep::ApproximateMemoryUsage` 直接返回 0，因为账全在 Arena 这里（[skiplistrep.cc:80-83](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L80-L83)）

> 💡 **理解技巧**：跳表节点身高随机 → 节点大小不均 → 逐节点 new 必碎片化；Arena 顺序分配既无碎片又缓存友好，还顺手解决了并发读的"悬垂指针"问题（知识点 7）。
> 🔄 **知识关联**：并发写场景 MemTable 会用 `ConcurrentArena`（定义在 [concurrent_arena.h:42](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/concurrent_arena.h#L42)），选择逻辑列入待验证点。

→ **下一站**：节点存进了 Arena，但每个节点的身高是怎么随机出来的？知识点 3。

<a id="id3"></a>
## ✅ 知识点 3: RandomHeight 与层高参数

**"抛硬币"的实现是拿随机数和一个预计算阈值比大小——没有取模，没有浮点。**

`RandomHeight()`（[inlineskiplist.h:559-573](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L559-L573)）：

```cpp
int height = 1;
while (height < kMaxHeight_ && height < kMaxPossibleHeight &&
       rnd->Next() < kScaledInverseBranching_) {
  height++;
}
```

- `kScaledInverseBranching_` 在构造时预计算：`(Random::kMaxNext + 1) / kBranching_`（[:837](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L837)）——随机数落在低 1/4 区间就晋级，一次比较完成一次"抛硬币"
- **两个层上限**：`kMaxHeight_ = 12`（这张表的软上限）vs `kMaxPossibleHeight = 32`（硬上限，栈上数组的维度，如 `InsertConcurrently` 的 `Node* prev[kMaxPossibleHeight]` [:914](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L914)）
- 期望层数：$E[height] = \frac{1}{1-p} = \frac{4}{3}$（$p=1/4$），平均每节点只占 1.33 层指针

> 📋 **术语提醒**：`kScaledInverseBranching_` 把"概率 1/4"换算成"32 位随机数的阈值"，是分支因子到比较域的缩放。
> ⚠️ **关键区分**：`max_height_`（当前表高）和 `kMaxHeight_`（允许上限）是两回事——前者动态增长，下个知识点讲。

→ **下一站**：表刚建出来时最高层是几？表高怎么随插入增长？知识点 4。

<a id="id4"></a>
## ✅ 知识点 4: head_ 与 max_height_ 的松弛设计

**头节点一次给满 12 层；表高从 1 起步，靠 relaxed CAS 懒增长——而且"读过头"无害。**

**构造**（[:831-851](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L831-L851)）：

```cpp
head_(AllocateNode(0, max_height)),   // head 直接给满 max_height 层
max_height_(1),                        // 但表高从 1 起步
```

**表高增长**（插入时，[:1035-1044](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1035-L1044)）：

```cpp
int max_height = max_height_.LoadRelaxed();
while (height > max_height) {
  if (max_height_.CasWeakRelaxed(max_height, height)) {
    max_height = height;
    break;
  }   // CAS 失败说明别人先长高了，重试或退出都行
}
```

**为什么 relaxed 内存序就够**？成员声明处的注释直接回答了（[:239-242](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L239-L242)）：

> "Relaxed reads are always OK because starting from higher levels only helps efficiency, not correctness."

——从比实际更高的层开始查找，只是多走几步空层（那些层只有 head_，下一步自然下降），**不影响正确性**。

> 💡 **理解技巧**：这是无锁设计的典型思路——先问"读错会怎样"，答案若是"只是慢一点"，就可以放心用 relaxed。

→ **下一站**：结构就位，看读操作怎么做到全程无锁。知识点 5。

<a id="id5"></a>
## ✅ 知识点 5: 无锁读：FindGreaterOrEqual 与 Contains

**Contains 就是一次 FindGreaterOrEqual 加一次相等判断——全程没有锁，连原子写都没有。**

**Contains**（[:1359-1369](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1359-L1369)）：

```cpp
bool InlineSkipList<Comparator>::Contains(const char* key) const {
  Node* x = nullptr;
  auto status = FindGreaterOrEqual(key, &x, false, false, nullptr);
  return (x != nullptr && Equal(key, x->Key()));
}
```

**FindGreaterOrEqual 的三个工程细节**（[:591-642](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L591-L642)）：

1. **相等立即返回**：`cmp == 0` 时不降到 0 层，当前层直接命中（[:630-632](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L630-L632)）
2. **`last_bigger` 复用比较结果**：下降时记下"刚把我拦下的节点"，下一层跳过重复比较（[:627-639](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L627-L639)）
3. **`PREFETCH` 预取**：比较之前先预取 `next->Next(level)` 进缓存（[:610](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L610)）

**为什么不能用 `FindLessThan(key)->Next(0)` 实现它**？源码注释（[:597-601](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L597-L601)）：两步之间可能有并发插入，结果就不对了——**无锁世界里"分两步"本身就是 bug 温床**。

> 🔄 **知识关联**：这里 `Next()` 用的是 acquire 读——为什么 acquire 就够，知识点 7 揭晓。

→ **下一站**：读无锁好理解，写呢？多个线程同时插怎么办？知识点 6。

<a id="id6"></a>
## ✅ 知识点 6: 无锁写：Splice 与 CAS 链接

**插入 = 先备好"括号"（Splice），再逐层 CAS 把自己钩进去；失败就重算括号重试。**

**Splice 是什么**（[:340-350](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L340-L350)）：每层一对 `prev_[i] / next_[i]` 指针，把新节点该进的位置"括"住。注释给出了括号不变式：`prev_[i+1] ≤ prev_[i] < key < next_[i] ≤ next_[i+1]`。

**CAS 链接循环**（`Insert<UseCAS=true>` 核心，[:1134-1171](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1134-L1171)）：

```cpp
for (int i = 0; i < height; ++i) {
  while (true) {
    // 第 0 层查重（注释：level 0 is sufficient）
    if (i == 0 && /* prev 或 next 撞上同 key */ ...) return false;
    x->NoBarrier_SetNext(i, splice->next_[i]);          // ① 先备好自己的 next
    if (splice->prev_[i]->CASNext(i, splice->next_[i], x)) {
      break;                                            // ② CAS 把 prev 的 next 换成自己
    }
    // ③ CAS 失败 = 有人抢先插了 → 重算该层括号，再试
    FindSpliceForLevel(key_decoded, splice->prev_[i], nullptr, i,
                       &splice->prev_[i], &splice->next_[i]);
  }
}
```

- **CAS 失败的处理**：只在**当前层**从 prev 重新走一段（`FindSpliceForLevel`，[:946-972](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L946-L972)），不用从头搜索——注释解释了为什么这样划算（[:1157-1161](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1157-L1161)）
- **重复检测只在第 0 层**做：所有节点都经过第 0 层，查到一次就够了（[:1137-1147](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1137-L1147)）
- **非并发路径**（[:1196-1197](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1196-L1197)）：同样的两步，但第二步用普通 `SetNext` 而非 CAS——因为只有一个人在写

**Splice 的复用（顺序写优化）**：非并发 `Insert` 会缓存上一次的括号（`seq_splice_`，[:244-247](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L244-L247)）；顺序插入时新 key 就在上个括号旁边，定位从 $O(\log N)$ 降到 $O(\log D)$（D = 与上次的距离，注释 [:127-136](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L127-L136)）

> 💡 **理解技巧**：CAS 的哲学是"乐观假设没人抢，抢了再重来"——竞争少时比锁快得多；第 0 层查重、逐层重算，都是把"重来"的成本压到最小。
> ⚠️ **关键区分**：CAS 链接从第 0 层往高层做（`i` 从 0 递增）——节点先入底层即"对读者可见"，高层只是索引。

→ **下一站**：CAS 换了指针，凭什么读者拿到的节点是初始化完整的？知识点 7。

<a id="id7"></a>
## ✅ 知识点 7: 内存序四件套——无锁正确的根

**acquire 读、release 写、CAS 强一致、relaxed 备料——四个变体各司其职，错一个就翻车。**

`Node` 的指针访问全部封装成带内存序的方法（[:379-407](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L379-L407)）：

| 方法 | 内存序 | 用途 |
|------|--------|------|
| `Next(n)` | **acquire** 读 | 读者拿到 next 指针 ⇒ 保证看到指向节点的完整初始化 |
| `SetNext(n, x)` | **release** 写 | 发布方：把"指向新节点"的指针挂出去 ⇒ 之前的初始化对读者可见 |
| `CASNext(n, exp, x)` | **CAS 强** | 并发写的原子交换 |
| `NoBarrier_Next/SetNext` | **relaxed** | 备料阶段用（此时节点还没发布，别人看不到） |

**发布的正确顺序**（`InsertAfter` 的注释把这个思想说得最直白，[:410-415](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L410-L415)）：

```cpp
void InsertAfter(Node* prev, int level) {
  // NoBarrier_SetNext() suffices since we will add a barrier when
  // we publish a pointer to "this" in prev.
  NoBarrier_SetNext(level, prev->NoBarrier_Next(level));  // 备料：relaxed 就行
  prev->SetNext(level, this);                             // 发布：release 兜底
}
```

**为什么读可以完全不拿锁**——三条合谋：

1. **发布即完成**：节点所有字段（含 key）在 release 挂链之前就绪；读者 acquire 读到的指针 ⇒ 指向的必然是完全体
2. **节点不可变**：链接后 key 和指针数组不再修改
3. **Arena 保活**：节点内存永不单独释放（知识点 2）→ 读者手里的指针永不悬垂，**没有 ABA 问题**

> 💡 **理解技巧**：acquire/release 配对建立的是"先发生后可见"的通道：写方 release 之前的所有写，对读到该指针的 acquire 读方全部可见。这就是无锁跳表不用锁也能读到完整节点的根本原因。
> 📋 **术语提醒**：`Atomic<Node*>` 不是裸 `std::atomic`，是 RocksDB 的封装（[util/atomic.h:105](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/util/atomic.h#L105)，`RelaxedAtomic<T>` 的子类），统一了 Load/Store/CAS 的默认内存序。

→ **下一站**：结构、读写都齐了，迭代器在这副骨架上能做什么、不能做什么？知识点 8。

<a id="id8"></a>
## ✅ 知识点 8: Iterator 的能力与代价

**前进免费，后退昂贵——没有 backward 指针，Prev 就是一次重新搜索。**

**能力清单**（定义 [:170-227](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L170-L227)）：

- `Next()`：`node_->Next(0)`，第 0 层走一步，$O(1)$（[:447-457](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L447-L457)）
- `Seek(target)`：包装 `FindGreaterOrEqual`（[:511-516](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L511-L516)）
- `SeekToFirst/SeekToLast`：后者走 `FindLast` 逐层到底（[:545-556](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L545-L556)）

**Prev 的真相**（[:482-491](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L482-L491)）：

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

- 注释说得很直白：**不存 prev 链接，退一格就重新查一次**
- `SeekForPrev` = `Seek` + 必要时 `Prev` 回退（[:528-538](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L528-L538)）

> 💡 **理解技巧**：反向指针不是存不起，是"每节点每层都多一个指针"的内存税太重——RocksDB 选择让低频的 Prev 付 $O(\log N)$ 的搜索费。反向迭代密集的场景有别的办法（比如换个 MemTableRep）。

---

## 🔑 核心要点总结

1. Node 是倒装布局：**高层指针在 header 前（负索引）、key 在 header 后**，一次 Arena 分配；身高借 `next_[0]` 暂存（StashHeight）
2. Arena 推指针分配：对齐 + bump，块 2KB 内联起步、4KB 常规、2GB 上限；无单节点释放，整表销毁一次搞定
3. RandomHeight：随机数 vs 预计算阈值 `kScaledInverseBranching_`，一次比较一次"抛硬币"；软上限 12 层、硬上限 32 层
4. 表高 `max_height_` 从 1 起步、relaxed CAS 懒增长——"读过头无害"所以敢用 relaxed
5. 读全程无锁：`Contains = FindGreaterOrEqual + Equal`；acquire 读 + PREFETCH + last_bigger 复用比较
6. 写用 CAS：Splice 括号定位 → 逐层 `NoBarrier_SetNext` 备料 + `CASNext` 挂链 → 失败仅重算当前层；重复检测只在第 0 层
7. 无锁正确三支柱：**release/acquire 配对发布 + 节点不可变 + Arena 保活无 ABA**
8. 迭代器：`Next` $O(1)$；`Prev` = `FindLessThan` 重搜 $O(\log N)$——不存反向指针的代价转移

## 📌 面试速记版

- **Node 布局**：`[高层指针… | Node(next_[0]) | key]`，身高藏 next_[0]，插入时 `key_ptr - 1` 找回节点
- **无锁写**：Splice 括号 + 逐层 CAS，失败重算当前层括号；查重只在第 0 层
- **无锁读**：acquire 读 next ⇒ 看到完整节点；正确性 = release 发布 + 不可变 + Arena 保活
- **内存序**：Next=acquire / SetNext=release / CASNext=强 CAS / 备料=relaxed
- **Prev 的代价**：无 backward 指针，退一格 = 重新 $O(\log N)$ 搜索
- **参数**：默认 12 层、1/4 晋级概率；表高 relaxed 懒增长

**记忆口诀**：指针倒装 key 在后，Arena 推针不回收；读凭 acquire 写靠 CAS，Prev 太贵重新搜。

## ✅ 自测 Checkpoint

1. 画出一个 height=2 节点的完整内存布局，`Node*` 指向哪？key 和高层指针各在哪？
2. `StashHeight` 为什么能借用 `next_[0]`？这个值什么时候失效？
3. Arena 的 `ApproximateMemoryUsage` 怎么算？为什么 `SkipListRep` 返回 0？
4. `max_height_` 为什么用 relaxed 读就够？（用"读错会怎样"的思路回答）
5. CAS 插入失败时做什么？为什么只重算当前层？
6. 无锁读正确的三个支柱是什么？少了 Arena 保活会出什么问题？
7. `Prev()` 怎么实现的？为什么 RocksDB 接受这个代价？

## 🔍 待验证点

【到源码核实】格式：结论 → `文件:行号` → ✅/❌ → 若有误记录正确结论

1. MemTable 在什么条件下把容器分配器换成 `ConcurrentArena`（而非裸 `Arena`）→ `db/memtable.h` 中搜 `ConcurrentArena` 的使用处
2. `allow_concurrent_memtable_write` 的默认值与生效条件 → `include/rocksdb/advanced_options.h` 或 options.h
3. `ConcurrentArena` 相对 `Arena` 加了什么并发保护 → [memory/concurrent_arena.h:42](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/concurrent_arena.h#L42) 起

⏸ **停止点**。跳表篇完结——回去写 Lab 1 时，你手里的 KopiDB 跳表会比 RocksDB 的简单得多（单线程、shared_ptr、有 backward_），但每个简化你现在都知道"工业版为什么不敢这么做"。
