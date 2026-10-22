# KopiDB 项目状态

最后核对：2026-10-23。下一步与待办见 [PLAN.md](PLAN.md)。

本文件记录当前事实、已确认的设计、全局约定、验收边界和延期事项。代码与记录不一致时，先检查代码，再更新记录；不能把讨论通过的方案当作已经实现。[PLAN.md](PLAN.md) 只记录本轮任务、执行顺序、TODO 与具体的验收场景。

## 当前进度

正在进行 **Lab 5.2：事务上下文与事务管理器**。此前已经推进过 SkipList、MemTable、Block、SST、迭代器和 Engine 的 MVCC 改造，当前继续接入事务状态判断，整个数据库的事务功能尚未验收完成。

当前任务：实现 **按事务 ID 标记回滚，并在查询时排除该事务的版本**。回滚集合、状态查询接口、结束登记、abort 及写入路径的旧方案清理已完成；存储查询过滤尚未完成。

当前推进项：第①、②阶段已完成；第③阶段的底层点查链和 Engine::get 已接通回滚判断。2026-10-23 检查及相关回归 46/46 通过，下一函数为 Engine::get_batch。仍按单函数推进，业务代码由用户手动加入。

2026-10-22 SST 检查：SST::get 已将回调正确传给按 key 构造的 SstIterator，范围/Bloom 检查、读取上限和有效墓碑处理正确。扩展现有测试到公开 get 入口，完整 test_sst 19/19 通过。随后已接通 Engine 候选 SST 查询及 get。

2026-10-23 最新 Engine 检查：get 使用严格的管理器前置检查，由回调持有 shared_ptr，并向内存、SST 同时传递回滚判断。新增 4 个测试，相关回归 46/46 通过；此前两个真实 abort 点查失败均已解决。13 个已有测试补充显式绑定，旧管理器失效用例按新约定检查 get 报错并保留底层无写入断言。当前 get 尚留两处过时的未绑定兼容注释，已提醒用户清理；执行逻辑正确。助手未修改业务代码。

当前 SkipList::get、MemTable::get/get_record/get_batch、Block::get_idx_binary/get_value_binary、BlockIterator/SstIterator 按 key 构造、SstIterator::seek、SST::get 及 Engine::sst_get_ 在显式传入回调时会排除已取消版本，保留有效墓碑，相关测试通过。Engine 的公开 get 已从管理器取得并传入回调；get_batch 和 commit 尚未接通。MemTable::get 返回的原节点迭代器、BlockIterator/SstIterator 按 key 构造及 seek 均不保存回调，后续 ++ 不自动沿用本次筛选；get_record/get_batch 在锁内复制结果。数据库回滚查询尚未完成。

2026-10-23 已确认管理器约定：Engine 公开查询必须有有效的管理器，未绑定和已销毁均报错；不再保留未绑定兼容分支，不需要 owner_before。get 已按此实现并通过缺失/失效测试，回调持有 shared_ptr 到查询结束；查询期间的持有关系经过代码检查，未添加生命周期并发压力测试。get_batch 草案沿用此约定，即使空输入也先检查依赖。Lab 5.5 的生产入口绑定仍按原计划延期，当前测试显式绑定，不以此降低业务查询的要求。

2026-10-20 用户纠正任务范围：撤回把 LSM 构造函数绑定作为当前下一步的安排，组件初始化整合按 Lab 5.5 推进。绑定是引擎使用管理器的依赖，但现有事务测试已显式完成绑定，可以先验证回滚逻辑；本轮不以修复 LSM 初始化入口为前置任务，也不把测试绑定视为生产入口已完成。

当前排期约束：项目时间紧，优先沿现有结构完成回滚。迭代器职责重构已经讨论，但列为后续事项，本轮只做必要的判断接入与参数传递。

