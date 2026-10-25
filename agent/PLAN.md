# KopiDB 当前任务计划：事务回滚

最后更新：2026-10-25。本文件只记录本轮任务、实施顺序和验收标准；项目状态、全局约定和延期事项统一见 [STATE.md](STATE.md)。

当前目标：完成 Lab 5.2 中的逻辑回滚，使事务取消后，查询能跳过其写入的版本并找到有效旧版本。

任务清单依据：[Lab 5.2](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.2-Tranc-ID.md)。回滚采用我们已确认的事务 ID 集合方案。

## 本轮里程碑

回滚判断由 `TranManager::is_aborted()` 提供，在现有版本筛选位置接入。六阶段按以下三个里程碑推进：

| 里程碑 | 对应阶段 | 可确认的成果 |
| --- | --- | --- |
| A：能够登记取消 | ①、② | 管理器记住回滚 ID，abort 正确结束上下文；暂不宣称存储查询已过滤。 |
| B：读写路径使用取消状态 | ③、④、⑤ | 单条、批量、范围查询以及事务内部路径满足各自验收项。缓存与混合隔离的未决项明确报告。 |
| C：正常重启后仍有效 | ⑥及最终回归 | 名单和 ID 正确恢复，已验证的存储行为没有回归。 |

## 六阶段总进度

**功能阶段进度：50%（3/6）。** 按功能验收通过的阶段数计算，不代表工作量或剩余时间；第③阶段还有两处旧注释待清理，第④阶段的局部完成暂不折算。

| 阶段 | 任务 | 状态 |
| --- | --- | --- |
| ① | 保存并查询回滚 ID | 已完成 |
| ② | 实现 abort，调整写入上下文 | 已完成 |
| ③ | 接通单条与批量查询 | 功能验收通过；get 的两处旧注释待清理 |
| ④ | 接通遍历、前缀和谓词查询 | 进行中：Level 构造的 Concact 调用漏传 false，构建待通过 |
| ⑤ | 接回事务读取缓存与提交检查 | 待开始 |
| ⑥ | 持久化状态与正常重启 | 待开始 |

## 当前推进项

**2026-10-25 检查 Level_Iterator 构造：声明/定义已接收回调，MemTable 与 L0 传递正确；src/lsm/level_iterator.cpp:89 创建 ConcactIterator 时漏了第三个参数 false，回调误放到 keep_all_versions 的位置。xmake build test_lsm 失败，构建结束后再次读取该行，仍为三参数调用。等待用户补为 (ssts, max_tranc_id_, false, is_aborted)；本轮未新增或运行测试，未修改业务代码。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-level-constructor-review-d182z7d3/build.log`。**

上轮 begin 测试覆盖首条与后续 key 的取消值/墓碑、有效墓碑、读取上限 0 与历史上限、回调独立保存、空块、全部取消及默认参数兼容。业务源码及头文件由用户修改，未降低既有断言。构建与测试日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-begin-rollback-efcgymdy`。

上一轮前缀测试覆盖连续取消值/墓碑、保留有效旧墓碑、首尾边界、读取上限 0 与历史上限、空前缀、空块、无匹配及全取消区间，原两参数调用保持兼容。业务源码及头文件由用户修改，未降低既有测试断言。构建与测试日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-prefix-rollback-368dh7bl`。

已解决：src/block/block.cpp 中两处 make_shared 调用补为 (shared_from_this(), first/end_idx, tranc_id, false, is_aborted)，函数前的区间说明已改为左闭右开。本轮测试验证右边界过滤后仍可到达、连续取消、有效墓碑、读取上限、空块/无匹配/全取消及默认空回调。成功构建与测试日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-predicate-fixed-ju3dbvki`；上一轮失败日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-predicate-build-rz0dsng4/build.log`。业务源码由用户修改，测试断言未降低。

上一轮新增 2 个 Block 测试，覆盖按 key 构造后的持续筛选、普通/全版本模式、读取上限 0 与历史上限、连续取消、有效墓碑、缓存、精确查询无结果，以及调用方清空回调后继续 ++。用户首次查找仍传 tranc_id/is_aborted 参数，与当前成员值一致，符合约定，无需机械改成草案的成员写法。业务源码及头文件由用户修改；构建与测试日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-key-callback-review-zjcug46y`。

