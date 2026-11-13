# KopiDB 全局问题账本

最后更新：2026-11-11。A1～A4 已解决，A5 已纳入当前计划；A6 为异常后继续写入的代码路径疑点。启动验收新记录 A7（状态文件写失败后损坏，已观察）与 A8（残缺 SST，尚待复现）。它们没有因构造主流程测试通过而被视为解决；当前实施进度见 [PLAN.md](PLAN.md)。

本文件持续记录全局发现，规则见 [audit-to-plan](skills/audit-to-plan/SKILL.md)。选择问题开始处理时，在 [PLAN.md](PLAN.md) 按编号展开执行步骤，并在这里更新状态和计划链接；验收后保留原条目，补充解决日期与证据。项目现状和全局约定见 [STATE.md](STATE.md)。

## 首次审计的范围与依据

核查事务上下文、提交／取消、事务状态、Engine 点查与批查、迭代器归并、SkipList／MemTable、Block／SST 构建、compaction、块缓存及文件工具的关键路径。结合当前代码、现有回归，以及 [Lab 5.2](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.2-Tranc-ID.md) 和本地 `reference-impl` 校正判断。

Lab 明确要求读已提交避免读到未提交数据，并允许本节跳过 WAL 持久化。WAL、Record 后续编解码、日志重放、崩溃恢复、Lab 5.5 入口初始化整合、后续 Redis/WiscKey 扩展不列入本次 bug 清单。已接受的同 key 版本编码超限抛异常、固定状态格式、调用方串行使用同一 TranContext 等约定保持不变。

这是对关键路径的一轮检查及针对性验证，不代表已经证明全库不存在其他缺陷。此前 247/247 是原有测试范围的通过记录；本轮扩大场景后新增了真实失败证据。

## 问题索引

以下时间保留为首次审计的粗估，尚未按具体修复方案复核；不作为当前排期或完成承诺。方案确定后再区分改动量与必要测试成本，不根据问题严重程度推算工期。