| 位置 | 当前代码状态 |
| --- | --- |
| [TranContext 构造、get、commit](../src/lsm/transation.cpp) | 已按此前讨论实现；接入回滚状态后还需调整有关调用。 |
| `TranContext::put()` | 已删除旧值查询和备份，更新函数说明，代码检查、构建及对应回归通过。 |
| `TranContext::remove()` | 已独立实现结束状态检查、DELETE 记录、立即/暂存墓碑；新增 4 个测试通过，覆盖重复删除、历史版本、与 put 交替及结束后拒绝删除。 |
| `TranContext::abort()` | 已实现状态检查、回滚登记及上下文清理；删除旧容器清理后，5 个当前阶段的回滚测试再次通过。Engine 点查询已接入回滚名单，批量与范围路径仍待接入。 |
| `TranManager::getNextTransactionId()`、`new_tranc()` | 已实现 ID 分配、上下文创建和活跃事务登记。 |
| [TranManager 成员](../include/lsm/transaction.h) | 已新增 `aborted_tranc_ids_` 和加锁的 const 查询接口 `is_aborted()`；回滚登记已接入并通过对应测试。 |
| `add_ready_to_flush_tranc_id()` | 同锁更新待刷盘状态与回滚集合；map 更新失败时撤销本次新增的回滚 ID。正常、重复、清理后保留及并发登记测试通过，分配失败分支通过代码检查。 |
| `add_flushed_tranc_id()` | 会清理待刷盘状态表；该表不能充当长期回滚名单。 |
| `write_tranc_id_file()`、`read_tranc_id_file()` | 当前均为空函数；正常重启后的状态与 ID 延续尚未完成。 |
| [LSM 构造函数](../src/lsm/engine.cpp) | 已创建 Engine 和 TranManager，互相绑定仍为待办，按用户要求留在 Lab 5.5 初始化整合时处理；当前事务测试自行绑定。 |
| 查询与迭代器 | SkipList、MemTable 点查/批查、Block/SST 点查链及 Engine::sst_get_ 支持显式回调并通过测试。Engine 公开 get 已接通；get_batch、事务缓存与提交检查、范围迭代器仍待接通。 |

## 已确认的设计

1. **读未提交继续立即写入。** `TranContext::put/remove` 使用本事务 ID 写入存储层；其他隔离级别先暂存，提交时应用修改。
2. **回滚名单由 TranManager 管理。** 采用普通 `std::unordered_set<uint64_t>`，读写均受现有互斥锁保护。当前先接受检查时加锁的成本。
3. **回滚通过排除版本实现。** 事务 5 回滚后，所有 `tranc_id == 5` 的记录都不再作为查询候选，覆盖内存和 SST。原记录可以暂时保留。
4. **继续比较真实版本号。** 保留现有跨 MemTable、SST 选择最大可见版本的逻辑；不改成找到第一个 key 就返回。
5. **先过滤回滚版本，再去重和处理墓碑。** 例如 `k@5 = new` 已回滚、`k@3 = old` 有效，应返回 `old@3`。版本上限为 0 只表示不限制版本大小，仍须排除回滚版本。
6. **Block/SST 记录编码保持现状。** 回滚 ID 随事务状态文件保存。刷盘登记完成不代表旧记录已被物理删除，因此不能顺便清掉回滚名单。
7. **读未提交 put 的冲突检查暂缓。** 当前先完成回滚主线；后续再讨论是否增加这项约束。
8. **迭代器重构不作为本轮前置任务。** 保留现有迭代器组合与推进逻辑，在去重之前接入必要的回滚判断；不要求每个迭代器各新增一套过滤。此前讨论的统一 MVCC 迭代器仍是延期的重构方向。

不采用“重新写入旧值/旧版本”作为回滚方案：它不能可靠消除已经写入的较新版本。`rollback_map_` 成员及所有业务引用已删除，相关旧值备份说明已清理。

2026-10-20 已核对 Lab 5.3～5.5：Lab 5.5 的“情形3：事务回滚”确实再次使用作者的 rollback_map_，但明确说明还原方式取决于前面 Lab 的实现，并非必须保留该成员。我们继续按事务 ID 排除版本，因此删除旧值备份及其引用；operations、WAL 的提交/回滚记录仍需保留。后续恢复必须接通持久化事务状态与查询过滤，不能仅跳过 WAL 重放就认为已落入 SST 的取消或未完成版本已失效。参考：[Lab 5.5](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.5-Recover.md)。

## 验收边界与待确认事项