上一轮新增的 4 个 Block 测试覆盖首次定位、后续 ++ 与缓存、回调对象独立保存、空块/尾后/全部取消；包含读取上限 0、连续取消的值和墓碑、有效墓碑保留及全版本模式。最初因初始化列表遗漏 is_aborted_(is_aborted)，4 个新增测试全部失败；用户补齐后重新构建，全部通过。构建与测试日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-index-verified-huwzqk31`。业务代码由用户修改。

此前已完成项复核：回滚登记与 abort、点查/批查回调传递、MemTable 三个范围入口与计划一致；没有发现新的阻塞问题。测试覆盖已完成阶段，未运行完整 test_lsm、WAL/恢复和其他未完成路径；该次复核未修改业务代码或测试断言。构建日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-plan-check-build-cl3hsb4p`；测试日志与 LSM 过滤器：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-plan-completed-review-gi26m8_i`。

2026-10-24 谓词约定复核：作者参考直接传递 predicate，本地接口与已核对的 Lab 5.2 均未要求谓词不能收到空 key。撤回额外 query_predicate 包装要求；保留收集结果时的标记过滤，谓词须能处理空字符串并满足单调性。上轮 48/49 中失败的 PredicateDoesNotPassMarkersToUserCallback 检查了助手额外引入的接口约束，不能据此认定当前查询结果有错；将其替换为 PredicateRangeResultsExcludeTransactionMarkers，验证含标记的活跃表/冻结表查询 [b, d) 只返回正确记录。构建成功，完整 MemTable 49/49；其他回滚、墓碑和 getter 测试断言未改，业务代码未改。

2026-10-23 本轮 iters_preffix 检查：声明与实现均接入可选回调，活跃表和冻结表收集正确，空 key 先于 get_cur_tranc_id 排除，入堆前筛选读取上限与回滚状态，保留有效墓碑，右边界每表只计算一次。新增 3 个前缀回滚用例，覆盖同表/跨表、取消值与墓碑、连续取消、历史上限、版本与来源优先级、空表/缺失/全部取消、空前缀及事务标记；补强原前缀测试的结果数量和索引边界断言。构建成功，完整 test_memtable 46/46 通过；未运行 test_lsm 或其他套件，助手未修改业务代码。

2026-10-23 本轮 begin 检查：用户已补齐 collect(*current_table, idx) 和 is_aborted(version) 判断，并以 std::move(items) 构造返回堆。空 key 先跳过，随后筛选版本上限与回滚状态；保留有效墓碑和同版本来源优先级。构建成功，完整 test_memtable 43/43 通过，包含上一轮失败的三个用例以及查询后刷盘仍报告事务标记的用例。助手未修改业务代码；本轮未运行 test_lsm 或其他测试套件。

2026-10-23 最新检查：get_batch 已取得一次管理器、创建整批共用的回调，并传给 memtable.get_batch 与 sst_get_；保留一次 SST 读锁、跨来源真实版本比较、最终墓碑处理、输入顺序和重复 key。最初代码检查发现 if (man) 写反，用户在本轮检查期间自行修正为 if (!man)，重新构建后验证最新版本。新增 2 个测试，扩展已有跨来源点查测试到批量入口；相关 48/48 通过。覆盖未绑定/失效及空批次、真实 abort 后活跃表/冻结表/SST、连续取消、历史上限、有效墓碑、重复与缺失 key。没有运行完整 test_lsm；助手未修改业务代码。

此前 MemTable 阶段：完整回归 41/41 通过，test_lsm 构建通过；该结果发生在本轮 Block 修改之前。此前失败的两个 Engine 回滚点查用例于 2026-10-23 接通 get 后通过。

已完成 cur_get_、frozen_get_、get_：同一回调传到底层 SkipList，过滤后继续比较各表的真实版本号；保留有效墓碑，相同版本优先较新来源，内部函数不重复加锁。这些行为已通过公开 get 的非空回调集成测试验证。

已完成公开 get：可选回调覆盖本次查找，返回原节点迭代器，保留有效墓碑及无效结果处理。锁只覆盖查找，迭代器不保存回调，后续 ++ 不自动沿用本次筛选。

已通过的 MemTable 点查测试覆盖：活跃表/冻结表/跨表的取消值与取消墓碑、过滤后的跨表版本比较、多张冻结表继续查找、有效墓碑、相同版本的来源优先级、全部取消/空表/缺失 key，以及两次独立查询之间更新回滚状态。get 和 get_record 均核对预期 value 和真实版本号；get_record 还验证返回副本不随后续更新与清空而改变。

已完成 get_record：声明增加可选回调，在两把读锁内调用 `get_(key, tranc_id, is_aborted)` 并复制返回结果；未命中返回 nullopt，有效墓碑保留为 {"", 真实版本号}。

已完成 get_batch：声明增加可选回调，一次获取两把读锁，逐 key 调用 get_ 并传入回调，锁内复制每个结果；保留输入顺序、重复 key、未命中位置及有效墓碑，空输入直接返回。新增测试核对跨表回滚后的明确预期，并与逐条 get_record 比较；同时覆盖历史读取、空表/空输入/全部取消、默认回调行为和副本稳定性。

已核对 SST 点查询调用链：SST::get → SstIterator::seek（由构造函数调用）→ 按 key 构造 BlockIterator → Block::get_idx_binary → adjust_idx_by_tranc_id。adjust_idx_by_tranc_id 已支持可选回调、越界判定、回到同 key 组首、寻找满足读取上限且未回滚的首个版本，无候选返回 -1，保留有效墓碑。上限 0 仅在没有回调时直接返回组首；有回调仍须筛选。

已完成 get_idx_binary：声明增加可选回调，保留 [left, right) 二分查找；命中 key 后将回调传给 adjust_idx_by_tranc_id，将 -1 转成 nullopt，否则返回记录下标。上限 0 的注释已补充“无回调”的条件。

已完成 get_value_binary：回调传给 get_idx_binary，判空后通过 offsets 读取 value；有效墓碑返回有值的空字符串 optional，未命中返回 nullopt。

第③阶段已完成 BlockIterator 按 key 构造的首次定位：在现有 keep_all_versions 参数之后增加可选回调，转发给 get_idx_binary；命中后保存下标，未命中指向 block->size()。该构造要求有效 Block，空指针用 invalid_argument 明确拒绝。当时回调仅用于构造定位，Block 24/24、SST 原有回归 16/16 通过；第④阶段现已补齐成员保存，后续 ++ 的回滚筛选也通过测试。

已完成 SstIterator::seek：回调传给 BlockIterator，正常未命中和异常后的状态均一致；catch (...) 清理后使用 throw 原样上报。回调只用于本次定位，不保存用于后续 ++。

已完成 SstIterator 按 key 构造函数：在 keep_all_versions 后增加可选回调；有 SST 时调用 seek(key, is_aborted)，空 SST 保持 end 状态，原有调用兼容。回调仅用于构造时定位，不保存到迭代器。

已完成 SST::get：声明新增第三个可选回调参数，保留 key 范围和 Bloom 快速排除，将回调作为 SstIterator 按 key 构造函数的第五个参数传入，第四个 keep_all_versions 保持原默认 false。命中的有效墓碑保留给上层比较；读取上限 0 也继续传递回调。公开入口已验证取消普通值/墓碑后找到有效旧值、保留有效墓碑、读取上限、全部取消/缺失、范围外查询及异常传递。

已完成 LSMEngine::update_best_from_sst：保留 private static，在 best 参数后增加可选 is_aborted 回调，并调用 sst->get(key, tranc_id, is_aborted)。未命中保持 best，命中后使用 get_cur_tranc_id 比较；仅更大版本替换，相同版本保留先遇到的来源，有效墓碑仍参与比较。本函数不获取事务管理器、不重复获取 ssts_mtx；调用方保证 SST 有效并持有集合锁。非空回调已通过本轮 sst_get_ 集成测试。

已完成 LSMEngine::sst_get_：增加第三个可选回调参数，两处候选查询均传递同一回调；L0 逐表比较，L1+ 每层二分唯一候选，再继续更深层。用户保留原有 find 后使用 operator[] 的写法，在调用方持锁且已经确认 L0 存在的条件下不影响当前结果；不要求照抄草案的局部写法。保留有效墓碑、未命中日志和既有默认参数调用。

已完成 LSMEngine::get 行为验收：必须有有效管理器，不再兼容未绑定查询，也不再使用 owner_before；共享指针由回调持有。两条查询路径正确传入回调，读取上限 0 仍过滤回滚版本。新增测试覆盖未绑定/失效拒绝、绑定后普通查询、真实取消新增/修改/删除、活跃表→冻结表→SST、有效旧墓碑、跨来源比较、同版本来源优先级、连续取消与历史上限。缺少管理器的两种情况均报错；旧注释“允许独立使用 Engine”“未绑定保留空回调”与实现相反，已提醒用户清理，异常文案建议用 unavailable。

已完成 LSMEngine::get_batch：与 get 使用相同的管理器前置条件，先检查管理器，再让空输入返回空结果；整个批次共享一个持有管理器的回调。一次调用 memtable.get_batch 取得保留墓碑的候选，再在一次 SST 读锁内逐 key 调用 sst_get_ 并传回调，按真实版本比较，最后处理墓碑。保留输入顺序、重复 key 和未命中位置。相关 48/48 通过，批量结果核对明确预期并与逐条 get 比较。

已通过的 SkipList 验收：取消值/墓碑后找到旧版本、连续取消且不改变物理记录、保留有效墓碑、同时遵守版本上限、全部取消/空表/缺失 key 边界，以及未提供回调时保持原行为。完整回归同时覆盖原有 MVCC、遍历、谓词与物理删除行为。

## 本轮验收场景约定

- 除专门的并发场景外，下面的查询均在 `abort()` 成功返回之后开始；已经返回给调用者的旧结果不属于本次查询的撤回范围。
- 下面用 `k@5` 表示 key 为 k、实际写入版本为 5。旧版本恢复必须核对 value 和真实版本号，不能只比较字符串。

## ① 保存并查询回滚 ID

- [x] 添加 `aborted_tranc_ids_` 集合，使用现有 `mutex_` 保护。
- [x] 实现 `is_aborted(uint64_t) const`，不向调用方暴露可修改的集合引用。
- [x] 修改 `add_ready_to_flush_tranc_id()`：登记 ABORTED 时同步更新回滚集合，检查异常情况下两份状态的一致性。
- [x] 补充并运行测试：初始无回滚、提交不加入名单、取消加入、重复登记、刷盘状态清理后仍保留回滚 ID，以及并发登记与查询。

**验收标准：**

| 场景 | 必须得到的结果 |
| --- | --- |
| 新管理器，或查询未登记的 ID | `is_aborted(id)` 返回 false，查询不改变集合。 |
| 登记事务 5 为 ABORTED；事务 6 为 OP_COMMITTED | 5 返回 true，6 返回 false。 |
| 重复登记事务 5 为 ABORTED | 状态仍然正确，不影响其他 ID。 |
| 刷盘登记清理了事务 5 的待刷盘状态 | `is_aborted(5)` 仍返回 true。 |
| 多线程登记不同 ID，同时查询 | 无登记丢失；线程结束后所有目标 ID 均可查到。代码检查确认所有集合访问受锁保护。 |

额外检查登记异常路径：更新失败不能留下“已经宣称取消成功，但回滚名单缺少该 ID”的状态。本阶段通过只代表管理器能正确记住取消状态。

## ② 实现 abort，调整写入上下文

- [x] 实现 `TranContext::abort()`：检查结束状态，登记回滚，设置上下文状态并清理暂存内容。
- [x] 调整 `put()`，移除为旧值回写方案准备的查询和记录，更新说明并通过对应回归。
- [x] 独立实现 `remove()`，保留立即写入/暂存删除的隔离级别分支，并检查已结束上下文。
- [x] 完成 `rollback_map_` 清理：成员、所有业务引用及相关过时说明已删除或更新，构建及对应回归通过。
- [x] 完成第②阶段的全部验证：暂存修改取消、重复取消、已提交不可取消、管理器失效及 put/remove/get/commit 的结束检查通过；读未提交取消不改写原记录。实际隐藏需等第③、④阶段接通后验收，不计入本阶段完成范围。

**验收标准：**

| 场景 | 必须得到的结果 |
| --- | --- |
| 活跃事务第一次成功 abort | 返回 true，`isAborted == true`、`isCommited == false`，管理器能查到其回滚 ID。 |
| 暂存型事务修改旧 key、新增 key、删除 key 后 abort | 数据库中的原记录保持原样，新 key 未写入，暂存内容清空。覆盖读已提交、可重复读和 SERIALIZABLE 分支。 |
| 读未提交事务多次修改同一个 key 后 abort | 登记的是该事务 ID；不会为了恢复旧值再写入一条值或墓碑。此阶段暂不要求存储查询已经过滤它。 |
| 已回滚事务再次 abort，或已提交事务调用 abort | 返回 false，原结束状态与已提交数据不变。 |
| 已结束事务再调用 put/remove/get/commit | put/remove/get 按既定接口抛出状态错误，commit 返回 false，不能新增写入。 |
| 回滚登记失败，例如管理器已经失效 | 明确报错，不返回成功，不提前把上下文标记为已完成回滚。 |

代码检查还需确认 `rollback_map_` 已无残留引用；创建、put/get 和现有提交测试不能因清理旧方案而退化。SST 中的回滚效果留到第③阶段验证。

## ③ 接通单条与批量查询

- [x] 接通 `SkipList::get` 的回滚判断参数；修正读取上限条件，通过完整 SkipList 回归（29/29）。
- [x] 修改 `MemTable::cur_get_` 的声明和实现，正确转发回调；已通过公开 get 的回滚集成测试。
- [x] 修改 `MemTable::frozen_get_` 的声明和实现，逐表传递回调并保留跨表版本比较；已通过公开 get 的回滚集成测试。
- [x] 修改 `MemTable::get_` 的声明和实现，向两侧传递同一回调；已通过公开 get 的回滚集成测试。此前提交回归 10/10 通过。
- [x] 接通公开 `MemTable::get`，补充 7 个回滚测试；完整 MemTable 回归 37/37 通过。
- [x] 接通 `MemTable::get_record`，在读锁内复制筛选后的记录；扩充跨表测试并新增副本与空结果测试，MemTable 38/38 通过，上层调用构建通过。
- [x] 接通 `MemTable::get_batch`，新增 3 个测试验证批量结果、逐条查询一致性及副本稳定性；完整 MemTable 41/41 通过，上层调用构建通过。
- [x] 修正 `Block::adjust_idx_by_tranc_id` 的循环条件；已通过 get_idx_binary 的非空回调集成测试。
- [x] 接通 `Block::get_idx_binary`，新增 5 个回滚测试；完整 Block 23/23 通过。
- [x] 修正 `Block::get_value_binary` 漏传回调的调用；扩展已有回滚测试到取值入口，完整 Block 23/23 通过。
- [x] 接通 BlockIterator 按 key 构造函数的回调，未命中定位到块尾；Block 24/24、SST 原有回归 16/16 通过（仅构造时点查）。
- [x] 完成 SstIterator::seek 验收：回调传递、正常未命中及异常状态一致，标准/非标准异常均重新抛出；完整 SST 19/19 通过。
- [x] 接通 SstIterator 按 key 构造函数的回调，扩展已有 3 个回滚测试，完整 SST 19/19 通过。
- [x] 接通 SST::get 的回调，扩展公开入口测试，完整 SST 19/19 通过，test_lsm 构建通过。
- [x] 实现并检查 LSMEngine::update_best_from_sst 的回调传递，非空回调已通过 sst_get_ 集成测试。
- [x] 接通 sst_get_ 的两处回调传递，新增 4 个跨文件/跨层回滚测试，连同原有 3 项读取回归，7/7 通过。
- [x] 实现 LSMEngine::get 的回调创建和管理器生命周期保护，验证内存/SST 共用回滚判断；相关 46/46 通过。
- [ ] 清理 get 中旧的未绑定兼容注释；此项不影响已验证的执行逻辑。
- [x] 实现 LSMEngine::get_batch 的同一回调传递，新增 2 项并扩展既有跨来源比较测试，相关 48/48 通过。
- [x] 接通 SST 点查询链：`sst_get_`、候选 SST 查询及 SST/Block 版本选择；由调用方显式提供回调的路径已通过测试。
- [x] 在 `Engine::get/get_batch` 中使用一致的回滚判断；上限 0 仍过滤回滚版本。
- [x] 确保候选被取消后继续寻找同 key 的旧版本，保留未回滚墓碑参与跨来源比较。
- [x] 测试活跃表、冻结表、SST 中的撤销新增/修改/删除，以及连续多个被取消版本、重复批量 key 和历史版本读取。

具体参数传递方式在实现相应函数时确定，沿用现有锁顺序，避免重复加锁。以局部接入为准，不把整个调用链重构作为点查修复的前提。

**验收标准：以下示例中的旧版本都有效，除特别说明外查询上限为 0。**

| 数据与操作 | 必须得到的结果 |
| --- | --- |
| `k@3 = old`，`k@5 = new`，取消 5 | 返回 `old@3`，不能返回 new 或不存在。 |
| `k@3 = old`，`k@5` 为墓碑，取消 5 | 返回 `old@3`；取消的删除操作不能继续遮住旧值。 |
| 只有 `k@5 = new`，取消 5 | 返回不存在。 |
| `k@3` 为有效墓碑，`k@5 = new`，取消 5 | 返回不存在；不能跳过有效墓碑去找更老的普通值。 |
| `k@3 = old`、`k@5 = dirty`、`k@6 = newest`，取消 5，6 已提交 | 上限 0 返回 `newest@6`，上限 5 返回 `old@3`，上限 2 返回不存在。 |
| 同 key 的两个较新版本都被取消 | 连续跳过它们，返回剩余最大可见版本；取消顺序不影响最终结果。 |

覆盖要求：上述关键场景分别落在活跃表、冻结表、SST 中，并覆盖新旧版本分属不同来源以及同一来源的情况。`get_batch` 对相同输入与逐条 `get` 结果一致，保留输入顺序、重复 key 和未命中位置；空批次正常返回。

依赖验收：Engine 的公开 get/get_batch 必须先绑定有效管理器，未绑定和失效均报错，不能悄悄当作“没有事务回滚”。现有存储及事务测试显式绑定；Lab 5.5 生产入口绑定仍未完成，测试绑定不代表该入口已接通。

## ④ 接通遍历、前缀和谓词查询

- [x] 接通 MemTable::begin：活跃表与冻结表都收集，入堆前过滤事务标记、过新及已回滚版本；完整 test_memtable 43/43 通过。
- [x] 接通 MemTable::iters_preffix：入堆前筛选标记、版本上限和回滚状态，新增 3 个用例，完整 test_memtable 46/46 通过。
- [x] 接通 MemTable::iters_monotony_predicate：直接传递支持空字符串的单调谓词，收集时排除标记及不可见/已回滚版本，堆归并后判断空结果；完整 MemTable 49/49。
- [x] 接通 BlockIterator 筛选函数及两种构造的回调保存；首次定位、后续 ++ 与两种遍历模式通过测试，完整 Block 30/30、SST 19/19。
- [x] 接通 Block::get_monotony_predicate_iters 的回调，让区间两端采用相同的版本筛选规则；完整 Block 32/32、SST 19/19 通过。
- [x] 接通 Block::iters_preffix 的回调，复用已经验收的谓词区间查询；新增 2 个测试，完整 Block 34/34、SST 19/19 通过。
- [x] 接通 Block::begin 的回调，复用按下标构造的首次定位和后续 ++ 筛选；新增 1 个测试，完整 Block 35/35、SST 19/19 通过。
- [ ] 接通 Block/SST 查询迭代器，并检查 Heap、Concact、Level 等调用链的过滤位置与参数传递。
- [ ] 接通 Engine 的 `begin` 和 `lsm_iters_monotony_predicate`。
- [ ] 确保过滤发生在同 key 去重及墓碑删除整组记录之前，不因回滚而误丢有效旧版本。
- [ ] 核查刷盘和 compaction 对原始版本的保留，区分查询过滤与物理清理。
- [ ] 测试内存与 SST 的范围结果一致、回滚墓碑不遮住旧值、全部版本被取消时范围为空。

已检查 BlockIterator::skip_by_tranc_id：类中按值保存默认空的 is_aborted_；函数先清理缓存、处理空块，再判断是否可以直接返回。读取上限与回滚状态条件正确，逐条移动索引并保留有效墓碑。用户的 const auto& version 绑定返回的 uint64_t 临时值，生命周期覆盖当前循环体，不构成悬空引用。完整 Block 24/24、SST 19/19 通过；本轮没有改业务代码或测试断言。

已完成 BlockIterator 按下标构造函数：第五个参数接收默认空的 is_aborted，初始化列表复制到 is_aborted_，然后调用 skip_by_tranc_id 完成首次定位；后续 ++ 自动使用同一成员。保留旧调用方式，已对照作者参考，沿用“初始化后筛选”的结构，只补回滚判断的接入。新增 4 个测试，完整 Block 28/28、SST 19/19 通过。

已完成 BlockIterator 按 key 构造：初始化列表复制 is_aborted_，首次定位沿用原参数调用 get_idx_binary；未命中到块尾、空 Block 抛异常及默认空回调约定保留。头文件已改为说明保存回调。新增 2 个测试验证构造后的 ++，完整 Block 30/30、SST 19/19 通过。

已完成 Block::get_monotony_predicate_iters：声明提供默认空回调，左右两个 BlockIterator 都传入 false 和 is_aborted；保留原二分逻辑，不额外包装用户谓词。2 个新增用例通过：筛选后的右边界与 ++ 一致，全部取消可返回 begin == end，未匹配 key 返回 nullopt，有效墓碑保留。完整 Block 32/32、SST 19/19 通过。

已完成 Block::iters_preffix：声明的第三个参数提供默认空回调，实现正确转发给 get_monotony_predicate_iters；保留前缀转单调谓词，不重复二分或版本筛选。新增 2 个前缀测试通过，完整 Block 34/34、SST 19/19 通过。

已完成 Block::begin：在 tranc_id 后接收默认空回调，构造 BlockIterator(shared_from_this(), 0, tranc_id, false, is_aborted)，首次定位与后续 ++ 均筛选，默认调用保持兼容。新增 1 个测试通过，完整 Block 35/35、SST 19/19 通过。Block::end 只构造块尾，无需回调。

当前验收（2026-10-24）：SstIterator 按 key 构造已通过 is_aborted_(is_aborted) 保存回调，调用 seek(key) 完成定位。seek 声明和实现均为单参数，直接把成员回调传给 BlockIterator，不再覆盖筛选条件。include/sst/sst_iterator.h 第 52、65 行已由用户同步为“构造时保存、定位时沿用”。

测试已迁移：原有 3 个回滚测试改为构造时传回调，继续验证读取上限、有效墓碑、取消墓碑、空句柄、未命中、缓存更新，以及标准/非标准异常上报和 end 状态。原“seek 清空回调”的行为改为用独立的无回调迭代器验证；重复 seek 保留本对象规则，回调查询的取消状态变化后可读到有效旧值。新增 RollbackSeekRetainsCallbackAfterCallerReset，验证调用方清空函数对象后跨 key 定位、未命中后恢复仍过滤。完整 SST 20/20 通过；本轮未运行其他套件，业务源码及头文件由用户修改。

构建与测试日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-constructor-seek-82t_mbe9`。本轮只修改测试和协作文档。

