# KopiDB 当前任务：修复 A1～A4

最后更新：2026-10-31。

目标：修复 [AUDIT.md](AUDIT.md) 中已经复现的 A1～A4，使四项问题通过行为验收，并通过相关存储与事务回归。用户已确认五阶段计划；默认由用户手写业务代码和头文件，助手提供逐函数实现、补充测试并维护记录。2026-10-31 用户单独授权助手完成 A1 三个文件工具函数，其余阶段仍沿用手写流程。

**修复进度：20%（1/5 阶段验收通过）**。按验收阶段数计算，不代表工时比例。

当前：阶段① A1 已由助手按授权修复并验收，相关回归 197/197 通过。下一问题为 A2，待用户指令后先交付 issue，再逐函数推进；没有开始修改 A2。证据与本轮未运行范围见 AUDIT。

| 阶段 | 工作 | 状态 |
| --- | --- | --- |
| ① [A1](AUDIT.md#a1) | 文件写入失败逐层上报 | 已验收：197/197 相关回归通过 |
| ② [A2](AUDIT.md#a2) | 目录 5.1.2「事务管理器」的混合隔离读取修复 | 待开始 |
| ③ [A3](AUDIT.md#a3) | compaction 收尾不构建空 SST | 待开始 |
| ④ [A4](AUDIT.md#a4) | 大记录后不同 key 正常换块 | 待开始 |
| ⑤ 整体验收 | 相关回归及修复记录收尾 | 待开始 |

## 本轮执行约定与边界

- 每进入 A1、A2、A3、A4 中一个新问题，先在聊天中写 issue，由用户手动发布；正文包括位置、触发过程、预期与实际差异、修复范围和验收标准。不自动创建远程 issue，也不要求用户必须先发布才能继续。
- 逐函数给出职责、调用链、带中文注释的完整实现；用户写好后检查并做针对性验证，按用户指令继续。每轮展示全部五阶段状态及完成百分比。
- A2 属于目录 **5.1.2「事务管理器」**（文章标题为 Lab 5.2）正文 3.2.3 get 的现有读取问题；目录 **5.2「WAL 和崩溃恢复」**尚未开始，本轮不实现日志编解码、WAL、恢复和初始化整合。
- 保留现有持久化格式及同 key 编码超限抛异常的约定；本轮不安排 iterator 重构或读未提交 put 的冲突策略重设计。
- 新发现先登记 AUDIT；只有确属本轮必要依赖才说明原因并纳入 PLAN。问题事实和历史复现日志保留在 AUDIT，项目全局约定见 STATE。

<a id="stage-a1"></a>

## 阶段① A1：文件写入失败逐层上报

已按本次单独授权由助手修改，三个函数均补充中文注释：

- [x] [StdFile::write](../src/utils/std_file.cpp)：使用写入定位并检查定位、写入结果，不能无条件返回成功。
- [x] [StdFile::create](../src/utils/std_file.cpp)：传递初始内容写入失败。
- [x] [FileObj::create_and_write](../src/utils/files.cpp)：检查创建／写入与最终流刷新结果，失败时抛异常。

仅联动复核：`SSTBuilder::build()`、`MemTable::flush_last()`、`LSMEngine::flush()` 的异常传播与成功后移除／登记顺序；目前不列为必改函数。

验收：已有 `TranCommitTest.AuditFlushWriteFailurePreservesMemory` 通过；补充必要断言，确认失败后原内存记录仍可读、失败 SST 未登记；正常文件读写和相关 SST 构建通过。测试仅在子进程限制文件长度，不影响父进程；不据此宣称 fsync 或断电恢复能力。

2026-10-31 验收通过：故障测试还确认解除文件长度限制后可重试刷盘，值与版本不变；File/Bloom 15/15、MemTable 49/49、SST 适用范围 28/28、LSM 适用范围 105/105。仅修改三个函数的逻辑，事务状态保存处修正一条过时注释；上层刷盘逻辑保持不变。A2～A4 及后续章节用例未计入本阶段通过数。

<a id="stage-a2"></a>

## 阶段② A2：事务读取隔离

依次处理：

- [ ] 在 [TranManager](../include/lsm/transaction.h) 新增读取状态查询，拟名 `should_skip_read()`，签名在该阶段细化；在同一次加锁中检查活跃与回滚集合，不改变现有 `is_aborted()` 的含义。
- [ ] [LSMEngine::get](../src/lsm/engine.cpp)：根据本次读取规则构造过滤条件，复用底层已有回调，在选择胜出版本之前过滤；保留既有调用行为。
- [ ] [TranContext::get](../src/lsm/transation.cpp)：自身修改优先，按隔离级别选择读取规则，首次合法读取之后再写入读缓存。

必要联动：事务与 Engine 头文件声明；只读复核 `new_tranc()` 和结束登记的生命周期，以及提交冲突查询与新读取规则的区分。底层 MemTable、SST、iterator 先复用已有回调，不列为必须重写。

验收：恢复用例 `TranReadVisibilityTest.RejectsDirtyValuesAndTombstonesBeforeCaching` 通过；覆盖内存与 SST 中未提交值／墓碑、有效旧值／旧墓碑、自身修改、提交后 RC 再读、回滚后读取、RR 重复读取与首次不存在缓存；RU 保持允许脏读。验证现有 SERIALIZABLE 单键读取路径，不宣称完整串行化。

<a id="stage-a3"></a>

## 阶段③ A3：compaction 收尾

- [ ] 修改 [LSMEngine::gen_sst_from_iter](../src/lsm/engine.cpp)：以当前 builder 是否真正加入记录判断收尾，不用包含空块开销的字节数判空。

验收：`TranCommitTest.AuditCompactionDoesNotBuildEmptyTail` 通过；覆盖最后一批已完整输出、仍有剩余记录、空输入的收尾行为，确认数据与版本保留，同 key 的版本不跨 SST 切分。

<a id="stage-a4"></a>

## 阶段④ A4：不同 key 正常换块

- [ ] 修改 [Block::add_entry](../src/block/block.cpp)：先区分本条记录无法编码与当前块容量不足，调整容量和旧块编码边界的检查顺序。

仅联动复核：`SSTBuilder::add()` 收到 false 后结束旧块、创建新块并重试的行为。

验收：`SSTTest.AuditDifferentKeyCanStartBlockAfterLargeEntry` 通过；大记录后不同 key 可以正常读回；单条长度超限和同 key 强制写入编码超限仍抛异常，不改变文件格式。

<a id="stage-regression"></a>

## 阶段⑤ 整体验收

- [ ] 运行本轮新增／既有缺陷回归，以及相关 LSM 事务与存储用例。
- [ ] 回归 SkipList、MemTable、Block、SST、File/Bloom 工具、BlockCache、BlockMeta 的已完成能力；测试在隔离临时目录执行，保存构建与运行证据。
- [ ] 明确列出因后续章节未实现而未运行的用例；保留源码，不删除有效断言或跳过本轮缺陷测试。
- [ ] 在 AUDIT 回写每项实际解决结果及证据；在 STATE 记录项目能力与验收范围，再收尾 PLAN。

完成标准：四项问题的行为验收及相关回归全部通过。历史 247/247 只代表此前范围，不替代本轮结果；仅写好代码或 issue 不计为阶段验收完成。
