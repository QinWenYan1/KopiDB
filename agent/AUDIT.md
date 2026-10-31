# KopiDB 全局问题账本

最后更新：2026-10-31。用户已确认 A1～A4 的五阶段修复计划，见 [PLAN.md](PLAN.md)。A1 已按本次单独授权由助手代修并通过验收；A2 的 issue 与管理器读取判断草案已交付，用户正在加入首个函数，尚未验收；A3～A4 待开始。

本文件持续记录全局发现，规则见 [audit-to-plan](skills/audit-to-plan/SKILL.md)。选择问题开始处理时，在 [PLAN.md](PLAN.md) 按编号展开执行步骤，并在这里更新状态和计划链接；验收后保留原条目，补充解决日期与证据。项目现状和全局约定见 [STATE.md](STATE.md)。

## 首次审计的范围与依据

核查事务上下文、提交／取消、事务状态、Engine 点查与批查、迭代器归并、SkipList／MemTable、Block／SST 构建、compaction、块缓存及文件工具的关键路径。结合当前代码、现有回归，以及 [Lab 5.2](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.2-Tranc-ID.md) 和本地 `reference-impl` 校正判断。

Lab 明确要求读已提交避免读到未提交数据，并允许本节跳过 WAL 持久化。WAL、Record 后续编解码、日志重放、崩溃恢复、Lab 5.5 入口初始化整合、后续 Redis/WiscKey 扩展不列入本次 bug 清单。已接受的同 key 版本编码超限抛异常、固定状态格式、调用方串行使用同一 TranContext 等约定保持不变。

这是对关键路径的一轮检查及针对性验证，不代表已经证明全库不存在其他缺陷。此前 247/247 是原有测试范围的通过记录；本轮扩大场景后新增了真实失败证据。

## 问题索引

以下时间保留为首次审计的粗估，尚未按具体修复方案复核；不作为当前排期或完成承诺。方案确定后再区分改动量与必要测试成本，不根据问题严重程度推算工期。

