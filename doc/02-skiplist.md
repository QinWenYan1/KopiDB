# 📘 SkipList：Node 内存布局与无锁并发（Node Layout & Lock-free Concurrency）

> RocksDB 源码精读 · 02 跳表 | 源码版本 [`e6a2ee0`](https://github.com/facebook/rocksdb/tree/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7) | 本篇涵盖：Node 倒装内存布局、Arena 分配器、随机层高、head_ 设计、无锁读、CAS 无锁写、内存序、迭代器能力边界

**从 01 走来**：[01-data-path](01-data-path.md) 里
- Put 工作流的最后一跳是 `skip_list_.Insert(handle)`（01 §KP1 第 5 步）
- Get 工作流的最后一跳是 `MemTable::Get` 进跳表 `Seek`（01 §KP10）。
- 01 全程把跳表当**黑盒**：只要求它"有序、能 Insert、能 Seek、并发安全"。本篇拆开这个黑盒，依次回答三个问题：
  1. 一个跳表节点在**内存里到底长什么样**、内存从哪儿来？（知识点 1-2）
  2. 节点的**层高**怎么随机、整张表的**骨架**怎么搭？（知识点 3-4）
  3. 多线程同时读写，凭什么**一把锁都不用**？（知识点 5-7——这正是 01 §KP1 第 5 步"并发插跳表"能成立的根）最后看迭代器在这副骨架上的能力边界（知识点 8）。

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

跳表靠 Node 串成链表，RocksDB 的 Node 是"头信息 + 内联数据"的变长结构，用 Arena 内存池管理。

**一个跳表节点要存什么？**
  - 一个 key 字节串，外加"它参加的每一层"各一个 next 指针（01 §KP2：节点身高随机，参加几层有几个指针）。
  - 问题来了：每个节点层数不一样，Node 这个 struct 该怎么摆这些大小不一的东西？

**先看两个朴素设计，才知道 RocksDB 在躲什么：**

- **朴素 A——定长数组**：`struct Node { int height; Node* next[12]; }`。简单，但每个节点都背 12 个指针（96 字节），而平均身高只有 4/3 层（知识点 3 会算这笔账）→ **约 90% 的指针内存是空气**。
- **朴素 B——变长数组**：`next[height]`，跟着身高走。但 C++ 的 struct 不允许真正的变长成员；堆上手工摆，也得先存一个 `height` 才知道指针区有多长——每个节点多背一个字段。

**RocksDB 的选择：三段式倒装布局——高层指针放 header 前面，key 放 header 后面，header 本身只剩一个指针。** 源码原话（[inlineskiplist.h:352-356](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L352-L356)）：

> "The Node data type is more of a pointer into custom-managed memory than a traditional C++ struct. The key is stored in the bytes immediately after the struct, and the next_ pointers for nodes with height > 1 are stored immediately _before_ the struct."

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

逐段拆解：

- **(3) header（8 字节，地址 1016）**：`Node` 这个 struct 里**唯一**声明的成员就是 `Atomic<Node*> next_[1]`（[:417-421](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L417-L421)，`Atomic` 是 RocksDB 对 `std::atomic` 的封装，知识点 7 细讲）——`sizeof(Node)` 只有 8 字节，这个格子就是**第 0 层**的 next 指针。"header" 听着唬人，其实就一个指针。
- **(2)(1) 高层指针区（header 前面，(height−1)×8 字节）**：第 1 层、第 2 层的 next 指针**倒着往低地址排**——第 1 层紧贴 header 前（1008），第 2 层再往前（1000）。
- **(4) key 区（header 紧后面）**：key 字节串，零偏移贴合。

**"负索引"是什么？** C++ 里数组下标本来就可以是负的——`a[-1]` 等价于 `*(a - 1)`，只要指向的是合法内存。RocksDB 利用这一点：struct 里只声明 `next_[1]`，第 n 层的指针用 `(&next_[0] - n)` 去够（`Next()` 的实现，[:379-384](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L379-L384)）。源码注释说得更直白（[:418-419](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L418-L419)）："Higher levels are stored _earlier_, so level 1 is at next_[-1]." 对照上图：

| 要访问什么 | 定位公式 | 本例地址 |
|------|------|------|
| 第 0 层 next 指针 | `&next_[0]`（就是 header 本身） | 1016 |
| 第 n 层 next 指针 | `(&next_[0] - n)` | 第 1 层 1008、第 2 层 1000 |
| key 起点 | `&next_[1]`（`Key()` 的实现，[:374](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L374)） | 1024 |
| 从 key 反推 Node | `(Node*)key - 1`（Insert 开头在用，[:1030](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1030)） | 1024 − 8 = 1016 |

**分配时怎么摆出这三段？** `AllocateNode`（[:858-880](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L858-L880)）：

```cpp
auto prefix = sizeof(Atomic<Node*>) * (height - 1);  // 高层指针区大小
char* raw = allocator_->AllocateAligned(prefix + sizeof(Node) + key_size);
Node* x = reinterpret_cast<Node*>(raw + prefix);     // header 落在正中间
x->StashHeight(height);  // 见下
```

一次分配 = 高层指针区 + header + key，Node 指针指向 `raw + prefix`。

**StashHeight：身高的"临时便签"**。这里有个微妙问题：节点链入跳表后**根本不需要**存身高——你从第 h 层走进这个节点，h 就必然是它的合法层（[:871-874](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L871-L874) 注释原话）。但 `Insert` 的那一刻又必须知道身高，才能逐层接指针。解法：趁节点还没链入、`next_[0]` 这 8 字节还空着，把 int 身高**借**存在里面（[:358-372](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L358-L372)），Insert 时 `UnstashHeight` 取出（[:1032](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1032)）。

> ⚠️ **关键区分**：StashHeight 只是"传递中的便签"——一旦 `SetNext` 往 `next_[0]` 写了真指针，便签即作废（注释原话 "Undefined after a call to SetNext"）。
> 💡 **理解技巧**：这套布局的收益是"省内存双杀"——① 不存 height 字段；② 高层指针只为高个子节点付费（平均身高 4/3 层 → 平均每节点指针开销 ≈ 8×4/3 ≈ 10.7 字节，对比朴素 A 的 96 字节）。代价是所有访问都要做指针算术，因此全封装进 `Next/SetNext/CASNext` 方法（知识点 7 会看到它们还兼管内存序）。

→ **下一站**：节点的"摆法"定了，但内存从哪儿来？几十万个不规则节点逐个 `new` 行不行？知识点 2。

---

<a id="id2"></a>
## ✅ 知识点 2: Arena 分配器

**上一棒结论：节点大小不固定（(height−1)×8 + 8 + key_size 字节），数量几十万级。新疑问：这些不规则小块内存从哪儿来？** 逐个 `new` 有三笔账：每次分配进堆管理器（可能拿锁）、节点四处散落（缓存不友好）、大小混杂（碎片）。RocksDB 的答案：**一次向堆要一大块，自己在块里"推指针"分。**

**分配就是推指针（bump allocation）**（`Arena::AllocateAligned`，[arena.cc:108-143](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.cc#L108-L143)）：

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

所谓"推指针"：块内只维护两个数——未使用起点 `aligned_alloc_ptr_` 和剩余字节数 `alloc_bytes_remaining_`。分配 = 起点前移 + 余额扣减，**没有空闲链表、没有合并分裂，几次加减法就是一次分配**。

**块管理参数**（[arena.h:31-37](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.h#L31-L37)）：

- 内联块 2KB（`kInlineSize`，小 MemTable 零堆分配）→ 常规块 4KB 起（`kMinBlockSize`）→ 上限 2GB
- 新块用 `new char[block_bytes]`，并用 `malloc_usable_size` 拿堆实际给的大小精确记账（[arena.cc:145](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.cc#L145) 起）
- **没有单节点释放**：MemTable 销毁时整个 Arena 一次性释放——这正是推指针分配能成立的对价

**内存统计因此简化**：`ApproximateMemoryUsage() = 已分配块总量 − 当前块剩余`（[arena.h:66-69](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/arena.h#L66-L69)）——`SkipListRep::ApproximateMemoryUsage` 直接返回 0，因为账全记在 Arena 这里（[skiplistrep.cc:80-83](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/skiplistrep.cc#L80-L83)）。

> 🔄 **知识关联**："无单节点释放"听着危险（读者正拿着指针怎么办？）——它恰恰是并发读安全的三大支柱之一，知识点 7 回收这个伏笔。并发写场景 MemTable 会用 `ConcurrentArena`（[concurrent_arena.h:42](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memory/concurrent_arena.h#L42)），选择逻辑这里不展开。

→ **下一站**：内存有了。每个节点能进几层——身高怎么随机出来？知识点 3。

---

<a id="id3"></a>
## ✅ 知识点 3: RandomHeight 与层高参数

**上一棒结论：节点大小由身高决定。新疑问：身高要满足什么分布、又怎么低成本生成？** 01 §KP2 讲过直觉：全 1 层退化成链表（O(n)），层层都满又太刚——要的是**指数衰减**：第 1 层 100%，第 2 层 1/4，第 3 层 1/16……

`RandomHeight()`（[inlineskiplist.h:559-573](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L559-L573)）：

```cpp
int height = 1;
while (height < kMaxHeight_ && height < kMaxPossibleHeight &&
       rnd->Next() < kScaledInverseBranching_) {   // 一次比较 = 抛一次硬币
  height++;
}
```

**没有取模、没有浮点——"抛硬币"就是拿随机数和一个预计算阈值比大小：**

- `rnd->Next()` 均匀分布在 [0, 2³²)；`kScaledInverseBranching_` 在构造时预计算为 `(Random::kMaxNext + 1) / kBranching_`（[:837](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L837)）= 2³²/4 = 2³⁰
- 随机数 < 2³⁰ 的概率恰好 1/4 → 每轮循环就是一次"1/4 概率晋级"
- 期望层高 E[height] = 1/(1−p) = **4/3**（p = 1/4）——平均每节点只占 1.33 层指针（≈10.7 字节），这就是知识点 1 敢在布局上省内存的底气

**两个层上限，别混**（循环条件里两个都在）：

- `kMaxHeight_ = 12`：**这张表**的业务上限（构造参数，[:831-836](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L831-L836)）
- `kMaxPossibleHeight = 32`：物理硬上限——栈上数组的维度，比如并发插入时 `Node* prev[kMaxPossibleHeight]`（[:914](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L914)）；身高绝不能超过它，否则栈数组越界

> 📋 **术语提醒**：`kScaledInverseBranching_` = 把"概率 1/4"换算成"32 位随机数域里的阈值"——从概率域到比较域的缩放，故名 Scaled Inverse Branching。
> ⚠️ **关键区分**：`max_height_`（当前表高，动态增长）和 `kMaxHeight_`（允许上限 12）是两回事——前者下个知识点讲。

→ **下一站**：节点有了、身高有了。整张表的骨架——表头和"当前表高"怎么搭、怎么长？知识点 4。

---

<a id="id4"></a>
## ✅ 知识点 4: head_ 与 max_height_ 的松弛设计

**上一棒结论：每个节点带着随机身高出生。新疑问：表头长什么样？整张表当前算几层？并发插入都想"拔高"表时怎么办？**

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

——读到**旧值**：从新表高之下的某层开始找，多走几步而已；读到**新值**从"比实际更高的层"开始：那些层只有 head_，比较失败自然下降。**两种"读错"都只是慢一点，不影响对错**。（relaxed 到底是什么、为什么叫"最弱"——知识点 7 系统讲，这里先记住结论。）

> 💡 **理解技巧**：无锁设计的典型思路——先问"读到旧值/错值会怎样"，答案若是"只是慢一点"，就放心用最弱同步；把昂贵的强同步留给真正影响正确性的地方（知识点 7 的发布/订阅配对）。

→ **下一站**：骨架就位。看读操作怎么做到全程无锁。知识点 5。

---

<a id="id5"></a>
## ✅ 知识点 5: 无锁读：FindGreaterOrEqual 与 Contains

**01 §KP2 讲过跳表查找的算法骨架——"下楼梯"：从最高层向右大步走，下一个太大（或没有了）就降一层，直到第 0 层精确定位。本知识点看它的工业级实现：算法没变，但多了三个工程细节，而且全程没有锁、连一次原子写都没有。**

**Contains：一次查找 + 一次相等判断**（[:1359-1369](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1359-L1369)）：

```cpp
bool InlineSkipList<Comparator>::Contains(const char* key) const {
  Node* x = nullptr;
  auto status = FindGreaterOrEqual(key, &x, false, false, nullptr);
  return (x != nullptr && Equal(key, x->Key()));
}
```

**FindGreaterOrEqual 的三个工程细节**（[:591-642](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L591-L642)）：

1. **相等立即返回**（[:630-632](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L630-L632)）：`cmp == 0` 时在当前层直接命中返回，不降到第 0 层——找到了就别再往下走
2. **`last_bigger` 复用比较结果**（[:627-639](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L627-L639)）：降层时记下"刚把我拦下的节点"（第一个 ≥ key 的）；降到下一层后，如果下一个节点还是它，直接跳过比较视为"继续降"（`next == last_bigger ? 1 : compare_(...)`）——key 比较是查找的 CPU 热点，省一次是一次
3. **`PREFETCH` 预取**（[:610](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L610)）：比较之前先发预取指令，把 `next->Next(level)` 即将访问的内存提前拉进 CPU 缓存，隐藏内存延迟

**为什么不能用 `FindLessThan(key)->Next(0)` 实现"大于等于"？** 源码注释给了两个理由（[:597-601](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L597-L601)）：① 没法在相等时提前返回；② 两步之间可能有并发插入挤进来，`Next(0)` 拿到的就不是"第一个 ≥ key"的了。**无锁世界里"分两步读"本身就是 bug 温床**——要拿某个位置，必须一次查找直接落位。

> 🔄 **知识关联**：这里 `Next()` 用的是 acquire 读——读者顺着指针拿到的节点，凭什么保证 key 已初始化完整？知识点 7 揭晓。

→ **下一站**：读无锁好理解（只读不改）。写呢？两个线程同时往同一位置插节点，谁赢？知识点 6。

---

<a id="id6"></a>
## ✅ 知识点 6: 无锁写：Splice 与 CAS 链接

**插入 = 先备好"括号"（Splice）定位，再逐层 CAS 把自己钩进去；失败就重算括号重试。**

**先补一课：CAS 是什么？** CAS（Compare-And-Swap，比较并交换）是一条 CPU 原子指令：`CAS(地址, 期望值, 新值)` = "如果这个地址里**现在还是**期望值，就换成新值、返回成功；否则什么都不动、返回失败"。整个过程不可打断；多线程同时 CAS 同一地址，硬件保证只有一个成功。类比抢座：看了一眼 3 号位是空的（期望值），我坐下（换新值）；若坐下的瞬间发现已有人（期望值不符），起来重找。

**Splice 是什么**（[:340-350](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L340-L350)）：每层一对 `prev_[i] / next_[i]`，在第 i 层把新节点该进的位置"括"住：`prev_[i] < key < next_[i]`。注释给出的不变式 `prev_[i+1] ≤ prev_[i] < key < next_[i] ≤ next_[i+1]` 用白话说：**高层的括号必然罩住低层的括号**（高层更稀疏，prev 只会更靠左、next 更靠右）——所以高层定位可以从低层结果出发，不用每层从头搜。

**CAS 链接循环**（`Insert<UseCAS=true>` 核心，[:1134-1171](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1134-L1171)）：

```cpp
for (int i = 0; i < height; ++i) {
  while (true) {
    // 第 0 层查重（注释：level 0 is sufficient）
    if (i == 0 && /* prev 或 next 撞上同 key */ ...) return false;
    x->NoBarrier_SetNext(i, splice->next_[i]);          // ① 备料：先写好我的 next
    if (splice->prev_[i]->CASNext(i, splice->next_[i], x)) {
      break;                                            // ② 发布：CAS 把 prev 的 next 换成我
    }
    // ③ 失败 = 有人抢先插了 → 只重算这一层的括号，再试
    FindSpliceForLevel(key_decoded, splice->prev_[i], nullptr, i,
                       &splice->prev_[i], &splice->next_[i]);
  }
}
```

- **①②的顺序是铁律**：先把新节点的 next 写好（此时没人看得见它，用 relaxed 的 `NoBarrier_` 变体就够），再 CAS 发布——指针一旦换过去，读者立刻就能走进来
- **CAS 失败只重算当前层**（`FindSpliceForLevel`，[:946-972](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L946-L972)）：从这一层的 prev 往右走一小段找到新位置，不用整表重搜——为什么划算见注释 [:1157-1161](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1157-L1161)；窄化第 i 层括号后可能破坏与 i−1 层的括号关系，代码用 `splice_is_valid = false` 标记留给下轮整体重算（[:1165-1170](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1165-L1170)）
- **重复检测只在第 0 层做**（[:1137-1147](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1137-L1147)）：所有节点都经过第 0 层，查一次就够
- **非并发路径**（[:1196-1197](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L1196-L1197)）：同样的"①备料②发布"两步，但第②步用普通 `SetNext` 而非 CAS——只有一个写线程，没人抢

**Splice 的复用与复用的边界**：非并发 `Insert` 缓存上一次的括号（`seq_splice_`，[:244-247](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L244-L247)）；顺序插入时新 key 就在上个括号旁边，定位从 O(log N) 降到 O(log D)（D = 与上次插入的距离，注释 [:127-136](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L127-L136)）。而**并发路径不复用它**：`InsertConcurrently` 在栈上现开 `prev[kMaxPossibleHeight] / next[kMaxPossibleHeight]` 数组（[:913-919](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L913-L919)）——`seq_splice_` 是跨调用的共享状态，并发场景没有"上一次插入"的概念。

**彩蛋——Splice 自己的内存布局**：`AllocateSplice`（[:883-893](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L883-L893)）和 Node（知识点 1）是**同款套路**：struct 只当"锚点"，`prev_` / `next_` 两个数组作为尾随内存一次从 Arena 切出。"struct + 自定义尾随内存"这个模式在跳表里出现了两次，值得认出来。

> 💡 **理解技巧**：CAS 的哲学是"乐观假设没人抢，抢了再重来"——竞争少时远快于锁；第 0 层查重、逐层重算，都是把"重来"的成本压到最小。
> ⚠️ **关键区分**：CAS 链接从第 0 层往高层做（`i` 从 0 递增）——节点先入底层即"对读者可见"，高层只是索引。读路径（知识点 5）因此永远能容忍"高层还没接好"的中间态。

→ **下一站**：CAS 换了指针，凭什么读者拿到的节点是初始化完整的？——内存序，无锁正确的根。知识点 7。

---

<a id="id7"></a>
## ✅ 知识点 7: 内存序四件套——无锁正确的根

**上一棒的幽灵问题：CAS 把指针挂出去的瞬间，读者就能顺着指针走进新节点——可读者凭什么保证看到 key 已经写好了？答案藏在一个反直觉的事实里：程序的执行顺序 ≠ 内存的可见顺序。**

**先补两课。第一课：为什么会乱序？** 编译器优化和 CPU 流水线/写缓冲都会重排内存操作的**对外可见顺序**。单线程无所谓（最终结果等价）；多线程下，写方的"先写 key、再挂指针"在读方眼里可能变成"先看到指针、后看到 key"——读者顺着指针读到未初始化的 key，崩。

**第二课：内存序是什么？** 给原子操作贴的"顺序保证标签"，三个档位最关键：

- **relaxed**：只保证这次读/写本身不被撕坏（原子性），不给任何顺序承诺——最便宜
- **release**（写方贴）：我这次写**之前**的所有写，必须先于这次写对别人可见——"发布"
- **acquire**（读方贴）：我这次读**之后**的所有读，必须看到配对 release 之前的一切——"订阅"

release + acquire 配对成功，就在两个线程间建立了"先发生后可见"（happens-before）的通道。类比寄快递：release = 寄件人**装好箱才贴快递单**（单 = 指针）；acquire = 收件人**凭单取件开箱**——只要单子到了，箱里的东西必然完好；relaxed 则只保证"单号本身没印糊"。

**`Node` 的指针访问全部封装成带内存序的方法**（[:379-407](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L379-L407)）：

| 方法 | 内存序 | 用途 |
|------|--------|------|
| `Next(n)` | **acquire** 读 | 读者拿到 next 指针 ⇒ 保证看到指向节点的完整初始化 |
| `SetNext(n, x)` | **release** 写 | 发布方：把"指向新节点"的指针挂出去 ⇒ 之前的初始化对读者可见 |
| `CASNext(n, exp, x)` | **CAS 强** | 并发写的原子交换（自带最强顺序保证） |
| `NoBarrier_Next/SetNext` | **relaxed** | 备料阶段用（节点还没发布，别人看不到，不需要顺序保证） |

**发布的正确顺序**（`InsertAfter` 的注释把这个思想说得最直白，[:410-415](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L410-L415)）：

```cpp
void InsertAfter(Node* prev, int level) {
  // NoBarrier_SetNext() suffices since we will add a barrier when
  // we publish a pointer to "this" in prev.
  NoBarrier_SetNext(level, prev->NoBarrier_Next(level));  // 备料：relaxed 就行
  prev->SetNext(level, this);                             // 发布：release 兜底
}
```

现在回看知识点 6 的"①备料②发布"铁律，就是这两行。

**为什么读可以完全不拿锁——三条合谋：**

1. **发布即完成**：节点所有字段（含 key）在 release 挂链之前就绪；读者 acquire 读到指针 ⇒ 指向的必然是完全体
2. **节点不可变**：链入后 key 和指针数组不再修改——读到的东西不会在你手里变质
3. **Arena 保活**（知识点 2 伏笔回收）：节点内存永不单独释放 → 读者手里的指针永不悬垂，也**没有 ABA 问题**（ABA = 指针从 A 变 B 又变回 A，CAS 误以为没变过；这里地址永不复用，天然免疫）

> 💡 **理解技巧**：记忆锚点——**写方 release 发布，读方 acquire 订阅，备料 relaxed，交换 CAS**。
> 📋 **术语提醒**：`Atomic<Node*>` 不是裸 `std::atomic`，是 RocksDB 的封装（[util/atomic.h:105](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/util/atomic.h#L105)，`RelaxedAtomic<T>` 的子类），统一了 Load/Store/CAS 的默认内存序——这也是知识点 1 布局图里"一个格子 8 字节"仍成立的原因。

→ **下一站**：结构、读写、正确性都齐了。最后看迭代器在这副骨架上能做什么、不能做什么。知识点 8。

---

<a id="id8"></a>
## ✅ 知识点 8: Iterator 的能力与代价

**骨架、读写都齐了。收尾问题：在这副只有前向指针的骨架上，"遍历"能做什么、要付什么代价？答案一句话：前进免费，后退昂贵。**

**能力清单**（定义 [:170-227](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L170-L227)）：

- `Next()`：`node_->Next(0)`，第 0 层走一步，O(1)（[:447-457](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L447-L457)）
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

注释说得很直白：**不存 prev 链接，退一格就重新查一次**。`SeekForPrev` = `Seek` + 必要时 `Prev` 回退（[:528-538](https://github.com/facebook/rocksdb/blob/e6a2ee0bd211489e64a45a6a0f6ce1dc67e195d7/memtable/inlineskiplist.h#L528-L538)）。

**这笔账值不值？** 存反向指针的代价：每节点每层多 8 字节——按平均 1.33 层算，每节点指针开销从 ≈10.7 字节涨到 ≈21.3 字节，**直接翻倍**，而这正是知识点 1 的布局设计拼命省下来的东西。RocksDB 的取舍：让低频的 Prev 每次付 O(log N) 搜索费，保住高频路径的内存。

> 💡 **理解技巧**：设计取舍的一般形式——**给低频操作付时间，给高频路径省内存**。

---

## 📌 面试速记版

| 面试题 | 一句话答 |
|---|---|
| RocksDB 跳表节点怎么省内存？ | 倒装布局：高层指针存 header 前（负索引访问）、key 贴 header 后；不存 height（链入后由访问层数隐式得知）；平均每节点仅 ≈10.7 字节指针开销 |
| 为什么节点内存用 Arena 而不是 new？ | 节点大小随机不均，逐个 new 必碎片；Arena 推指针分配 O(1)、无碎片、缓存友好、整表一次性释放 |
| 层高怎么随机？ | `rnd->Next() < 阈值(2³²/4)`，一次整数比较 = 一次 1/4 概率抛硬币；期望 4/3 层，软上限 12、硬上限 32 |
| max_height_ 为什么敢 relaxed？ | 从更高层开始找只是多走空层——"只影响效率，不影响正确性" |
| 并发读为什么不加锁？ | 三支柱：release/acquire 配对保证"发布即完成"、节点链入后不可变、Arena 保活无悬垂无 ABA |
| 并发插入怎么做？ | Splice 括号定位 → 逐层 ①relaxed 备料 next ②CAS 发布 prev→next；失败只重算当前层括号重试；第 0 层统一查重 |
| 为什么 Prev() 是 O(log N)？ | 不存反向指针（每节点内存翻倍不划算），退一格 = 一次 FindLessThan 重搜 |

**记忆口诀**：**"指针倒装 key 贴后，Arena 推针不回收；抛币一次比阈值，表高懒长 relaxed；备料 relaxed 发布 release，读取 acquire 交换 CAS；Prev 重搜省指针，三柱撑起无锁读。"**

---

**下一站**：跳表这个容器拆完了。但它不单独存在——包着它的 MemTable 还管着跳表不管的事：什么时候写满冻结、active/immutable 怎么切换、内存账记到多少触发 flush、并发写怎么组织。→ 03-memtable（待写）
