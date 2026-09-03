# 📘 SkipList 基础：put / get / remove 的 RocksDB 对应实现（InlineSkipList Basics）

> RocksDB 源码精读 · 01 跳表·基础读写 | 源码版本 [`e6a2ee0`](https://github.com/facebook/rocksdb/tree/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7) | 对应 **Lab 1.1**：`SkipList::put` / `get` / `remove` / `random_level`

**本篇定位**：一篇笔记对应一个子 lab，篇内只讲 lab 函数在 RocksDB 跳表（`InlineSkipList`，[inlineskiplist.h](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h)）里的对应实现：

| 你的 lab 函数 | RocksDB 对应实现 | 本篇位置 |
|---|---|---|
| `random_level()` | `RandomHeight()` | 知识点 4 |
| `put(k, v)` | `AllocateKey` → `AllocateNode` → `Insert`（挂链） | 知识点 2-3、5、7 |
| `get(k)` | `Contains` → `FindGreaterOrEqual` | 知识点 6 |
| `remove(k)` | 跳表层**无对应**——RocksDB 用墓碑删除 | 知识点 8 |

跳表只是 MemTable 的内存容器；容器装进 MemTable 之后的读写路径属于 Lab 2.1，见 [05-memtable-rw](05-memtable-rw.md)。迭代器（Lab 1.2）、范围查询（Lab 1.3）各自成篇。

---

## 🧠 核心概念总览

- [*知识点1: 跳表(SkipList)结构原理*](#id1)
- [*知识点2: Node 的倒装内存布局——put 的节点怎么摆*](#id2)
- [*知识点3: Arena 分配器——put 的内存从哪来*](#id3)
- [*知识点4: RandomHeight 与层高参数——random_level 对应物*](#id4)
- [*知识点5: head_ 与 max_height_ 的松弛设计*](#id5)
- [*知识点6: 查找：FindGreaterOrEqual 与 Contains——get 对应物*](#id6)
- [*知识点7: 插入挂链：Splice 与"备料-发布"两步——put 对应物*](#id7)
- [*知识点8: remove 无对应物——删除是墓碑的事*](#id8)

---

<a id="id1"></a>
## ✅ 知识点 1: 跳表(SkipList)结构原理

**跳表 = 多层有序链表：底层全量、上层稀疏索引，查找类似二分。**

```
Level 2:  HEAD ────────────────────────► 17 ───────────────► NIL
                                         │
Level 1:  HEAD ───► 9 ─────────────────► 17 ─────► 25 ─────► NIL
                                         │
Level 0:  HEAD ───► 3 ──► 9 ──► 12 ────► 17 ──► 19 ──► 25 ─► NIL

查找 19 的路径：HEAD(L2) → 17(L2) → 下降 → 17(L1) → 下降 → 17(L0) → 19 ✔
插入 22 的路径：HEAD(L2) → 17(L2) → 下降 → 17(L1) → 下降 → 17(L0) → 19 前驱找到 → 插入 22 
```

**核心规则：**

- 第 0 层包含**所有**节点，越往上越稀疏
- 每个节点以概率 $p$ 向上"晋级"一层，层数呈指数衰减分布
- **查找**：从最高层向右走，走不通就下降一层 → 期望复杂度 $O(\log n)$
    1. **从最顶层开始**：从跳表的最高层（最稀疏的索引层）的头部节点出发。
    2. **向右大步跳跃**：在当前层向右遍历，如果下一个节点的值 **小于** 目标值，就继续向右跳。
    3. **太大就往下掉**：如果下一个节点的值 **大于或等于** 目标值，或者右边没节点了，就下降到下一层。
    4. **重复直到底层**：在下一层继续执行"向右跳→太大就下降"的过程，直到到达最底层（原始数据层）。
    5. **底层精确定位**：在最底层继续向右比较，找到目标值或确认不存在。
- **插入**：先按查找路径定位，再随机决定新节点层数，逐层接指针
    1. **先找到位置**：像查找一样从顶层往下走，记录每层最后经过的节点（这些就是新节点的前驱）。
    2. **抛硬币定层高**：随机决定新节点有几层（比如抛硬币，正面就加一层，直到反面为止）。
    3. **创建节点**：按决定的层数创建新节点，存好要插入的值。
    4. **各层插入链表**：从最底层到最高层，像普通链表一样把新节点"缝"进前驱和后继之间。
    5. **更新表头高度**：如果新节点层数超过了当前最高层，把头节点也增高到这一层。

**RocksDB 的具体参数**（源码证据 [inlineskiplist.h:74-76](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L74-L76)）：

```cpp
explicit InlineSkipList(Comparator cmp, Allocator* allocator,
                        int32_t max_height = 12,
                        int32_t branching_factor = 4);
```

- 最高 **12 层**；每晋级一层概率 **1/4**（`branching_factor = 4`，实现见 `RandomHeight()` [inlineskiplist.h:559-570](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L559-L570)）

> 📋 **术语提醒**：`分支因子(branching factor)` = 相邻两层之间的稀疏比例。$p = 1/4$ 即平均每 4 个节点才有 1 个晋级。
> ⚠️ **关键区分**：晋级概率越小，索引越稀疏、单节点内存越省，但查找步数略增——空间与时间的权衡。


---

<a id="id2"></a>
## ✅ 知识点 2: Node 的倒装内存布局——put 的节点怎么摆

**那 RocksDB 的一个跳表节点在内存里到底怎么摆**？

- 跳表靠 Node 串成链表，RocksDB 的 Node 是"头信息 + 内联数据"的变长结构，用 Arena 内存池管理。

**一个跳表节点要存什么？**
  - **一个 key 字节串**
  - **外加"它参加的每一层"各一个 next 指针**（知识点 1：节点身高随机，参加几层有几个指针）
  - 问题来了：每个节点层数不一样，Node 这个 struct 该怎么摆这些大小不一的东西？

**先看两个朴素设计，才知道 RocksDB 在躲什么：**

- **朴素 A——定长数组**：`struct Node { int height; Node* next[12]; }`。
  - 简单，但每个节点都背 12 个指针（96 字节），而平均身高只有 4/3 层（知识点 4 会算这笔账）→ **约 90% 的指针内存是空气**
- **朴素 B——变长数组**：`next[height]`，跟着身高走。
  - 但 C++ 的 struct 不允许真正的变长成员；得额外存个 `height` 字段才知道指针区多长，多背一个包袱

**RocksDB 的选择**：三段式倒装布局
- 高层指针放 header 前面，key 放 header 后面，header 本身只剩一个指针（[inlineskiplist.h:352-356](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L352-L356)）：


**布局图**（以一个 height=3 的节点为例，假设本次分配的原始内存起点 `raw` 在地址 1000）：

```
低地址 ──────────────────────────────────────────────────────► 高地址
+-----------+-----------+-----------+------------------------+
| next_[-2] | next_[-1] | next_[0]  | key bytes ...          |
+-----------+-----------+-----------+------------------------+
  addr 1000   addr 1008   addr 1016   addr 1024
  ^           ^           ^           ^
  (1)         (2)         (3)         (4)
```

**逐段拆解**：

- **(3) header（8 字节，地址 1016）**：
  - `Node` 这个 struct 里**唯一**声明的成员就是 `Atomic<Node*> next_[1]`（[:417-421](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L417-L421)，`Atomic` 是 RocksDB 对 `std::atomic` 的封装）
  - `sizeof(Node)` 只有 8 字节，这个格子就是**第 0 层**的 next 指针。"header" 听着唬人，其实就一个指针
- **(2) (1) 高层指针区（header 前面，(height−1)×8 字节）**：
  - 第 1 层、第 2 层的 next 指针**倒着往低地址排**——第 1 层紧贴 header 前（1008），第 2 层再往前（1000）。
- **(4) key 区（header 紧后面）**：
  - **key 字节串，零偏移贴合**
  - SkipList Node 本身没有 value 字段，只有一段"key 数据区"
  - 在 RocksDB 的 MemTable 里，传给 SkipList 的这段连续内存，通常编码的是一整个 entry:
    ```
    [varint key_len][internal_key][varint value_len][value]
    ```
  - 实际上在 MemTable 这个使用场景里，**key区 = entry**

**"负索引"是什么？** 
- C++ 里数组下标本来就可以是负的——`a[-1]` 等价于 `*(a - 1)`，只要指向的是合法内存。
- RocksDB 利用这一点：struct 里只声明 `next_[1]`，第 n 层的指针用 `(&next_[0] - n)` 去够（`Next()` 的实现，[:379-384](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L379-L384)）。
- 源码注释说得更直白（[:418-419](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L418-L419)）："Higher levels are stored _earlier_, so level 1 is at next_[-1]." 对照上图：

  | 要访问什么 | 定位公式 | 本例地址 |
  |------|------|------|
  | 第 0 层 next 指针 | `&next_[0]`（就是 header 本身） | 1016 |
  | 第 n 层 next 指针 | `(&next_[0] - n)` | 第 1 层 1008、第 2 层 1000 |
  | key 起点 | `&next_[1]`（`Key()` 的实现，[:374](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L374)） | 1024 |
  | 从 key 反推 Node | `(Node*)key - 1`（Insert 开头在用，[:1030](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1030)） | 1024 − 8 = 1016 |

**分配时怎么摆出这三段？** `AllocateNode`（[:858-880](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L858-L880)）：

- 一次分配 = 高层指针区 + header + key，Node 指针指向 `raw + prefix`：

  ```cpp
  auto prefix = sizeof(Atomic<Node*>) * (height - 1);  // 高层指针区大小
  char* raw = allocator_->AllocateAligned(prefix + sizeof(Node) + key_size);
  Node* x = reinterpret_cast<Node*>(raw + prefix);     // header 落在正中间
  x->StashHeight(height);  // 见下
  ```

> 📍 **调用位置**：`AllocateNode` 全库仅两个调用方——① 构造时 `head_(AllocateNode(0, max_height))`（[:840](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L840)，知识点 5 即讲）；② 每次插入前经 `AllocateKey`（[:855](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L855)）。`AllocateKey` 生产环境唯一调用方是 `SkipListRep::Allocate`（[skiplistrep.cc:36](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L36)）——正是 [05 §KP6](05-memtable-rw.md#id6) `MemTable::Add` 第一步 `table->Allocate`（[memtable.cc:1142](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L1142)）钻到最里层：**先分节点、stash 身高，再把 key 编码进节点尾部，最后 InsertKey 挂链**。

**StashHeight：身高的"临时便签"**
- 这里有个微妙问题：节点链入跳表后**根本不需要**存身高：
  - **你从第 h 层走进这个节点，h 就必然是它的合法层**（[:871-874](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L871-L874) 注释原话）
- 但 `Insert` 的那一刻又必须知道身高：
  - **通过`height` 知道给这个 `node` 留几个 `next` 位置**
- **解法**：
  - 趁节点还没链入、`next_[0]` 这 8 字节还空着
  - 把 int 身高**借**存在里面（[:358-372](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L358-L372)
  - Insert 时 `UnstashHeight` 取出（[:1032](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1032)）。

> ⚠️ **关键区分**：StashHeight 只是"传递中的便签"——一旦 `SetNext` 往 `next_[0]` 写了真指针，便签即作废（注释原话 "Undefined after a call to SetNext"）。
> 💡 **理解技巧**：这套布局的收益是"省内存双杀"——① 不存 height 字段；② 高层指针只为高个子节点付费（平均身高 4/3 层 → 平均每节点指针开销 ≈ 8×4/3 ≈ 10.7 字节，对比朴素 A 的 96 字节）。代价是所有访问都要做指针算术，因此全封装进 `Next/SetNext` 方法。


---

<a id="id3"></a>
## ✅ 知识点 3: Arena 分配器——put 的内存从哪来

**节点的"摆法"定了，但内存从哪儿来？几十万个不规则节点逐个 `new` 行不行？**

- 比如 RocksDB 要创建 10 万个 **大小不固定`((height−1)×8 + 8 + key_size )字节`的节点**：
  ```
  普通 new：

  new Node → 堆申请
  new Node → 堆申请
  new Node → 堆申请
  ...
  10 万次 new
  ```
- 这样会产生很多问题：频繁进入堆分配器、可能加锁，而且大量小块内存比较分散

**RocksDB 的答案**：使用 Arena 一次向堆要一大块，自己在块里"推指针"分
  - Arena 的做法：
    ```
      第一次：
      从堆申请一大块，比如 4KB

      ┌──────────────────────────────┐
      │ Node │ Node │ Node │         │
      └──────────────────────────────┘
             ↑
        bump pointer
    ```

  - 然后：
    ```
    分配 Node
    ↓
    aligned_alloc_ptr_ 往后移动 sizeof(Node)
    ↓
    再分配 Node
    ↓
    继续往后移动
    ```


所以它叫 **bump allocation** 指针碰撞/指针推进分配（`Arena::AllocateAligned`，[arena.cc:108-143](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.cc#L108-L143)）：

- **所谓"推指针"：块内只维护两个数**
  1. 未使用起点 `aligned_alloc_ptr_` 
  2. 剩余字节数 `alloc_bytes_remaining_`
- 分配 = 起点前移 + 余额扣减，**没有空闲链表、没有合并分裂，几次加减法就是一次分配**

  ```cpp
  // 对齐：原子指针要求地址是 8 的倍数，先算要补几个字节
  size_t slop = (current_mod == 0 ? 0 : kAlignUnit - current_mod);
  if (needed <= alloc_bytes_remaining_) {
    result = aligned_alloc_ptr_ + slop;   // 当前块够用：返回对齐后的位置
    aligned_alloc_ptr_ += needed;         // "未使用起点"往前推，完事
    alloc_bytes_remaining_ -= needed;
  } else {
    result = AllocateFallback(bytes, true);  // 当前块不够：向堆要一块新的
  }
  ```
- 那如果当前 4KB block不够放了呢? **重新申请**！
  - 意思非常简单：
    ```
    当前 Arena Block 不够了
            ↓
    再向堆申请一个新的大 block: AllocateFallback(bytes, true);
            ↓
    继续 bump allocation
    ```
- 那 `slop` 是什么？**就是为了满足内存对齐!**
  - 比如 RocksDB 要求地址是 8 的倍数:
    ```
    当前地址 = 1003
    1003 % 8 != 0
    ```
  - 那么先浪费几个字节：
    ```
    1003 → 1008
          ↑
          slop
    ```
  - 然后从 1008 开始分配

- 为什么需要内存对齐？**提高 CPU 读写数据的效率，并满足某些硬件的特殊限制！**
  - CPU 读取内存不是一个字节一个字节读的，而是成块读取（比如一次读 4 字节或 8 字节）
  - **对齐的情况**：如果一个 4 字节的整数（int）存放在能被 4 整除的地址上，CPU 一次就能把它完整读出来。
  - **不对齐的情况**：如果它存放在奇数地址，CPU 可能需要读取两次内存，再把碎片拼起来，速度慢很多

> ⚠️ **为什么 RocksDB 选 8？**: 因为 Arena 里面可能存放各种对象，选 8 是为了让 Arena 无论在 64 位还是 32 位机器上都能安全地放下需要 8 字节对齐的对象；4 太小，无法保证 8 字节对齐
> ```
> uint64_t      // 8 bytes
> double        // 通常需要 8-byte alignment
> void*         // 64 位机器通常是 8 bytes
> ```



**Arena的块管理策略**（[arena.h:31-37](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.h#L31-L37)）


- **Block 会根据需求增长，但设置一个最大上限**
  - 内联块 2KB（`kInlineSize`，小 MemTable 零堆分配）
  - 常规块 4KB 起（`kMinBlockSize`）
  - 上限 2GB
- **新块用 `new char[block_bytes]` 向系统申请一整块连续的原始内存**
- **并用 `malloc_usable_size` 拿堆实际给的大小精确记账**（[arena.cc:145](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.cc#L145) 起），比如：
  - `new char[4096]` 但底层内存分配器可能实际上给了你 `4160 bytes`
  - 所以 RocksDB 会问一下：哥们，你实际上给我的这块内存到底有多大？
- **没有单节点释放**
  - MemTable 销毁时整个 Arena 一次性释放——这正是推指针分配能成立的对价
  - Arena 不负责一个一个 Node 的释放，而是负责管理这些“大块 Block”

**块内存统计**：Arena 里所有 Node 的内存，最终都来自 Arena 自己申请的 Block，所以只需要让 Arena 统一记账
  - 例如：
    ```
    Arena 一共申请了 4096B
    当前这个 Block 还剩 1000B

    实际已经用掉：
    4096 - 1000 = 3096B
    ```
  - 所以：
    - `ApproximateMemoryUsage() = 已申请的 Block 总大小 - 当前剩余空间`（[arena.h:66-69](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.h#L66-L69)）
    - `SkipListRep::ApproximateMemoryUsage` 直接返回 0，因为账全记在 Arena 这里（[skiplistrep.cc:80-83](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L80-L83)）
      - `SkipList`：我不统计，返回 0

      - `Arena`：所有内存我统一统计

> 🔄 **知识关联**："无单节点释放"听着危险（读者正拿着指针怎么办？）——节点内存与整张 MemTable 同生共死，读者手里的指针永不悬垂。这个性质是 RocksDB 读路径敢不拿锁的底气之一，[05-memtable-rw](05-memtable-rw.md) §KP9 回收这个伏笔。并发写场景 MemTable 会用 `ConcurrentArena`（[concurrent_arena.h:42](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/concurrent_arena.h#L42)），选择逻辑这里不展开。


---

<a id="id4"></a>
## ✅ 知识点 4: RandomHeight 与层高参数——random_level 对应物

**节点大小由身高决定，可是要如何决定呢？身高要满足什么分布、又怎么低成本生成？**

-  知识点 1 讲过直觉：全 1 层退化成链表（O(n)），层层都满又太刚
- 要的是**指数衰减**：第 1 层 100%，第 2 层 1/4，第 3 层 1/16……

  `RandomHeight()`（[inlineskiplist.h:559-573](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L559-L573)）：

  ```cpp
  int height = 1;
  while (
      height < kMaxHeight_ &&
      height < kMaxPossibleHeight &&
      rnd->Next() < kScaledInverseBranching_
  ) {
      height++; // 一次比较 = 抛一次硬币
  }
  ```

> 📍 **调用位置**：`RandomHeight` 全库唯一调用点就是 `AllocateKey`（[:855](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L855)）——每次插入分配节点时现抛一次。注意它**不在** `Insert` 里：身高在分配时就定死并 stash（知识点 2 的便签），`Insert` 只是 `UnstashHeight` 取回（[:1032](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1032)）。

**没有取模、没有浮点——"抛硬币"就是拿随机数和一个预计算阈值比大小：**

- `rnd->Next()` 均匀分布在 `[0, 2³²)`
- `kScaledInverseBranching_` 在构造时预计算为 `(Random::kMaxNext + 1) / kBranching_`（[:837](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L837)）= 2³²/4 = 2³⁰
- 随机数 < 2³⁰ 的概率恰好 1/4 → 每轮循环就是一次"1/4 概率晋级"
  ```
  随机数范围：
  0 ---------------- 2^30 ---------------- 2^32
                      ↑
                     1/4
  ```
- 期望层高 E[height] = 1/(1−p) = **4/3**（p = 1/4）
  - 平均每节点只占 1.33 层指针（≈10.7 字节），这就是知识点 2 敢在布局上省内存的底气

**两个层上限，别混**（循环条件里两个都在）：

- `kMaxHeight_ = 12`：**这张表的业务上限/可配置上限**（构造参数，[:831-836](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L831-L836)）
- `kMaxPossibleHeight = 32`：**物理硬上限/实现安全底线** 
  - 栈上数组的维度，比如并发插入路径里 `Node* prev[kMaxPossibleHeight]`（[:914](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L914)）
  - 身高绝不能超过它，否则栈数组越界

> 📋 **术语提醒**：`kScaledInverseBranching_` = 把"概率 1/4"换算成"32 位随机数域里的阈值"——从概率域到比较域的缩放，故名 Scaled Inverse Branching。
> ⚠️ **关键区分**：`max_height_`（当前表高，动态增长）和 `kMaxHeight_`（允许上限 12）是两回事——前者下个知识点讲。



---

<a id="id5"></a>
## ✅ 知识点 5: head_ 与 max_height_ 的松弛设计

**每个节点带着随机身高出生，那么表头长什么样？整张表当前算几层？**

**构造**（[:831-851](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L831-L851)）：

```cpp
head_(AllocateNode(0, max_height)),   // head 一次给满 12 层
max_height_(1),                        // 但当前表高从 1 起步
```

- **head 为什么一次给满**：head 是每一层查找的起点，哪层都可能用到 → 一次给满，终身不变
- **表高为什么从 1 起步**：空表从高层找起全是空层，白走——表高随实际需求"懒增长"

**表高增长**（插入时，[:1035-1044](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1035-L1044)）：

```cpp
int max_height = max_height_.LoadRelaxed();
while (height > max_height) {          // 新节点比现在的表高
  if (max_height_.CasWeakRelaxed(max_height, height)) {
    max_height = height;
    break;   // CAS 成功：我拔高了表
  }          // 失败：别人先拔了，用最新值再判断（多半直接退出循环）
}
```

**为什么敢用 relaxed（最弱的内存序）？** 成员声明处的注释直接回答了（[:239-242](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L239-L242)）：

> "Relaxed reads are always OK because starting from higher levels only helps efficiency, not correctness."

——读到**旧值**：从新表高之下的某层开始找，多走几步而已；读到**新值**从"比实际更高的层"开始：那些层只有 head_，比较失败自然下降。**两种"读错"都只是慢一点，不影响对错**。

> 💡 **理解技巧**：先问"读到旧值/错值会怎样"，答案若是"只是慢一点"，就放心用最弱同步；把昂贵的强同步留给真正影响正确性的地方。

→ **下一站**：骨架就位。看 get 的对应实现怎么查找。知识点 6。

---

<a id="id6"></a>
## ✅ 知识点 6: 查找：FindGreaterOrEqual 与 Contains——get 对应物

**知识点 1 讲过跳表查找的算法骨架，本知识点看它的工业级实现——也就是 lab 里 `get` 的 RocksDB 对应物：算法没变，但多了三个工程细节。**

**`Contains`：一次查找 + 一次相等判断**（[:1359-1369](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1359-L1369)）：
- 先调用 `FindGreaterOrEqual` 找到第一个 `>= key` 的节点，然后再判断它是不是 `== key`

  ```cpp
  bool InlineSkipList<Comparator>::Contains(const char* key) const {
    Node* x = nullptr;
    auto status = FindGreaterOrEqual(key, &x, false, false, nullptr);
    return (x != nullptr && Equal(key, x->Key()));
  }
  ```

**`FindGreaterOrEqual` 的三个工程细节**（[:591-642](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L591-L642)）：

1. **相等立即返回**（[:630-632](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L630-L632)）：`cmp == 0` 时在当前层直接命中返回，不降到第 0 层——找到了就别再往下走
2. **`last_bigger` 复用比较结果**（[:627-639](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L627-L639)）：降层时记下"刚把我拦下的节点"（第一个 ≥ key 的）；降到下一层后，如果下一个节点还是它，直接跳过比较视为"继续降"（`next == last_bigger ? 1 : compare_(...)`）——key 比较是查找的 CPU 热点，省一次是一次
3. **`PREFETCH` 预取**（[:610](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L610)）：比较之前先发预取指令，把 `next->Next(level)` 即将访问的内存提前拉进 CPU 缓存，隐藏内存延迟

**为什么不能用 `FindLessThan(key)->Next(0)` 实现"大于等于"？** 源码注释给了两个理由（[:597-601](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L597-L601)）：① 没法在相等时提前返回；② 两步之间可能有并发插入挤进来，`Next(0)` 拿到的就不是"第一个 ≥ key"的了——要拿某个位置，必须一次查找直接落位。

> 📍 **调用位置**：表外谁来调？读路径：`MemTable::Get` → `GetFromTable` → `table_->Get(key, &saver, SaveValue)`（[memtable.cc:1684](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L1684)）→ `SkipListRep::Get`（[skiplistrep.cc:85-93](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L85-L93)）——它不直接调 `FindGreaterOrEqual`，而是现场建 `SkipListRep::Iterator` 再 `Seek`（[skiplistrep.cc:89](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L89)），Seek 内部才进 `FindGreaterOrEqual`（[:514](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L514)，即 [02-skiplist-iter](02-skiplist-iter.md) §KP1 的 `Iterator::Seek`）。**点查与扫描共用同一条 Seek 通道**；[05 §KP7](05-memtable-rw.md#id7) 的"Seek 命中后逐版本回调 SaveValue"就发生在这个迭代器循环里（[skiplistrep.cc:90-91](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L90-L91)）。

→ **下一站**：会找了，再看怎么挂进去——put 的对应实现 Insert。知识点 7。

---

<a id="id7"></a>
## ✅ 知识点 7: 插入挂链：Splice 与"备料-发布"两步——put 对应物

**插入 = 先备好"括号"（Splice）定位，再逐层"备料-发布"把自己钩进去。**

> 📍 **调用位置**：`Insert` 的生产环境入口全部来自 `MemTable::Add`（[05 §KP6](05-memtable-rw.md#id6)）：`table->InsertKey`（[memtable.cc:1180](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/db/memtable.cc#L1180)）→ `SkipListRep::InsertKey` → `skip_list_.Insert`（[skiplistrep.cc:47](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L47)）→ `Insert<false>`（[:909](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L909)，复用 `seq_splice_`）。（RocksDB 另有一条 `concurrent_memtable_writes` 并发插入路径，用 CAS 原子指令替代普通指针写；lab 的跳表并发由上层 MemTable 的锁保证，那条路径本篇不展开。）

**Splice 是什么**（[:340-350](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L340-L350)）：每层一对 `prev_[i] / next_[i]`，在第 i 层把新节点该进的位置"括"住：`prev_[i] < key < next_[i]`。注释给出的不变式 `prev_[i+1] ≤ prev_[i] < key < next_[i] ≤ next_[i+1]` 用白话说：**高层的括号必然罩住低层的括号**（高层更稀疏，prev 只会更靠左、next 更靠右）——所以高层定位可以从低层结果出发，不用每层从头搜。

**挂链循环**（单写者路径 `Insert<UseCAS=false>` 核心，[:1180-1198](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1180-L1198)）：

```cpp
for (int i = 0; i < height; ++i) {
  // 第 0 层查重（注释：level 0 is sufficient）
  if (i == 0 && /* prev 或 next 撞上同 key */ ...) return false;
  x->NoBarrier_SetNext(i, splice->next_[i]);   // ① 备料：先写好我的 next
  splice->prev_[i]->SetNext(i, x);             // ② 发布：把 prev 的 next 换成我
}
```

- **①②的顺序是铁律**：先把新节点的 next 写好（此时没人看得见它，`NoBarrier_` 变体不发内存栅栏、最便宜），再发布——指针一旦换过去，读者立刻就能走进来
- **重复检测只在第 0 层做**（[:1181-1191](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1181-L1191)）：所有节点都经过第 0 层，查一次就够

**Splice 的复用**：非并发 `Insert` 缓存上一次的括号（`seq_splice_`，[:244-247](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L244-L247)）；顺序插入时新 key 就在上个括号旁边，定位从 O(log N) 降到 O(log D)（D = 与上次插入的距离，注释 [:127-136](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L127-L136)）。（并发路径没有"上一次插入"可复用，括号在栈上现开，本篇不展开。）

**彩蛋——Splice 自己的内存布局**：`AllocateSplice`（[:883-893](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L883-L893)）和 Node（知识点 2）是**同款套路**：struct 只当"锚点"，`prev_` / `next_` 两个数组作为尾随内存一次从 Arena 切出。"struct + 自定义尾随内存"这个模式在跳表里出现了两次，值得认出来。

> ⚠️ **关键区分**：链入从第 0 层往高层做（`i` 从 0 递增）——节点先入底层即"可被找到"，高层只是索引；读者因此永远能容忍"高层还没接好"的中间态。

→ **下一站**：put/get 都齐了。lab 里还有 `remove`——RocksDB 跳表能删节点吗？知识点 8。

---

<a id="id8"></a>
## ✅ 知识点 8: remove 无对应物——删除是墓碑的事

**Lab 1.1 的 `SkipList::remove` 在 RocksDB 的 `InlineSkipList` 里没有对应物：整张表没有任何删除接口，只增不减。**

- RocksDB 的删除发生在 **MemTable 层**：`Delete(key)` 不碰跳表现有节点，而是**再插一条** type = `kTypeDeletion` 的 entry（墓碑，tombstone）；读路径查到墓碑就判死，空间等后台 compaction 才真正回收
- 为什么值得：跳表的读者不拿锁（[05 §KP9](05-memtable-rw.md#id9)），靠的是"节点链入后不可变、内存不回收"——真删节点会把这两根柱子都抽掉
- 墓碑怎么写进 MemTable、读路径怎么短路，见 [05-memtable-rw](05-memtable-rw.md) §KP10；lab 里 `remove` 真删节点是教学简化（KopiDB 没有多版本与 compaction，真删最直观）

> ⚠️ **面试考点**："RocksDB 的跳表怎么删元素？"——答"它不删，删除是更高层的墓碑语义"，比答"remove 函数"高一个段位。

---

## 📌 面试速记版

| 面试题 | 一句话答 |
|---|---|
| 跳表结构与关键参数？ | 多层有序链表：第 0 层全量、上层稀疏索引；查找/插入期望 O(log N)；RocksDB 参数：最高 12 层、晋级概率 1/4 |
| RocksDB 跳表节点怎么省内存？ | 倒装布局：高层指针存 header 前（负索引访问）、key 贴 header 后；不存 height（链入后由访问层数隐式得知）；平均每节点仅 ≈10.7 字节指针开销 |
| 为什么节点内存用 Arena 而不是 new？ | 节点大小随机不均，逐个 new 必碎片；Arena 推指针分配 O(1)、无碎片、缓存友好、整表一次性释放 |
| 层高怎么随机？ | `rnd->Next() < 阈值(2³²/4)`，一次整数比较 = 一次 1/4 概率抛硬币；期望 4/3 层，软上限 12、硬上限 32 |
| max_height_ 为什么敢 relaxed？ | 从更高层开始找只是多走空层——"只影响效率，不影响正确性" |
| get 的对应实现？ | `Contains` = `FindGreaterOrEqual` + `Equal` 一次落位；三优化：相等立即返回、`last_bigger` 复用比较结果、`PREFETCH` 预取 |
| put 的对应实现？ | `AllocateKey`（随机身高 + 三段式节点）→ Splice 括号定位 → 逐层①备料写自己的 next ②发布改 prev 的 next；第 0 层查重，顺序插入复用 `seq_splice_` |
| RocksDB 跳表怎么删元素？ | 不删——跳表只增不减；删除是 MemTable 层写一条 `kTypeDeletion` 墓碑 entry |

**记忆口诀**：**"指针倒装 key 贴后，Arena 推针整块收；抛币比阈定身高，表高懒长不用愁；查找三招省比较，备料发布两步走；跳表只增从不删，删除墓碑另起头。"**

---

**下一站**：跳表的读写拆完了。Lab 1.2 要给跳表加光标——迭代器能做什么、Prev 为什么贵。→ [02-skiplist-iter](02-skiplist-iter.md)