- TranContext 按事务创建：每次 begin_tran/new_tranc 返回新上下文，不是按线程缓存的 thread_local 对象，也没有绑定线程的机制。一个线程可以创建多个事务，shared_ptr 也能被传给其他线程。
- 当前 TranContext 没有内部线程同步，同一实例的方法及成员访问必须由调用方串行执行；管理器的锁不保护上下文的 isAborted、temp_map_ 等成员。2026-10-20 核对作者的 transaction.h 与 test_lsm.cpp：上下文无 mutex，事务测试顺序操作句柄；这些代码没有证明“每个线程自动独占一个上下文”。本地 commit/abort 的串行使用注释来自此前助手草案，不是已经核实的 Lab 明文规定。
- 当前保留上述使用限制；2026-10-20 已核对用户加入的类接口说明，明确外部同步与跨线程移交要求。该说明不表示“自动线程隔离”，也不能将同一上下文并发安全列为已验收。若以后要求并发操作同一上下文，再统一同步 put/remove/get/commit/abort 及相关成员访问，而非仅锁 abort。
- 只添加集合或实现 `abort()`，还不能宣称回滚完成；点查、批量查询、范围遍历和提交冲突检查都需要接通。
- `TranContext::get()` 的 `read_map_` 可能绕过存储层查询，需要检查缓存值和缓存的“不存在”结果如何与事务状态配合。
- 混合隔离级别下，读已提交、可重复读对其他事务未提交数据的可见性仍需处理。仅排除 ABORTED 不等于已经实现“只读已提交”。
- 当前 `SERIALIZABLE` 有单键读记录校验，不代表已经实现完整串行化或范围幻读保护。
- 当前提交路径跳过 WAL 持久化；WAL 和崩溃恢复按后续 Lab 推进。
- Block 编码超限沿用已确认的抛异常方案，长期容量设计另行讨论。

## 全局排期与延期事项

- **以 Lab 章节顺序为主线。** 当前推进 Lab 5.2 的事务回滚，不能因发现其他模块未完成，就把后续 Lab 的任务插入当前步骤。每个拟修改函数都应说明它与当前验收目标的直接关系；必要依赖先解释清楚，无关缺陷、优化和重构记入全局待办，不自行扩大任务。
- 后续章节顺序：完成 Lab 5.2 的事务逻辑、状态维护及对应验收，再进入 Lab 5.3 Record、Lab 5.4 WAL、Lab 5.5 恢复与初始化整合。骨架已有函数体不代表已经按我们的流程实现、验收。
- 优先完成当前 Lab 功能。沿用现有迭代器组合、接口拼写与 Block/SST 编码；只对本轮需要的调用链进行局部接入，小型辅助函数仅在直接减少重复代码时引入。
- 每完成一项功能运行对应测试，接通后做受影响范围的回归；没有新改动或失败线索时，不重复全库排查。
- 新发现的问题先判断是否阻碍当前验收；无关问题放入全局待办。有效旧值丢失、取消墓碑继续遮住旧值等问题必须在对应任务中修复。

以下事项延期，统一在本文件维护，不放进当前 PLAN：

| 事项 | 当前安排 |
| --- | --- |
| 迭代器职责重构、集中 MVCC 筛选 | 当前回滚功能接通后另行评估，详见下方议题一。 |
| 同 key 写入版本单调约束与读取优化 | 与迭代器重构一并讨论，详见下方议题二。 |
| 同一 TranContext 被并发访问的风险 | 当前由调用方保证串行使用，类接口说明已补充；是否支持内部同步留待后续评估，详见下方议题三。 |
| 回滚名单查询性能优化、共享只读快照 | 先采用普通集合加锁方案，性能优化另行评估。 |
| 回滚版本物理清理与回滚 ID 安全回收 | 后续设计；当前名单不能因刷盘登记结束就删除。 |
| LSM 构造函数的组件初始化整合 | 留到 Lab 5.5；当前回滚测试显式绑定 Engine/TranManager。生产入口接通后另做集成验收。 |
| WAL 写入、日志重放和崩溃恢复 | 按后续 Lab 推进；恢复流程需适配事务 ID 回滚方案，覆盖取消/未完成版本已落入 SST 的情况，与当前正常关闭、重启验收区分。 |
| Block 长期容量设计 | 保留当前编码超限抛异常方案，长期方案继续讨论。 |

## 延期 issue

以下议题均延期，不阻塞当前回滚主线。这里保存 issue 草案与讨论边界，GitHub 编号和链接待用户发布后补充。

### 议题一：明确迭代器职责，集中管理 MVCC 可见性与版本筛选