| 编号 | 状态 | 优先级 | 问题 | 最小修复范围 | 历史粗估 |
| --- | --- | --- | --- | --- | --- |
| [A1](#a1) | [已解决](PLAN.md#stage-a1) | P1 | 写文件失败仍报告成功，刷盘误移除内存数据 | StdFile::write/create、FileObj::create_and_write 的错误传递 | 45～90 分钟 |
| [A2](#a2) | [处理中：管理器读取判断](PLAN.md#stage-a2) | P1 | RC/RR/SERIALIZABLE 可以读到其他 RU 事务的未提交版本，RR 缓存可延续错误 | TranContext::get、Engine 读取策略、TranManager 状态查询的必要联动 | 2～4 小时 |
| [A3](#a3) | [已纳入计划](PLAN.md#stage-a3) | P1 | compaction 最后一批刚好落盘后，又构建空 SST 并抛异常 | LSMEngine::gen_sst_from_iter 的收尾判空 | 20～40 分钟 |
| [A4](#a4) | [已纳入计划](PLAN.md#stage-a4) | P2 | 大记录之后不同 key 无法正常换块，被提前的偏移检查拒绝 | Block::add_entry 的检查顺序，验证 SSTBuilder::add 衔接 | 20～40 分钟 |
| [A5](#a5) | 待决策（改进建议） | 待定 | 公开写入接口是否应拒绝空 key／空 value，避免误用保留标记 | LSM／TranContext 写入口及批量参数预检查 | 未估时 |

A1～A4 为已确认缺陷，确认日期为 2026-10-30。用户已确认按 A1 → A2 → A3 → A4 → 整体验收推进。A5 是接口约束改进建议，尚待决策，不计为第五项已确认缺陷，也未加入当前 PLAN。每进入一个新修复问题，先在聊天中写 issue 草稿供用户手动发布，再逐函数推进；没有自动创建远程 issue。具体执行与验收标准维护在 PLAN。

<a id="a1"></a>

### A1：写失败被当作刷盘成功

修复前位置：`src/utils/std_file.cpp:74` 的 write 无条件返回 true；同文件 `create:42` 忽略 write 结果；`src/utils/files.cpp:36` 的 create_and_write 未检查最终 sync 结果。

实际链路：文件写入／刷新失败 → SSTBuilder::build 仍返回 SST 对象 → MemTable::flush_last 将冻结表移除 → Engine 登记 SST。写成功的假象会造成数据丢失风险。

测试 `TranCommitTest.AuditFlushWriteFailurePreservesMemory` 在子进程内将普通文件长度限制为 512 字节，再通过正常 flush 写入大记录。实际输出为：`flush returned without error; memory before=8210, after=0, registered SSTs=1`。父进程资源限制不变；这不是崩溃恢复测试。

参考实现中 StdFile::write 也无条件返回 true，说明这是沿用代码的问题，不能据此认定它正确。

修复方向：逐层传递写入和刷新失败，让 build 抛异常；已有 flush_last 的成功后移除流程即可保留数据。验收：失败要上报，内存记录仍在，不登记失败的 SST；正常文件读写通过。

2026-10-31 首个函数准备：重新核对当前代码及本地 reference-impl，两者 write 都无条件返回 true。提供的草案保持 bool 接口，seekp 定位失败即返回 false，写入后返回流状态；最终流刷新检查仍由后续 create_and_write 处理。业务源码尚未修改，草案未执行。

已向 test/test_utils.cpp 添加 `FileTest.StdFileWritePreservesOtherBytes`（指定偏移覆盖并关闭重读，验证其他字节保留）和 `FileTest.StdFileWriteReportsFileSizeLimitFailure`（仅子进程限制文件长度，64 KiB 写入遇到真实 I/O 失败须返回 false）。构建 test_utils 成功；隔离临时目录运行 FileTest.*，10 项中 9 通过、1 失败，失败为后者的 write 返回 true，子进程退出码 2。未运行 Bloom、LSM 或其他套件，本结果仅是 write 修复前基线。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a1-write-baseline-or2bppwa`。

**解决日期：2026-10-31。** 用户明确授权助手代修 A1，三个文件工具函数已修改并配中文注释：write 使用 seekp 并返回真实错误状态；create 传递 write 失败，保留空文件创建及打开失败原行为；create_and_write 检查最终 sync 并抛异常。SSTBuilder、MemTable、Engine 的刷盘逻辑无需修改。另修正事务状态保存函数中“write 错误检查不充分”的过时注释，无逻辑变化。

新增 `FileTest.StdFileCreateReportsWriteFailure` 与 `FileTest.CreateAndWriteReportsIoFailures`，分别验证 create 的失败传递及 FileObj 的小／大写入失败上报；二者在修改前实际均失败，日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a1-propagation-baseline-vqfv2_v2`。

加强 `TranCommitTest.AuditFlushWriteFailurePreservesMemory`：失败后检查内存字节数、原值及版本、SST 登记与层级目录；恢复资源限制后重试成功，内存正常移除，SST 中值和版本与失败前一致。修复后四个目标均构建成功，隔离目录实际运行 **197/197 通过**：File/Bloom 15、MemTable 49、SST 28、LSM 105。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a1-validation-3h5b8e_i`，含每套件过滤器、构建日志、测试 JSON、summary.json 和源码哈希。

本轮范围：A2 的混合隔离用例、A3 的空尾部用例、A4 的换块用例仍保留且未修复，未纳入 A1 验收；11 项依赖后续入口／恢复的 LSM 用例也未运行，完整名单见日志 excluded.json。没有新增 fsync、文件替换原子性或崩溃恢复保障。

<a id="a2"></a>

### A2：未提交版本进入严格读取与缓存

章节归属：目录 **5.1.2「事务管理器」**（文章标题 Lab 5.2）正文 3.2.3 get。目录 **5.2「WAL 和崩溃恢复」**尚未开始，与本项读取隔离错误区分。

位置（2026-10-31 核对）：`src/lsm/transation.cpp:158` 的 RC 读取与 RU 共用 Engine::get(key, 0)；`:179` 的 RR 首次读取随后在 `:183` 直接缓存；`src/lsm/engine.cpp:118` 仅检查 is_aborted。

场景：已提交 k@1=old；RU 事务 2 写入 dirty 或墓碑但不提交；随后创建的 RC/RR/SERIALIZABLE 事务读取。当前会返回 dirty 或 nullopt。事务 2 取消后，RR/SERIALIZABLE 仍可返回此前缓存的错误结果。

原延期测试 `TranReadVisibilityTest.RejectsDirtyValuesAndTombstonesBeforeCaching` 已原样恢复并重新运行，失败与上述场景一致。

2026-10-31 用户已认可 A1 并要求继续，A2 开始 issue 草稿。当前调用链和历史 results.json 已复核：测试通过 new_tranc 创建所有事务，覆盖 RC/RR/SERIALIZABLE 与未提交值／墓碑的组合；首次读取返回 dirty 或 nullopt，RR/SERIALIZABLE 还会缓存错误结果。活跃集合在 new_tranc 登记、结束登记成功后移除，当前 is_aborted 只查回滚集合。本轮未重跑测试、未修改业务代码，仍未解决。

2026-10-31 用户确认 issue 并要求实现。首个草案为管理器 `should_skip_read(id, require_committed)`，具体接口、调用衔接及待补测试维护在 PLAN。再次核对本地 reference-impl：作者的 RC/RR 使用开始 ID 作为读取上限，但仅靠上限无法排除更早创建而尚未提交的 RU 事务；本地需结合事务状态。当前只交付草案，未修改业务源码，A2 仍未解决。

2026-10-31 用户提出“作者是否只假定单线程”的前提复核：当前混合隔离用例的业务操作全部在同一调用线程顺序执行，RU 事务尚未结束时再创建读事务即可触发；不需要两个线程同时访问同一上下文。重新构建 test_lsm 并只运行此用例，1 项中 1 项失败，首次读取实际 dirty、预期 old。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a2-sequential-review-i9o1i2i1`。助手未修改业务源码；用户在本轮期间开始手写管理器接口，现有 get 调用链尚未接入该接口，本次不是新增接口的验收。

参考核查边界：本地 reference-impl 的部分 Engine 函数仍是桩，不能据此声称已运行作者完整实现。本次另核对作者远程 master 的 [事务创建／读取](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/lsm/transation.cpp)、[Engine 点查](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/lsm/engine.cpp) 与 MemTable 点查，以及事务 Lab；这些已查材料中未发现“每个 DB 最多一笔活跃事务”或“隔离级别不可混用”的明确限制。作者 master 未在本地构建运行，不能把本地失败冒充作者运行结果。若额外约定整笔事务完全串行或禁止 RU 与严格读取混用，可避开本项案例，但这是比单线程更强的限制，不能擅自作为作者前提。

修复方向：根据读取事务的隔离要求筛选候选，严格读取排除其他仍在执行的事务及已回滚事务，先筛选再选择最大版本并缓存。不能简单将所有 is_aborted 改成“未提交即过滤”，否则会破坏 RU 的既定行为；也不能只在最终结果上过滤，丢失被遮住的有效旧版本。新状态查询接口的名称与签名在修复方案中确定，本审计不提前扩张公开接口。

验收：有效旧值和旧墓碑都能正确返回；自身暂存修改优先；RU 保持允许脏读；RC 能看到后续提交；RR 首次结果合法且重复读取稳定；覆盖 MemTable 与 SST。

<a id="a3"></a>

### A3：compaction 收尾误判空 builder

位置：`src/lsm/engine.cpp:874` 使用 real_size()>0 判断是否还需 build；`src/sst/sst.cpp:407` 加上了 Block::cur_size，而空 Block 也计入 2 字节条目数。

场景：合并的最后一批记录正好达到目标并生成 SST，builder 随即重置为空；循环结束后 real_size() 仍为 2，于是再调用 build，抛出 `SSTBuilder::build: Cannot build empty SST`。

测试 `TranCommitTest.AuditCompactionDoesNotBuildEmptyTail` 通过正常 put_batch/flush 触发真实 compaction，已复现上述异常。

修复方向：按当前 builder 是否实际加入记录判断收尾，不用编码预估字节数代替“有无记录”。验收：恰好完成最后一批与仍有尾部记录两种情况均成功，不多建空 SST，合并后数据和版本保留。

<a id="a4"></a>

### A4：正常换块被偏移检查抢先拒绝

位置：`src/block/block.cpp:195` 的旧块偏移检查发生在 `:205` 的容量判断之前。

场景：a 的 value 为 65530 字节，长度字段合法，第一条记录从偏移 0 开始可编码；整条记录使块内数据超过 65535 字节。接着加入不同 key 的 b，本可因容量不足返回 false，让 SSTBuilder 换新块，但当前先抛出偏移超限异常。

测试 `SSTTest.AuditDifferentKeyCanStartBlockAfterLargeEntry` 已复现 `Block::add_entry: entry offset exceeds uint16_t limit`。

修复方向：区分“本条记录无法编码”与“旧块装不下、可以换块”；调整容量及偏移检查顺序。单条长度超限仍抛异常；force_write 的同 key 版本组超限仍抛异常，不拆分同 key，不改变持久化格式。

验收：大记录后不同 key 正常进入新块并可读回；原有同 key 超限保护仍成立。

<a id="a5"></a>

### A5：明确公开写入接口对空 key／空 value 的处理策略

类型：API 约束改进建议。状态：待决策，未纳入 PLAN。记录日期：2026-10-31。

背景：用户询问误传空 key 或空 value 的结果。现有约定将空 key 保留给内部事务标记，将空 value 解释为墓碑；这不是已确认支持普通空字符串数据的接口。本条讨论是否让公开写入口主动报参数错误，不能只因缺少额外防护就判定现有实现违反已接受契约。

代码核查：`LSM::put/put_batch`（src/lsm/engine.cpp:953、958）和 `TranContext::put`（src/lsm/transation.cpp:60）没有空 key／空 value 校验；`remove` 相关入口也未拒绝空 key。空 value 会沿现有路径成为墓碑；空 key 会与保留键冲突，范围收集和 Level_Iterator 会跳过它。commit 通过 `mem.put_("", "", tranc_id_)` 写结束标记；flush_last 在 key/value 都空时收集该事务 ID。以上来自代码核查，本轮没有新增或运行测试。

建议供后续决定：在 LSM 与 TranContext 的用户写入口拒绝空 key；如需防止误把 put 空 value 当作删除，可同时拒绝公开 put/put_batch 的空 value，要求通过 remove 明确删除。错误应在修改暂存表、操作记录或存储层之前上报，例如抛 std::invalid_argument；批量接口先检查全部参数再开始写入。拒绝空 value 会改变当前允许的“put 空值即删除”行为，需要先确认，不能直接作为必修修复实施。

边界：底层仍需允许合法墓碑和内部事务标记，不把拒绝空数据的检查机械加到 SkipList、MemTable、Block 或 SST。若未来需要保存普通空字符串值，需单独讨论记录类型表示，不在当前 A2 中修改编码。

## 验证记录

- 原混合隔离用例和新增 3 个问题用例均构建成功并实际运行，4 项均复现失败；未修改业务代码或削弱断言。
- 完整 File/Bloom 工具测试 11/11、BlockCache 7/7、BlockMeta 6/6，共 24/24 通过。
- 第一轮日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-transaction-audit-9pf2t5tg`。新增 SST 测试初次使用了不存在的 fixture 路径变量，助手已改为该 fixture 的 test_data 路径；该编译错误不属于业务缺陷。
- 修正测试并补充 I/O 现场输出后的复核日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-audit-confirmation-g62hkbp9`，SST 与 LSM 两个目标构建成功，分别复现 A4、A1。
- 上一轮关键存储及 LSM 回归 247/247 的日志仍保留，本轮没有重复运行整套，也没有运行依赖尚未完成 Lab 的入口／WAL／恢复用例。
- 原 24 项延期测试已全部回到 test/test_lsm.cpp，注释和函数体 SHA-256 均与原清单一致；未使用 DISABLED_ 隐藏失败。原暂存文件已清理，当前普通 test_lsm/test_sst 包含上述待修的失败用例。