SST 后续按函数推进：operator++、seek_first 与普通构造均已通过；SST::begin 和 sst_iters_monotony_predicate 也已通过，ConcactIterator 构造和 operator++ 也已通过，当前处理 Level_Iterator 构造。BlockIterator 两种构造已保存回调，后续操作沿用成员，无需重复配置。

已完成 operator++：用户在新块构造参数末尾传入 is_aborted_。新增 2 个测试验证跨块取消值/墓碑、连续全取消块、有效墓碑保留、普通/全版本模式、读取上限 0/5/10、缓存刷新、尾部全取消到 end、调用方清空回调以及共享块缓存的不同读取规则互不覆盖。完整 SST 22/22 通过，未运行其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-increment-rollback-bbsk_a_o`。

已完成 seek_first：用户将 is_aborted_ 传入每个 BlockIterator，继续使用已有的逐块查找逻辑。新增 2 个测试覆盖连续全取消前缀块、取消/有效墓碑、读取上限 0/6/10、普通/全版本模式、调用方清空回调、重复定位与缓存刷新、整表取消后 end 状态及空 SST 句柄。完整 SST 24/24 通过，未运行其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-seek-first-rollback-k8s3u3g7`。

已完成普通构造：用户在 keep_all_versions 后增加默认空回调参数，声明与定义一致，初始化列表保存 is_aborted_ 后调用 seek_first。新增 RollbackDefaultConstructorInitializesFilterBeforeFirstSeek，验证首次定位跳过全取消首块、取消/有效墓碑、读取上限 0/6/10、普通/全版本模式、回调独立保存与后续跨块推进、默认参数、全部取消及空句柄。完整 SST 25/25 通过，未运行其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-default-ctor-rollback-nskqalc0`。

已完成 SST::begin：声明在 keep_all_versions 后增加默认空回调，定义将三项读取条件传入普通 SstIterator 构造。用户补齐漏传的第四个参数后，完整 SST 26/26 通过。公开入口测试覆盖取消首块、取消墓碑后的旧值、保留有效墓碑、跨过全取消块、普通/全版本遍历、调用方清空回调及整表取消；保留原有默认参数测试。SST 本身不保存回调；上层显式传递仍在后续既定任务内。

已完成 sst_iters_monotony_predicate：保留元数据剪枝、跳过空区间和右边界跨块归一化，将 is_aborted 传给 Block::get_monotony_predicate_iters，并在 final_begin/final_end.emplace(nullptr, tranc_id, false, is_aborted) 时保存。普通声明和两处 friend 已同步，默认参数只在普通声明提供。新增 2 个测试覆盖单块/跨块、连续取消值与墓碑、有效墓碑、读取上限 0/6/10、回调独立保存、右边界过滤后可到达、取消尾块至全局 end、全取消/无匹配/空句柄及默认调用兼容。完整 SST 28/28 通过；Engine 传递回调仍待后续接通。

已完成 ConcactIterator 构造：声明与定义一致，按值保存 is_aborted_，创建各张 SST 起点时传入成员，保留已有逐表查找及有效墓碑。新增 ConcactRollbackConstructorSelectsFirstValidSst、ConcactRollbackConstructorHandlesEmptyResults，连同原 MvccConcactSkipsInvisibleSsts 共 3/3 通过。初始/最终空 SST 迭代器仅作占位，不读取记录，无需携带回调。

已完成 ConcactIterator::operator++：每次进入新 SST 都调用 begin(max_tranc_id_, keep_all_versions_, is_aborted_)；连续跳过无有效记录的表，全部耗尽后保留原结束态。复读当前代码确认只传三个参数；上一轮构建错误已解决。新增 ConcactRollbackIncrementPreservesFilterAcrossSsts、ConcactRollbackIncrementKeepsUnfilteredHistory，加上此前三个测试共 5/5 通过，默认无回调的全版本读取仍保留原始记录。

当前 Level_Iterator 构造草案：增加默认空的回调参数，将它分别传给 memtable.begin(max_tranc_id_, false, is_aborted)、L0 的 sst->begin(max_tranc_id_, false, is_aborted)、L1+ 的 ConcactIterator(ssts, max_tranc_id_, false, is_aborted)。保留现有读锁、真实版本比较、同版本来源优先级、事务标记和最终墓碑过滤。Level 只在构造时创建子迭代器，后续 ++ 直接推进它们，因此不增加自己的回调成员；MemTable/L0 已收集副本，Concact/SST 已自行保存回调。头文件同步声明并直接包含 functional；Engine::begin 的管理器取用和传递仍在下一步骤，默认参数维持旧调用兼容。已对照本地 reference-impl 的 Level 构造和推进路径。用户已修改，但 Concact 调用漏传 false 导致构建失败，修正后再补充三路回滚归并测试并验收。

2026-10-24 同类问题审计：检索 include/src 中全部 is_aborted 声明、赋值和调用，并核对各迭代器接口及查询转发路径。当前只有 BlockIterator 与 SstIterator 保存该回调，均在已有回调参数的构造中初始化，未发现其他普通操作重复接收并覆盖成员。BlockIterator 按 key 构造向 Block 查询传递构造参数，与当前成员规则一致，不构成重复配置。其余查询参数用于当次筛选或创建新对象。审计时发现的 ++ 跨块和 seek_first 漏传现均已通过验证，普通构造也已接通；公开遍历、范围入口及上层接通尚未完成。审计只针对该设计问题，未运行新测试，不宣称完整回滚链已完成。

验证边界：两种构造、重复 seek、++ 跨块、seek_first 和公开 SST::begin 均已通过；SST 谓词范围入口也已通过；Concact 构造和跨 SST ++ 已通过；Level 和 Engine 的回调传递仍待接通。以上是既定后续任务，不能把本次测试通过视为 SST、Engine 的完整范围回滚查询已完成。

已完成 MemTable::begin：第三个参数接收可选 is_aborted。collect 中先跳过空 key，再用 get_cur_tranc_id 取得真实版本，筛选读取上限与回滚状态；收集所有内存表，保留有效墓碑和来源优先级。查询不删除原节点；测试确认刷盘仍能读取事务标记并报告 ID。后续 getter 已允许读取标记版本，begin 的标记过滤仍保留；is_valid 及 Level/SST 的现有过滤未改动。Engine 范围入口尚未传入回调，因此本函数通过不代表完整范围回滚查询已接通。

已完成 iters_preffix：第三个参数接收可选 is_aborted，沿用 begin 的锁顺序和来源编号；每张表仅计算一次前缀右边界，在左闭右开区间收集。空 key 在 get_cur_tranc_id 前排除；收集阶段不去重、不删除有效墓碑，最终 HeapIterator 沿用默认 skip_delete=true，释放表锁后移动候选构造堆。已对照作者参考并验证本地来源优先级及统一归并方案。

iters_preffix 验收：取消新值/墓碑后返回有效旧值；跨活跃表与冻结表比较真实版本，同版本按来源优先级选择；读取上限 0 与历史上限均正确；有效墓碑隐藏对应 key；缺失前缀、空表、全部取消返回 end；空前缀遍历不暴露事务标记，普通前缀不越过右边界。

已完成 iters_monotony_predicate：保留 SkipList 直接使用 predicate 定位命中区间的结构，不增加包装。谓词可收到空 key；即使命中标记，收集时也会跳过，不作为结果返回。区间收集使用真实版本号，筛选读取上限与回滚状态，保留有效墓碑；释放锁后构造堆，再用 begin_iter.is_end() 决定是否返回 nullopt。getter 允许读取实际标记版本，因此当前先读版本、再跳过标记的顺序安全。验收覆盖范围边界、全匹配/只有标记、取消值与墓碑、历史上限和最终空结果。

2026-10-23 getter 调整已完成：只将判定改为 !current，允许读取实际标记节点的版本，保留 end 抛 runtime_error，保持 is_valid 与其他迭代器不变。新增测试覆盖普通值、墓碑、多版本事务标记、默认空迭代器、空表 begin/end 和遍历耗尽；完整 SkipList 31/31 通过。查询中的标记过滤仍需保留，未发现现有调用依赖空 key 抛异常来过滤记录。

此处过滤只在创建迭代器的收集阶段执行，返回的 HeapIterator 持有数据副本且不保存回调；后续 abort 不会自动重建已经返回的堆。新 begin 调用重新检查名单。这描述的是 MemTable 当前副本迭代器行为，不代表 SST 惰性遍历或整个数据库的快照隔离已确定。

本阶段沿用现有迭代器组合，只接通必须的回滚判断与传参。清单列出的是检查覆盖范围，不要求每个迭代器新增一份过滤循环；也不新建统一 MVCC 迭代器来替换现有体系。

**验收标准：**

| 场景 | 必须得到的结果 |
| --- | --- |
| 对固定数据执行点查、全量遍历、前缀和谓词查询 | 在各自查询范围内选出相同的有效版本；面向用户的遍历按 key 有序且无重复。 |
| 取消的最新值或墓碑与有效旧值共存 | 输出有效旧值；不能因提前去重或处理取消墓碑而漏掉整个 key。 |
| 某个块、某张 SST 的所有候选均被取消 | 能继续推进到后续有效记录，无空位置解引用、漏项或死循环。 |
| 区间内所有版本都被取消 | 全量遍历到 end；返回 optional 的范围接口返回无结果。 |
| 混有有效墓碑和内部事务标记 | 面向用户的遍历不暴露这两类记录；内部归并仍保留必要墓碑，直到完成版本选择。 |
| 同一组数据经历 flush、compaction | 有效旧版本仍可找回，查询结果与整理前一致；覆盖 L0 与 L1+ 来源。 |

代码检查确认回滚判断在去重之前生效。并发边界另行明确：迭代器创建后才发生回滚时，是否沿用创建时视图不能靠猜测；确定规则后补对应测试，不能把本阶段测试当作完整范围快照隔离的证明。

## ⑤ 接回事务读取缓存与提交检查

- [ ] 检查 `TranContext::get()` 的 `read_map_` 路径，包括缓存值、缓存不存在及混合隔离级别场景。
- [ ] 明确上述场景的可见性语义；不能仅在缓存返回前检查有值结果，就宣称缓存问题全部解决。
- [ ] 修改 `commit()` 中的 `latest_record` 查询，排除已取消版本，继续保留有效墓碑用于冲突检查。
- [ ] 测试已取消版本不会造成错误冲突，有效的更新/墓碑仍能触发应有的冲突。

**验收标准：**

| 场景 | 必须得到的结果 |
| --- | --- |
| 有效旧值 `k@3`，已取消的 `k@6`，事务 5 暂存写 k 后提交 | 不因已取消的 6 产生错误冲突。 |
| `k@6` 是有效的已提交值或墓碑，事务 5 暂存写 k 后提交 | 两种情况都拒绝提交，事务 5 的全部暂存写入均未应用。 |
| 可重复读首次缓存有效的已提交值，或有效的“不存在”结果 | 后续其他事务提交修改时，重复读仍保持第一次结果；本事务自己的 put/remove 仍优先。 |
| 其他事务先写入未提交值/墓碑，当前事务进行读已提交或可重复读，随后写事务取消 | 不把对方的未提交值/墓碑当作已提交结果读取或缓存；取消后不能残留错误的新值或“不存在”。 |
| SERIALIZABLE 单键读校验 | 使用与读取约定相符的有效版本，取消版本不造成错误比较；仍能识别应拒绝的有效数据变化。 |

**本阶段存在需要先解决的设计依赖：**回滚集合只能识别 ABORTED，不能区分“仍活跃”和“已提交”。上表的混合隔离级别场景需要明确提交状态的判断方式。不能通过“发现缓存失效就重新查询”直接宣称可重复读已经正确；相关问题解决并验证前，该验收项保持未完成。

## ⑥ 持久化状态与正常重启

- [ ] 独立实现 `write_tranc_id_file()`，保存需要恢复的事务状态和回滚 ID。
- [ ] 独立实现 `read_tranc_id_file()`，验证文件内容并恢复状态。
- [ ] 按现有 map/set 结构核查结束登记、刷盘登记、状态查询和 `TranManager` 构造函数，不把已有函数体视为已经验收。
- [ ] 正常关闭、重启后继续分配正确的新 ID，并在提供查询前恢复回滚名单。
- [ ] 测试回滚数据刷盘、compaction、正常重启后仍不可见，旧值仍可读取。

**验收标准：**

| 场景 | 必须得到的结果 |
| --- | --- |
| 新目录首次启动，以及合法状态文件的写入、读取 | 正确初始化或恢复事务状态和回滚 ID，不把已提交事务误标为回滚。 |
| 回滚的记录已进入 SST，随后正常关闭、重启 | 第一次查询就排除回滚记录；修改/删除回滚后读到旧值，新增回滚后仍不存在。 |
| 回滚 ID 已从待刷盘状态表移除，再保存、重启 | 回滚名单仍包含它；compaction 后结果也保持正确。 |
| 重启前分配过已提交、已回滚或尚未写入记录的 ID | 正常保存、重启后新 ID 大于此前所有已分配 ID，继续分配不重复。 |
| 重复保存和多次正常重启 | 状态结果稳定，回滚版本不会重新出现。 |
| 已存在的状态文件被截断或字段不合法 | 明确报告读取失败，不静默当作新数据库并重置 ID。 |

本阶段验收正常保存、正常关闭与重启。持久化实现还需明确旧状态文件的兼容或报错策略。

## 最终验收

- [ ] 运行完整的回滚相关测试，记录实际结果。
- [ ] 运行 Lab 5.2 已实现范围的事务创建、读写、提交和状态维护测试。
- [ ] 运行受影响的 SkipList、MemTable、Block、SST、迭代器及 Engine 存储层回归。
- [ ] 区分本次回归失败与后续 Lab 尚未实现的测试，不以整套 `test_lsm` 全绿作为当前唯一判断依据。
- [ ] 更新 STATE.md，列清已经完成的能力、剩余限制和下一节 Lab 的入口。

最终通过条件：六个阶段的适用验收项均有证据，没有未解释的回归失败。当前第①、②阶段已完成，第③阶段功能验收通过、旧注释待清理；第④阶段进行中，第⑤、⑥阶段待开始。里程碑 A 已达到，里程碑 B 尚未完成。