- 状态：已在聊天中提供 issue 草案，重构尚未启动。
- 位置：[BlockIterator](../src/block/block_iterator.cpp)、[HeapIterator](../src/iterator/iterator.cpp)、[TwoMergeIterator](../src/lsm/two_merge_iterator.cpp)、[Level_Iterator](../src/lsm/level_iterator.cpp)。
- 问题：多个迭代器同时承担推进、版本过滤、同 key 去重或墓碑处理；规则与执行位置分散，新增回滚判断容易遗漏，也难以确认下层是否已经丢掉旧版本。
- 例子：`k@5` 已取消、`k@3` 有效；若下层先去重，上层再过滤 5，就可能漏掉应返回的 3。
- 待讨论方案：明确原始遍历、归并排序、用户查询 MVCC 筛选的职责，评估集中筛选的设计；不是已经决定重写全部迭代器。
- 验收目标：点查、批查、范围查询保持一致；历史版本、墓碑、来源优先级和回滚场景正确；compaction 保留必要版本，并评估性能与内存开销。
- 当前处理：沿用已有结构接通回滚判断，不把重构作为前置条件。

### 议题二：评估写入版本单调约束，以简化读取时的跨来源比较

- 状态：设计评估待办，尚未决定实施写入约束或简化读取；GitHub 链接待补充。
- 核心问题：目前可能先写 `k@5` 并刷入 SST，再把 `k@3` 写入新的 MemTable。较新的来源未必含有较大的版本，因此命中 MemTable 后仍需查询 SST 并比较真实版本号。这是此前多处跨来源比较修补共同应对的问题。
- 优化目标：在实际写入时保证同一个 key 的有效写入版本不倒退；例如已有有效 `k@5` 后，拒绝迟到的 `k@3`。这里是同 key 的约束，不要求不同 key 的 ID 也按顺序写入。
- 预期收益：若这个约束覆盖所有参与查询的数据，且冻结、刷盘、compaction 保持来源先后关系，那么按照活跃表、冻结表从新到旧、L0 从新到旧、L1 及更深层的顺序，遇到第一个可见记录即可决定结果。命中有效墓碑时也可以确定“不存在”，无需继续查找更深来源的较大版本。
- 可能简化的位置：[MemTable 的 frozen_get_/get_/get_batch](../src/memtable/memtable.cpp)、[Engine 的 get/get_batch/sst_get_](../src/lsm/engine.cpp)，以及 [Level_Iterator](../src/lsm/level_iterator.cpp)、[TwoMergeIterator](../src/lsm/two_merge_iterator.cpp) 等普通查询归并中的同 key 来源选择。逐项确认适用条件，不直接删除全版本 compaction 的排序与输出逻辑。
- 当前事实：[TranContext::put/remove](../src/lsm/transation.cpp) 的读未提交分支直接写入，没有写冲突检查；暂存型事务已有 commit 阶段的检查。普通 LSM 的直接、批量写入入口不经过 TranContext::put。
- 检查时机：读未提交需要在实际写入时检查；暂存型事务不能仅在 put 时预检，因为 put 到 commit 之间数据库仍会变化，实际应用修改前的检查必须保留。
- 并发与覆盖要求：检查与实际写入必须正确同步，覆盖所有相关写入入口，包括删除墓碑和批量操作；随后验证冻结、刷盘、compaction 保持用于提前返回的顺序约束。
- 存量数据：新增检查不会自动修正数据库中已经存在的来源与版本倒置；优化读取前需明确旧数据兼容、整理或回退策略。
- 待讨论的写入语义：拒绝低版本写入时如何报告失败、事务如何结束；如何对待已取消版本、其他活跃事务和有效墓碑。
- 与另一个问题的区别：本议题聚焦“首次命中的可见来源能否直接胜出”，不是取消快照可见性筛选。每个来源仍先找出对本次读取可见的候选，之后才应用来源优先的优化。
- 验收方向：证明并测试同 key 有效版本的写入约束；对比优化前后的查询结果与候选来源访问次数，覆盖并发、删除、取消、历史快照、跨层与 compaction 场景，保留必要回归。
- 当前处理：现有代码尚未具备该全局保证，继续保留跨来源比较；本轮先完成回滚，该议题延期。

### 议题三：明确 TranContext 的并发访问边界，评估内部同步保护