| 编号 | 状态 | 优先级 | 问题 | 最小修复范围 | 历史粗估 |
| --- | --- | --- | --- | --- | --- |
| [A1](#a1) | 已解决 | P1 | 写文件失败仍报告成功，刷盘误移除内存数据 | StdFile::write/create、FileObj::create_and_write 的错误传递 | 45～90 分钟 |
| [A2](#a2) | 已解决 | P1 | RC/RR/SERIALIZABLE 可以读到其他 RU 事务的未提交版本，RR 缓存可延续错误 | TranContext::get、Engine 读取策略、TranManager 状态查询的必要联动 | 2～4 小时 |
| [A3](#a3) | 已解决 | P1 | compaction 最后一批刚好落盘后，又构建空 SST 并抛异常 | LSMEngine::gen_sst_from_iter 的收尾判空 | 20～40 分钟 |
| [A4](#a4) | 已解决 | P2 | 大记录之后不同 key 无法正常换块，被提前的偏移检查拒绝 | Block::add_entry 的检查顺序，验证 SSTBuilder::add 衔接 | 20～40 分钟 |
| [A5](#a5) | 已解决（接口约束） | 待定 | 公开写入接口校验空 key／空 value，避免误用保留标记 | LSM／TranContext 写入口及批量参数预检查 | 未估时 |
| [A6](#a6) | 待验证（代码路径疑点） | 待定 | 结束尝试失败后 put/remove 仍可能追加记录，使结束记录不再位于事务日志末尾 | TranContext::put/remove 的结束尝试检查及异常路径测试 | 未估时 |
| [A7](#a7) | 已确认，待排期 | P1 | 状态文件直接覆盖写失败会破坏旧状态，阻碍后续启动 | write_tranc_id_file 的完整写入后替换；保存失败后的重试验证 | 未估时 |
| [A8](#a8) | 待验证（代码路径疑点） | 待定 | 恢复刷盘失败可能留下正式名称的残缺 SST，阻碍下次加载 | SSTBuilder／FileObj 的文件发布与失败处理 | 未估时 |
| [A9](#a9) | 已解决（约定维护范围） | P1 | clear 删除仍打开的 WAL，后续成功写入在异常退出后丢失 | LSM::clear 与 WAL 停止／重建、事务状态重置的必要联动 | 未估时 |
| [A10](#a10) | 已复现，整体验收阻塞 | P1 | 普通写入的完成标记集中进同一 SST 块，6000 个独立 key 无法刷盘 | MemTable 刷盘、Engine 完成通知及旧 SST 标记整理 | 未估时 |

A1～A4 为已确认缺陷，确认日期为 2026-10-30。用户已确认按 A1 → A2 → A3 → A4 → 整体验收推进。A5 是接口约束改进建议，已确认随目录 5.2.3 的公开入口初始化整合处理，不计为第五项已确认缺陷，已于 2026-11-08 纳入 [PLAN](PLAN.md) 的阶段⑤，当前实现进度见 A5 条目。每进入一个新修复问题，先在聊天中写 issue 草稿供用户手动发布，再逐函数推进；没有自动创建远程 issue。具体执行与验收标准维护在 PLAN。

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

2026-11-01 管理器接口已由用户实现并通过检查；新增 3 项状态读取测试，连同 12 项相关回归全部通过。记录实际进入 SST 后回滚、后续刷盘推进检查点，should_skip_read 仍拒绝该事务；已提交事务正常放行。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a2-manager-review-91gavzcv`。Engine 与 TranContext 尚未接入，A2 仍未解决；本轮未重复运行完整混合隔离测试，也未改业务源码。

参考核查边界：本地 reference-impl 的部分 Engine 函数仍是桩，不能据此声称已运行作者完整实现。本次另核对作者远程 master 的 [事务创建／读取](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/lsm/transation.cpp)、[Engine 点查](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/lsm/engine.cpp) 与 MemTable 点查，以及事务 Lab；这些已查材料中未发现“每个 DB 最多一笔活跃事务”或“隔离级别不可混用”的明确限制。作者 master 未在本地构建运行，不能把本地失败冒充作者运行结果。若额外约定整笔事务完全串行或禁止 RU 与严格读取混用，可避开本项案例，但这是比单线程更强的限制，不能擅自作为作者前提。

修复方向：根据读取事务的隔离要求筛选候选，严格读取排除其他仍在执行的事务及已回滚事务，先筛选再选择最大版本并缓存。不能简单将所有 is_aborted 改成“未提交即过滤”，否则会破坏 RU 的既定行为；也不能只在最终结果上过滤，丢失被遮住的有效旧版本。新状态查询接口的名称与签名在修复方案中确定，本审计不提前扩张公开接口。

验收：有效旧值和旧墓碑都能正确返回；自身暂存修改优先；RU 保持允许脏读；RC 能看到后续提交；RR 首次结果合法且重复读取稳定；覆盖 MemTable 与 SST。

2026-11-01 Engine 接入验收：新参数默认保留原读取行为，显式要求已提交时正确排除活跃记录。新增 3 项场景测试及相关回归共 30/30 通过，覆盖内存与 SST 的值／墓碑、连续过滤、提交后重查和版本上限。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a2-engine-review-ebk97xy5`。TranContext::get 尚未选择读取要求，A2 仍未解决，进入最后一个函数及后续完整验收。

2026-11-01 已解决：三个函数均由用户实现，读取者的隔离级别决定是否排除活跃版本，过滤发生在选版本和缓存之前。新增完整事务场景覆盖 SST 的值／墓碑、RU 保留脏读、RC 后续提交可见、RR/SERIALIZABLE 的首次合法值与不存在缓存、自身暂存修改优先；原混合隔离失败用例已通过。适用 LSM 测试 115/115 通过，另列 11 个后续 Lab 用例及 A3，不宣称完整串行化或崩溃恢复。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a2-final-review-_5j9qx4o/a2-regression`，排除清单与源码哈希在父目录。

<a id="a3"></a>

### A3：compaction 收尾误判空 builder

位置（2026-11-01 核对）：`src/lsm/engine.cpp:889` 使用 real_size()>0 判断是否还需 build；`src/sst/sst.cpp:407` 加上了 Block::cur_size，而 `src/block/block.cpp:589` 在空 Block 中也计入 2 字节条目数。

场景：合并的最后一批记录正好达到目标并生成 SST，builder 随即重置为空；循环结束后 real_size() 仍为 2，于是再调用 build，抛出 `SSTBuilder::build: Cannot build empty SST`。

测试 `TranCommitTest.AuditCompactionDoesNotBuildEmptyTail` 通过正常 put_batch/flush 触发真实 compaction，已复现上述异常。

2026-11-01 复测：A2 回归通过后，在另一隔离目录单独运行该用例，1/1 失败，仍抛 `SSTBuilder::build: Cannot build empty SST`。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a2-final-review-_5j9qx4o/a3-baseline`。已核实远程提交 `39188fabd597a8bf15b658221806c3f2166b0460` 的 engine.cpp、sst.cpp、block.cpp 及原回归位置；在聊天中起草 issue 并提供局部记录标志方案，等待用户手写，未修改业务代码。

修复方向：按当前 builder 是否实际加入记录判断收尾，不用编码预估字节数代替“有无记录”。验收：恰好完成最后一批与仍有尾部记录两种情况均成功，不多建空 SST，合并后数据和版本保留。

2026-11-02 已解决：用户添加 has_pending_entries，并改用 get_cur_tranc_id 写入真实版本。助手扩展原回归到两种 key 及其版本，新增 TranCommitTest.CompactionPreservesTailAndSameKeyVersions，检查尾部记录、全版本及墓碑保留、历史点查和同 key 只出现在一张 SST。构建成功，适用 test_lsm 117/117 通过；未运行原 11 个后续 Lab 用例。空输入分支仅做代码检查：标志保持 false，跳过循环和收尾，返回空列表；未添加私有函数测试入口。源码在构建与测试期间未变化。日志及命令、配置、排除清单、结果和源码哈希：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a3-review-s3bcul4a`，A3 运行位于 a3-lsm-regression 子目录。助手未修改业务代码；旧 getter 注释已提示用户整理。

<a id="a4"></a>

### A4：正常换块被偏移检查抢先拒绝

位置：`src/block/block.cpp:195` 的旧块偏移检查发生在 `:205` 的容量判断之前。

场景：a 的 value 为 65530 字节，长度字段合法，第一条记录从偏移 0 开始可编码；整条记录使块内数据超过 65535 字节。接着加入不同 key 的 b，本可因容量不足返回 false，让 SSTBuilder 换新块，但当前先抛出偏移超限异常。

测试 `SSTTest.AuditDifferentKeyCanStartBlockAfterLargeEntry` 已复现 `Block::add_entry: entry offset exceeds uint16_t limit`。

2026-11-02 进入实施：构建 test_sst 并单独复测该用例，1/1 失败，仍在添加 b 时抛出上述异常。证据位于 `/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a3-review-s3bcul4a/a4-baseline`，不计为 A3 回归失败。已核实远程提交 39188fabd597a8bf15b658221806c3f2166b0460 的相关源码和测试；在聊天中交付 issue 与函数草案，业务修改及修复后验收尚未完成。

修复方向：区分“本条记录无法编码”与“旧块装不下、可以换块”；调整容量及偏移检查顺序。单条长度超限仍抛异常；force_write 的同 key 版本组超限仍抛异常，不拆分同 key，不改变持久化格式。

验收：大记录后不同 key 正常进入新块并可读回；原有同 key 超限保护仍成立。

2026-11-02 已解决：用户将容量判断移到偏移与条目数检查之前，保留最前面的单条长度检查。助手新增 BlockTest.FullBlockReturnsFalseWithoutChangingRecords，验证返回 false 不改变旧块编码、旧值仍可读取；新增 FullBlockStillRejectsUnencodableKeyAndValueLengths，验证满块也不能掩盖非法单条长度；扩展原 SST 回归检查块数为 2 及两条记录的真实版本。构建成功，完整 Block 37/37、SST 29/29 通过，旧的强制写入偏移边界和合法同 key 版本用例也通过。日志、配置、命令和结果：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a4-final-review-u_gt1j45`，两个套件各在独立目录运行。业务代码由用户修改；第 176 行旧注释仍需用户同步为“非强制写入且非空块容量不足时返回 false”，已在聊天指出，不作为功能缺陷重新记账。

<a id="a5"></a>

### A5：明确公开写入接口对空 key／空 value 的处理策略

类型：API 约束改进建议。状态：2026-11-13 已解决。2026-11-08 纳入 Lab 5.5 阶段⑤；六个公开写入口的空参数校验均已实现并验收。原记录／方向确认日期：2026-10-31。

背景：用户询问误传空 key 或空 value 的结果。现有约定将空 key 保留给内部事务标记，将空 value 解释为墓碑；这不是已确认支持普通空字符串数据的接口。本条讨论是否让公开写入口主动报参数错误，不能只因缺少额外防护就判定现有实现违反已接受契约。

实施前代码核查：`LSM::put/put_batch`（src/lsm/engine.cpp:953、958）和 `TranContext::put`（src/lsm/transation.cpp:60）没有空 key／空 value 校验；`remove` 相关入口也未拒绝空 key。空 value 会沿现有路径成为墓碑；空 key 会与保留键冲突，范围收集和 Level_Iterator 会跳过它。commit 通过 `mem.put_("", "", tranc_id_)` 写结束标记；flush_last 在 key/value 都空时收集该事务 ID。以上来自代码核查，本轮没有新增或运行测试。

已认可的方向：在 LSM 与 TranContext 的用户写入口拒绝空 key，公开 put/put_batch 拒绝空 value，通过 remove 明确删除。错误应在修改暂存表、操作记录或存储层之前上报，例如抛 std::invalid_argument；批量接口先检查全部参数再开始写入。拒绝空 value 会改变当前允许的“put 空值即删除”行为，实施时同步补充测试并更新 README。具体函数及错误处理随入口整合逐函数落实，业务代码由用户手写。

2026-11-11 已核实远程提交 ae156f7269ba30bd3d940f3ab5daa28b328a3053：TranContext::put/remove 在 transation.cpp:114／158，LSM 写入口在 engine.cpp:1055–1073，尚无上述校验。issue 草稿在聊天中交付，由用户手动发布；当前首个修改点为 TranContext::put 的前置空参数检查。

本轮进展：TranContext::put 已在操作记录和数据变化前拒绝空参数，并保留结束状态检查优先级。新增 2 项测试、迁移两项旧 put 空值删除用例为 remove，相关 7/7 通过；构建及运行期间源码／测试哈希一致。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-tran-put-validation-9gm4zsyi`。其余入口校验按最新 PLAN 放到阶段⑤末，不把这一函数通过记成 A5 全部完成。

2026-11-11 普通 LSM::put 也已拒绝空 key／空 value，位置在 ID 分配前；空值不再被当作用户删除操作。该函数编码失败的已分配 ID 会登记取消。相关参数、日志、数据、检查点及恢复测试最终 7/7 通过，详见 PLAN 与 `kopidb-direct-put-review-ssy4g0kf` 证据目录。put_batch、remove、remove_batch 及 TranContext::remove 的输入约束尚未完成，A5 仍为处理中。

2026-11-12：LSM::put_batch 已在分配 ID 和写入前检查全部空 key／value；空批次不分配 ID，后部非法参数不会应用前部合法项。相关新增 6/6 通过，证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-put-batch-review-e0iphix9`。删除入口尚待实现，A5 不标记整体完成。

2026-11-12：LSM::remove 已在分配 ID 前拒绝空 key，相关参数、DELETE 日志、数据／历史版本、检查点和恢复场景 7/7 通过。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-remove-review-us7jp4yl`。剩余 LSM::remove_batch 与 TranContext::remove 尚待补齐，A5 仍为处理中。

2026-11-12：LSM::remove_batch 已完成空批次与整批空 key 预检查；后部非法输入不分配 ID／写日志或删除前部值，相关新增 6/6 通过。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-remove-batch-review-inf09t0f`。A5 仅剩 TranContext::remove，尚不标记整体完成。

2026-11-13 A5 验收完成：TranContext::remove 最后一处空 key 检查由用户实现，新增 2 项和既有 4 项删除测试共 6/6 通过；结束状态优先、无日志／数据副作用和后续合法操作均验证。六个写入口的输入约束全部接通，README 已同步，底层合法墓碑与内部标记未被禁止。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-lab55-final-review-6nk6rqu6/a5`。本问题关闭不代表阶段⑥整体验收已经通过。

边界：底层仍需允许合法墓碑和内部事务标记，不把拒绝空数据的检查机械加到 SkipList、MemTable、Block 或 SST。若未来需要保存普通空字符串值，需单独讨论记录类型表示，不在当前 A2 中修改编码。

<a id="a6"></a>

### A6：结束尝试失败后仍可能追加 PUT/DELETE

记录日期：2026-11-10。状态：代码核查与异常路径推演，尚未做故障注入复现；未自动加入当前 PLAN 或开始修改业务代码。正常串行调用、正常提交完成后的保护已经存在，本项不把“崩溃前没有 COMMIT”误判为缺陷。

代码依据：`TranContext::commit` 先向 operations 追加 COMMIT，调用 persist_pending_wal_ 成功后才设置 isCommited（当前 transation.cpp:443、444、452）；put/remove 只检查 isCommited/isAborted，尚未检查 operations 中保留的结束尝试。commit/abort 则已经拒绝再次处理包含 COMMIT/ROLLBACK 的未完成结束尝试。

可发生的异常路径推演：RU 已成功写出 CREATE、PUT1；commit 追加 COMMIT 后，在 persist_pending_wal_ 构造未写后缀 vector（当前 transation.cpp:95）时分配失败。此时 WAL 未被调用、仍可用，已写计数未推进，isCommited 仍为 false。调用者捕获异常、释放内存后再次 put，现有检查允许把 PUT2 追加到 COMMIT 后；后缀变成 COMMIT、PUT2，随后可能按此顺序进入 WAL。该过程仅使用串行公开方法，不依赖外部直接修改 operations。

边界：真实 append/sync I/O 故障会把 WAL 标为 write_failed，后续写入被拒绝，因此不能声称任意提交失败后都会将异常顺序写入文件。内存 operations 可继续追加，与日志实际是否写出，须分别验证。

影响：当前 restore_recovery_state 草案以末条是否 COMMIT 判断提交，依赖生产路径不在结束记录后继续追加。正常流程有保障，但异常后继续使用同一上下文的规则尚未在 put/remove 中闭合；不能无条件宣称结束记录总在最后。

候选方向：写入入口在追加记录之前，拒绝已保留结束尝试的上下文；复用已有记录／状态，避免为了本项先重构整个事务模块。需补正常结束、提交／取消异常后 put/remove 的拒绝和记录不变测试，并验证预写日志之前失败且 WAL 仍可用的路径。选入修复时先交付 issue 草稿，再明确最小改动；本条不代表修复已完成。

<a id="a7"></a>

### A7：覆盖事务状态文件失败后破坏旧状态

记录日期：2026-11-10。已确认写失败会破坏旧文件；故障解除后的再次启动将被长度校验拒绝，后者为结合实际文件长度和读取代码的推导，尚未额外执行重启探针。未自动扩展为本轮构造函数修复，不将此问题算作已解决。

write_tranc_id_file（transation.cpp:777）先以 create=true 清空正式 tranc_id 文件，再写入／刷新（781）。实际子进程故障测试将文件长度限制为 16 字节并忽略 SIGXFSZ：初始状态文件 40B，恢复保存失败后剩 16B；WAL 内容保持，新 WAL 未创建。read_tranc_id_file:800 会拒绝小于 32B 的文件，下一次启动在管理器构造阶段就会失败，尚未进入 WAL 恢复。

测试 `LSMStartupWALTest.StateSaveFailurePreservesWalAndStopsInitialization` 通过的是“报告保存失败、保留旧 WAL、不启动新写入器”，并以 RecordProperty 记录 state_bytes_before=40、state_bytes_after=16。不能把这项通过说成已验证故障解除后的自动恢复。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-lsm-startup-review-t5pta068/test_wal/results.json`。

候选方向：先完整写出临时状态文件并检查刷新，再替换正式文件；失败保留上一份有效状态。保持现有文件格式与公开接口，不借此引入版本兼容或完整断电持久性承诺。选入实施时需先交付 issue 草稿，并验证写失败／替换失败后能再次启动。

<a id="a8"></a>

### A8：SST 写失败可能留下正式名称的残缺文件

记录日期：2026-11-10。来源为对构造函数直接依赖的只读核查；仅代码推演，尚未实际注入故障，未纳入当前实施。

SSTBuilder::build（sst.cpp:503）调用 FileObj::create_and_write 直接写正式 sst_N.level 路径；文件写入／刷新失败时会报告异常，但可能已留下部分内容（files.cpp:40–50）。下一次 LSMEngine 构造先扫描并打开所有此类正式文件（engine.cpp:54–75），可能在读取 WAL 前被坏 SST 挡住。已有“目录占用目标路径”的打开失败测试不会产生部分 SST，不能覆盖此情况。

待验证：低内存阈值触发恢复期间刷盘，在子进程限制文件大小制造部分 SST；解除故障后检查再次启动。若成立，评估完整写入后再发布正式文件名的方案；与 A7 同属文件失败后的完整性边界，但调用点和验证不同，不宣称现有 WAL 保留机制已解决它。

<a id="a9"></a>

### A9：clear 后成功写入仍使用已删除的 WAL，异常退出丢失新值

记录日期：2026-11-12。类型：已复现缺陷。状态：2026-11-13 已解决（独占维护范围）。已按 Lab 5.5 PLAN 阶段⑤完成必要 clear／状态／关闭联动，issue 已在聊天交付，五个函数均由用户实现并验收。与 A7/A8 的文件失败原子性问题分别记录。

已核实远程提交 `71c9d71bf584a69207d8529a4e5a1c583a70b75b`：[LSM::clear](https://github.com/QinWenYan1/KopiDB/blob/71c9d71bf584a69207d8529a4e5a1c583a70b75b/src/lsm/engine.cpp#L1313) 只调用 Engine::clear；[Engine::clear](https://github.com/QinWenYan1/KopiDB/blob/71c9d71bf584a69207d8529a4e5a1c583a70b75b/src/lsm/engine.cpp#L371-L397) 删除目录全部普通文件，包括 wal.* 和 tranc_id。管理器仍持有旧 WAL 与清理线程，文件句柄没有在删除前关闭。macOS 本轮运行中，后续小写入经旧句柄写出并返回成功，但目录里没有可供下次启动读取的 WAL。

复现：同一 LSM 实例 put("before-clear", "old") → clear() → put("after-clear", "new") → 当前查询确认 new → _Exit(0) → 新进程重开。全程单线程、无事务上下文和迭代器，不依赖 I/O 故障注入。旧 key 正确消失，新 key 实际也变为 nullopt；期望新值仍为 new。

新增测试 `LSMStartupWALTest.ClearThenDirectWriteSurvivesProcessExit` 构建成功、**0/1 通过**，RecordProperty 记录重启前 WAL 文件数为 0；相关源码和测试哈希在构建运行期间一致。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-clear-wal-baseline-g92e2ndn`。测试保留在原套件中，未禁用或削弱断言。该独立基线失败不归因于已通过 18 项检查的 commit(test_fail)。

修复方向：先显式停止旧 WAL／等待清理线程，再清空存储，重置并保存管理器的相关状态，最后启动新 WAL；不能只在 clear 后创建新日志而让旧线程和句柄继续存活。具体函数、维护前提和验收标准进入 PLAN。 2026-11-12 已按用户要求细化为 stop_wal、reset_after_clear、Engine::clear、LSM::clear 及 LSM 析构错误收尾五个函数。stop_wal 新增 3/3 通过，证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-stop-wal-review-jhief_dw`；clear 尚未接入该接口，不能标为整体修复完成。删除文件失败须报告，不能按清空成功继续重置状态；不因此承诺原子 clear 或并发 clear。

2026-11-12 A9 进展：reset_after_clear 新增 4/4 通过，保存重开后继续分配 ID 5、后续刷盘检查点推进到 5，旧 ready/aborted 已清理。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-reset-after-clear-review-cji6c5ba`。Engine::clear 错误传播、上层流程与析构收尾仍待完成，未重跑或宣称原失败回归已通过。

2026-11-13 A9 进展：Engine::clear 已正确重抛文件删除异常，新增 2/2 通过、无跳过。正常清空及 POSIX 0500 目录下真实删除失败均已验证，权限在测试后恢复。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-engine-clear-review-o8b2rd63`。上层 LSM::clear 尚未接通、析构收尾待完成，原始失败回归未重跑，A9 不标记已解决。

2026-11-13 A9 进展：LSM::clear 已按五步接通，原 ClearThenDirectWriteSurvivesProcessExit 保留原断言，从 0/1 转为 1/1。新增重复清空／ID 持久化、活跃事务拒绝、删除失败后恢复权限重试三项，本轮合计 **4/4 通过、无跳过**。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-lsm-clear-review-9f2p_bmx`。原始日志丢失场景已修复，析构错误收尾及相应失败场景尚待验收，未据此关闭整个 A9。

2026-11-13 A9 验收完成：LSM 析构补齐错误捕获，相关 **17/17 通过、无跳过**（WAL 10、LSM 7；新增 3 项真实故障用例）。覆盖停止与状态重置、正常／重复 clear、活跃事务拒绝、删除失败后重试、原始 _Exit 回归、状态保存失败后的异常展开、WAL 初始化失败后的销毁／修复重开、显式 flush 报错而析构不终止进程。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-clear-destructor-review-8bo00q9d`；最终运行源码／测试哈希稳定。

A9 在“调用方独占维护、无活跃事务和存活数据库迭代器”的已选范围内标为已解决；README 已补充该使用约定和清空失败可能部分删除数据的行为。不承诺并发／原子 clear、断电或任意文件损坏恢复，不包含 A7/A8 的文件发布与原子替换修复。

<a id="a10"></a>

### A10：内部完成标记集中写入 SST 块，普通多 key 写入也会偏移超限

记录日期：2026-11-13。类型：已复现缺陷，来自 Lab 5.5 阶段⑥整体验收。状态：处理中，用户已开始三函数修复；Engine::flush 首次修改尚漏掉完成 ID 汇总及空 SST 安全返回，代码检查未通过，具体步骤见 PLAN。普通写入口接通 WAL／完成登记后，每次独立写入都新增一个空 key、空 value 的完成标记，扩大了原刷盘路径未覆盖的标记数量。

远程提交 `f5cebb8e7e774d68eda7d99928084121a6c5ab81` 已通过 GitHub 读取核实：[MemTable::flush_last](https://github.com/QinWenYan1/KopiDB/blob/f5cebb8e7e774d68eda7d99928084121a6c5ab81/src/memtable/memtable.cpp#L404-L420) 收集标记 ID 后仍将标记 builder.add；[SSTBuilder::add](https://github.com/QinWenYan1/KopiDB/blob/f5cebb8e7e774d68eda7d99928084121a6c5ab81/src/sst/sst.cpp#L384-L403) 对相同 key 使用 force_write；[Block::add_entry](https://github.com/QinWenYan1/KopiDB/blob/f5cebb8e7e774d68eda7d99928084121a6c5ab81/src/block/block.cpp#L179-L212) 的数据区偏移仍只有 uint16_t。每个标记占 12B，第 5463 条起始偏移为 5462×12=65544，超过 65535。

本地新增 LSMTest.ManyIndependentWritesCanFlush：6000 个不同 key、小 value，perMem=16MiB／total=64MiB，全部写入后显式 flush_all。构建成功，实际 **0/1 通过**，抛 std::length_error，描述为 Block::add_entry: entry offset exceeds uint16_t limit。源码和测试哈希稳定。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-lab55-final-review-6nk6rqu6/marker-baseline`。不是单个业务 key 被用户写入大量版本，也不属于尚未开始的 Lab 6；这是内部标记被作为同 key 数据持久化导致的正常写入障碍。

影响：无法生成本次业务 SST；失败数据仍保留在内存，WAL 未因此被删除，不将现场描述成已发生永久数据丢失。析构会记录同类失败，单纯关闭重开查询可能依靠 WAL 重放而掩盖刷盘失败，因此必须显式验证 flush 成功。大规模 LSM／compaction 回归待修复后继续，不默默排除或记为通过。

修复准备已核对：没有生产调用从 SST 空 key 标记重建完成状态。标记 ID 由 MemTable::flush_last 收集、Engine::flush 通知；重启使用状态文件和 WAL。已选定范围为 Engine::flush 处理纯标记无 SST 的完成通知、flush_last 消费标记而不将其写入 SST，以及 gen_sst_from_iter 清理旧 SST 标记。保留非空 key 墓碑、全部业务版本、现有编码与同业务 key 共块约束；混合表构建失败时不能提前报告完成 ID。具体步骤与验收标准见 PLAN；issue 与补充 QA 已在聊天交付，由用户手动发布，修复尚未验收。

## 验证记录

<a id="final-regression-2026-11-02"></a>

2026-11-02 A1～A4 最终回归：8 个目标均重新构建成功。Block 37/37、SST 29/29 为同一轮 A4 验收结果，随后运行 SkipList 31/31、MemTable 49/49、File/Bloom 15/15、BlockCache 7/7、BlockMeta 6/6、适用 LSM 117/117；总计 **291/291 通过**，无禁用用例。本轮新增 2 个 Block 用例并扩展原 SST 回归，全部针对已确认的容量／编码约定；未改业务源码。

- 每个测试目标在独立临时目录复制 config.toml，并以绝对路径运行已构建二进制。构建／测试期间源码、测试和配置哈希未变化。
- 未运行的 11 个 LSM 用例属于 LSMTest：BasicOperations、Persistence、LargeScaleOperations、IteratorOperations、MixedOperations、MonotonyPredicate、TranContextTest、Recover、BigPersistence、BigPersistence2、SmallConfigLargeDataPersistent；这些仍依赖后续公开入口初始化／WAL／恢复工作，没有删除或禁用源码。
- test_compact 未运行：当前夹具调用 init_new_wal，相关存储 compaction 回归已由 test_lsm 验证。test_wal、test_redis、test_wisckey 不在本轮已完成模块验收范围；没有将其未运行计为失败，也没有宣称相应能力已完成。
- 证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a4-final-review-u_gt1j45`。含 8 份构建日志、各目标的配置／command.json／results.json／test.log、excluded.json、summary.json，以及前后源码哈希。
- 已核对并提示两处旧注释：block.cpp:176 的 force_write 条件写反、engine.cpp:856 的 getter 说明过时。用户负责业务文件中的注释整理，不作为新功能缺陷记账。本轮通过不代表完整串行化、完整线程安全或崩溃恢复已经实现。

以下为首次审计及历史基线：

- 原混合隔离用例和新增 3 个问题用例均构建成功并实际运行，4 项均复现失败；未修改业务代码或削弱断言。
- 完整 File/Bloom 工具测试 11/11、BlockCache 7/7、BlockMeta 6/6，共 24/24 通过。
- 第一轮日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-transaction-audit-9pf2t5tg`。新增 SST 测试初次使用了不存在的 fixture 路径变量，助手已改为该 fixture 的 test_data 路径；该编译错误不属于业务缺陷。
- 修正测试并补充 I/O 现场输出后的复核日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-audit-confirmation-g62hkbp9`，SST 与 LSM 两个目标构建成功，分别复现 A4、A1。
- 上一轮关键存储及 LSM 回归 247/247 的日志仍保留，本轮没有重复运行整套，也没有运行依赖尚未完成 Lab 的入口／WAL／恢复用例。
- 原 24 项延期测试已全部回到 test/test_lsm.cpp，注释和函数体 SHA-256 均与原清单一致；未使用 DISABLED_ 隐藏失败。原暂存文件已清理，当前普通 test_lsm/test_sst 包含上述待修的失败用例。