- 记录日期：2026-10-20。状态：潜在并发风险，尚未实施内部同步，也未发布 GitHub issue。
- 位置：[TranContext 类定义](../include/lsm/transaction.h)、[put/remove/get/commit/abort](../src/lsm/transation.cpp)。
- 当前事实：每个事务有独立上下文，但没有自动线程绑定；同一个 shared_ptr 可以被传给多个线程。上下文的状态、缓存和暂存容器没有自身互斥保护。
- 触发条件：多个线程未做外部同步，同时操作同一个上下文或直接访问其公开成员；不同线程各自使用不同上下文不属于这一问题。
- 影响：例如 put 插入 temp_map_ 时 abort 同时清空它，会产生数据竞争；commit 与 abort 并发还可能破坏事务结束状态的一致性。TranManager 和存储层的锁不能代替上下文同步。
- 当前处理：用户已补充类接口注释并经核对，明确同一实例必须由调用方串行使用，跨线程移交需正确同步。注释表达使用限制，不代表代码强制执行，也不代表并发风险已消除。
- 后续评估：是否支持同一上下文的并发调用；如果支持，统一保护状态检查、容器访问及结束状态更新，并处理公开成员绕过保护的问题，不能仅给 abort 加锁。检查新增锁与管理器、存储层锁的顺序。
- 验收方向：若实施同步，补充同一上下文 put/abort、get/abort、commit/abort 等并发用例，验证无数据竞争、结束状态唯一、结束后不再接受修改，并检查不同上下文仍可并发；条件允许时使用 ThreadSanitizer。
- 当前排期：作为全局延期事项保存在 STATE，不加入当前回滚 PLAN 的实施步骤。

## 测试状态

- 最新检查（2026-10-23）：`xmake build test_lsm` 成功，独立临时目录运行相关测试 **46/46 通过**。新增 `LSMTest.EngineGetRequiresLiveManager` 和 3 个 `EngineGetRollbackTest`，覆盖严格依赖检查、取消值/墓碑、活跃表/冻结表/SST、真实版本与同版本来源优先级、连续回滚、历史读取。复查原有点查/批查基础回归、SST 回滚、刷盘及事务创建/读写/提交/删除/取消测试。旧批查回归没有取消版本，因此不代表批量回滚已完成。未运行完整 test_lsm、其他存储套件或 Lab 5.5 入口。
- 最新日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-engine-get-rollback-0eu767bl/result.log`；同目录 `filter.txt` 保存本轮完整 gtest 过滤器。
- 前一轮检查（2026-10-22）：新增 4 个 EngineRollbackSst 测试：过滤后跨 L0/L1/L2 比较及有效墓碑、候选全部取消后继续查找及不清空已有 best、同版本来源优先级、空集合/缺失/回调异常。包含读取上限 0 和历史上限、L1 二分两侧、取消墓碑、取消名单在两次查询间变化和默认空回调行为。`xmake build test_lsm` 成功，独立临时目录复制配置后运行 `LSMTest.EngineRollbackSst*` 及此前 3 个 EngineMvcc 读取回归，**7/7 通过**。未运行完整 test_lsm、真实 abort 查询测试或其他套件，未修改业务代码。
- 前一轮日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-engine-sst-rollback-s_no42vs/result.log`。
- 前一轮 update_best_from_sst 检查（2026-10-22）：实现检查、构建及既有空回调读取路径 **3/3 通过**；当时非空回调尚未接通。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-candidate-review-mfm7c3sa/result.log`。
- 前一轮 SST::get 检查（2026-10-22）：扩展原有 3 个回滚测试及 EndDoesNotReadBlocks，公开入口完整 SST **19/19 通过**，test_lsm 构建通过。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-get-rollback-r4d0s4ki/result.log`。
- 前一轮 SstIterator 按 key 构造检查（2026-10-22）：扩展原有 3 个回滚测试到构造入口，完整 SST **19/19 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-key-ctor-kggoiq63/result.log`。
- 前一轮 seek 修正检查（2026-10-22）：catch (...) 同时重置块编号和块内指针，再重新抛出异常；标准和非标准异常测试及完整 test_sst **19/19 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-seek-fixed-k6df6mic/result.log`。
- 前一轮 seek 检查（2026-10-22）：新增 3 个测试，覆盖重新打开 SST、回滚过滤与读取上限、两种遍历模式、有效墓碑、重复 seek 缓存更新、全部取消/缺失/空 SST 句柄、异常上报与后续再次定位。曾为 18/19，唯一失败断言是异常后的 `it == sst->end()`，现已解决。旧日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-seek-rollback-upp8ca__/result.log`。
- 前一轮 BlockIterator 检查（2026-10-22）：按 key 构造正确转发回调，空指针抛 invalid_argument，未命中定位块尾。扩展已有 5 个回滚用例并新增空指针测试，**Block 24/24、SST 16/16 通过**；未验证 ++ 的回滚过滤。
- 前一轮日志：Block `/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-key-ctor-test_block-b2izzsjo/result.log`；SST `/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-key-ctor-test_sst-pblbxc4g/result.log`。
- 前一轮 get_value_binary 检查（2026-10-22）：用户补上漏传的回调，扩展已有 5 个回滚测试到取值入口后，完整 test_block **23/23 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-value-fixed-xk1s5jsn/result.log`。
- 前一轮补参前，5 个 RollbackPoint 测试因取值接口忽略回调而失败，补参后全部通过。旧日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-value-review-mpuxy6o7/result.log`。
- 前一轮 get_idx_binary 检查（2026-10-22）：新增 5 个回滚测试，完整 test_block **23/23 通过**；当时尚未测试取值入口的回滚过滤。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-index-rollback-imu5tn3s/result.log`。
- 前一轮 Block 检查（2026-10-21）：修正 while 条件后，完整 test_block **18/18 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-adjust-fixed-e8ntbyst/result.log`。
- 已解决的上一轮回归：BlockTest.DecodeTest、EncodeTest、RejectsOffsetOverflowWithoutChangingExistingRecords、ForceWriteWithinEncodingLimitsStillSucceeds、MvccPointReadSelectsNewestVisibleVersion、MvccTombstoneDoesNotFallBackToOlderValue、MvccEncodingPreservesFullWidthTransactionIds 曾因同一个循环条件错误失败，本轮均通过。旧日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-adjust-review-gcsp5sbr/result.log`。
- 前一轮 get_batch 检查（本轮 Block 修改之前）：新增 3 个批量测试，覆盖跨表取消值/墓碑、版本上限、顺序和重复 key、逐条查询一致性、空结果和副本稳定性。test_memtable、test_lsm 构建成功，完整 MemTable **41/41 通过**，test_lsm 只构建未运行。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-get-batch-rollback-ae5vyu8v/result.log`；其中 missing/fail.sst 打开失败来自预期的刷盘失败测试。
- 前一轮 get_record 检查：扩充跨表回滚测试并新增副本与空结果测试，完整 MemTable **38/38 通过**，test_lsm 构建通过。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-get-record-rollback-vaop21p9/result.log`。
- 前一轮公开 get 检查：新增 7 个回滚点查测试，完整 MemTable **37/37 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-memtable-get-rollback-ohi4rqe3/result.log`。
- 前一轮 get_ 检查（2026-10-21）：test_lsm 与 test_memtable 均构建成功，MemTable **30/30**、`TranCommitTest.*` **10/10** 通过。本轮没有重跑提交测试，commit 的回滚判断仍待接入。对应日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-get-internal-memtable-942g3zya/result.log`、`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-get-internal-commit-ompvxjq9/result.log`。
- 上一轮 SkipList 检查（2026-10-20）：用户修正读取上限条件后，构建及完整 test_skiplist **29/29 通过**，包含全部 6 个回滚测试。此前上限条件导致的 3 个失败已解决。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-skiplist-abort-fixed-djbup06n/result.log`。
- 上一轮 LSM 检查（2026-10-20）：remove 的删除路径及结束状态检查正确；搜索 src/include/test 没有 rollback_map_ 引用，`xmake build test_lsm` 成功。独立临时目录中运行当时已实现范围的测试，**36/36 通过**；这是本次 SkipList 修改之前的结果，未修改原有失败用例的断言。
- 上一轮 LSM 过滤器：`--gtest_filter='LSMTest.TranContextConstructor*:LSMTest.TranContextPut*:LSMTest.TranContextGet*:LSMTest.NewTransaction*:LSMTest.TransactionEndRegistration*:TranCommitTest.*:TranRemoveTest.*:TranAbortTest.Cancels*:TranAbortTest.ExpiredManager*:TranAbortTest.ReadUncommittedRegisters*:TranAbortTest.CannotAbort*'`。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-remove-review-kcekzes_/result.log`。
- [test/test_lsm.cpp](../test/test_lsm.cpp) 已有事务创建、读写、提交、结束登记等测试，以及 `TranAbortTest` 的 7 个回滚用例和 `TranRemoveTest` 的 4 个删除用例。
- 本轮新增删除测试：暂存删除到提交后生效；读未提交立即写墓碑但保留历史版本；put/remove 交替取最后一次操作；实际 commit/abort 后拒绝删除且无副作用。同步扩充暂存取消测试，加入结束后的 remove 检查。
- 2026-10-20：新增 3 个 abort 用例，覆盖空事务取消、管理器失效时保留原状态、读未提交取消不改写物理记录；扩充暂存取消用例，检查暂存删除、清空容器、重复调用及已结束的 put/get/commit。已提交事务取消还检查管理器未误记回滚。
- 前一轮较宽范围验证：在独立临时目录复制配置后运行 `test_lsm --gtest_filter='LSMTest.TranContext*:LSMTest.NewTransaction*:LSMTest.TransactionEndRegistration*:TranCommitTest.*:TranAbortTest.*'`，**35 个测试：32 个通过，3 个失败**。其中两个回滚点查失败已于 2026-10-23 解决并通过；LSM 初始化入口仍留在 Lab 5.5，本轮未重跑该入口用例。
- 失败一：`LSMTest.TranContextTest` 抛出 `TranManager::new_tranc: engine is not initialized`；LSM 构造中的组件绑定尚未实现，按用户要求留到 Lab 5.5 初始化整合，不列为当前回滚阶段的修复项。
- 已解决的失败二、三：`TranAbortTest.ReadUncommittedRestoresPreviousVersionInMemoryAndSst`、`TranAbortTest.KeepsNewerCommitButHidesAbortedVersionFromOlderSnapshot` 于 2026-10-23 接通 Engine::get 后通过，保持原断言。历史验证日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-abort-context-review-738zvtgk/result.log`。
- 验证边界：本轮未运行完整 test_lsm；未注入内存分配故障，未启用 ThreadSanitizer。批量、范围、事务缓存/提交的回滚判断以及状态持久化仍待验收。

## 协作方式

- 用户手动修改业务源码及头文件；助手在聊天中给出带中文解释的实现和准确位置。
- 一次推进一个函数，检查完成后，等用户明确说“下一个”再继续。
- 每次展示计划或进度，都先列出全部六个阶段及其已完成、进行中、待开始状态，再标出当前阶段正在处理的函数；必要时补充当前阶段细项。该约定按用户 2026-10-22 最新要求更新，不再只在阶段结束时展示总进度。不能把单个函数通过写成整个阶段完成。
- Lab 指定函数即使已有实现，也要独立推导，并对照作者参考；有差异时说明理由。
- 助手可以直接补充测试、维护这两份文档，并执行相关检查。测试使用独立临时目录和配置，避免测试清理影响项目数据。
- Git 写操作由用户执行；“提 issue”默认指在聊天中起草内容。

## 全局验收与记录约定

- 每个阶段需满足行为标准并通过相应构建、测试和代码检查，才能认定完成；实现任务和测试任务分别勾选。
- 每轮验证已接通的功能，依赖后续工作的用例保持待验收；不通过跳过失败断言、降低预期或遗漏已知失败来宣称通过。
- 实现任务经检查后勾选，测试任务实际通过后勾选。记录日期、涉及文件/函数、测试命令、用例范围、通过/失败数量和未验证项。
- 未解决或延期的能力不得计入“已完成”，不承诺尚未评估的工时。

## 记录更新规则

实现检查完成、测试结果变化或设计决定改变时更新本文件。更新日期表示最后核对时间，不代表全部功能在该日通过验收。只保留当前有效结论；当前任务的顺序、TODO 和验收标准维护在 [PLAN.md](PLAN.md)。全局协作规则、项目状态和延期事项只在本文件维护。

方案发生变化时改写当前任务的待办，并同步本文件中的设计结论，不保留互相矛盾的执行安排。
