# KopiDB 当前任务：崩溃恢复

最后更新：2026-11-11。用户已确认本轮目标与六阶段计划。

章节：目录 **5.2.3「崩溃恢复」**，文章标题 **Lab 5.5 崩溃恢复**。

**进度：66.7%（4/6 阶段按所列范围验收通过）**。阶段⑤普通 get 2/2、get_batch 3/3 已通过；本轮 TranContext::put 空参数校验构建成功，新增 2 项及受影响回归合计 **7/7 通过**。下一函数为 LSM::put 接入 WAL 和自动提交，A5 其余入口校验放到本阶段末尾。阶段内部分完成不折算百分比。

## 本轮目标

重启后恢复已提交的数据，排除回滚或未提交的数据，正确延续事务 ID，并保留恢复所需的日志。

核心函数按 Lab 正文 2.1 → 2.2 → 2.3 → 2.4 推进；每轮只交付一个函数的完整实现和中文注释。业务源码／头文件由用户手写，助手负责检查、补测试和维护文档。用户说“检查并下一个”后，验收通过即可继续。

## 资料与当前起点

- 已完整阅读 [Lab 5.5](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.5-Recover.md)，核对[作者 WAL](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/wal/wal.cpp)、[事务管理器](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/lsm/transation.cpp)和[LSM 入口](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/lsm/engine.cpp)。参考只用于校正，不把已有函数体算作独立实现完成。
- 开始本轮时 `WAL::recover` 和 `LSM` 构造入口为桩；当前 recover、check_recover、init_new_wal 和 LSM 启动入口均已在本轮对应范围验收。`check_recover`、`init_new_wal` 已有参考函数体，也按本轮目标重新推导。
- 上一节 Record/WAL 与事务日志接入已验收；历史结果与能力边界见 [STATE.md](STATE.md#当前进度)。本轮测试不能用上一节通过数代替。

## 阶段与验收标准

| 阶段 | 按顺序处理的函数／工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 收集日志 | `WAL::recover()` | 按数字序号读取 WAL；跨文件按事务 ID 分组并保留各事务操作顺序；正确处理检查点、空目录和异常文件；读取不改写旧日志 | 已验收：修正后构建成功，8/8 通过 |
| ② 日志检查入口 | `TranManager::check_recover()` | 使用连续检查点调用 WAL::recover，原样返回候选记录；不改变事务状态／ID；读取失败正常上报 | 已验收：构建成功，新增 3/3 通过 |
| ③ 初始化新日志 | `TranManager::init_new_wal()` | 新 WAL 能接收写入，旧日志不被提前删除或覆盖；不能把仅重放到内存的数据误记为已刷盘 | 已验收：新增 3/3，相关回归 4/4，通过；旧日志提示文字待同步 |
| ④ 接通启动恢复 | `TranManager::restore_recovery_state(records)` → `LSM::LSM()`；复核启动／保存失败路径 | 组件绑定正确；区分提交／取消／未结束事务，恢复回滚过滤与 ID 进度；仅重放有效事务，重复正常重启结果正确；失败保留 WAL | 已验收所列范围：辅助函数 5/5，本轮启动相关 8/8；文件写失败后的自动重试边界见审计 |
| ⑤ 必要接口衔接 | get → get_batch → 普通写入口、`commit(test_fail)`、A5 参数校验；必要的状态／关闭联动 | 普通写入同样受 WAL 保护；纯读取不制造事务 ID 缺口；故障点能留下可恢复的提交；非法参数在产生修改前拒绝 | 进行中：get、get_batch 及 TranContext::put 校验通过；当前 LSM::put 的 WAL 与自动提交 |
| ⑥ 整体验收 | 恢复用例、受影响回归、文档收尾 | 正常重启、绕过析构的进程退出、回滚／未提交版本已落 SST、恢复失败保留日志均有验证；报告真实通过范围与未覆盖边界 | 待开始 |

## 具体函数范围

### 按 Lab 独立实现

1. [src/wal/wal.cpp](../src/wal/wal.cpp)：`WAL::recover()`。
2. [src/lsm/transation.cpp](../src/lsm/transation.cpp)：`TranManager::check_recover()`。
3. 同文件：`TranManager::init_new_wal()`。
4. [src/lsm/engine.cpp](../src/lsm/engine.cpp)：`LSM::LSM()`。

2026-11-09 职责修正：`check_recover()` 按 Lab §2.2 只读取并返回候选记录。状态分类、回滚集合／待处理集合恢复、ID 延续仍属于已确认恢复目标，移到阶段④的独立管理器步骤；辅助接口采用 `void TranManager::restore_recovery_state(const std::map<uint64_t, std::vector<Record>>& records)`，**这是本地拆分管理器状态恢复职责的辅助函数，不是 Lab 明列的新函数**。阶段④辅助函数已由用户实现，并在 2026-11-10 通过 5 项测试；不让 LSM 直接访问管理器私有集合，也不靠重复分配 ID 的循环推进 ID。

### 阶段⑤的必要联动

- `LSM::put()`、`put_batch()`、`remove()`、`remove_batch()`：接通普通写入的日志和结束状态；保持用户不必手动 commit 的调用方式。
- `LSM::get()`、`get_batch()`：调整当前纯读取也分配事务 ID 的路径，沿用已确认的普通查询语义。
- `TranContext::commit(bool test_fail)`：实现恢复测试故障点；不能把正常析构测试等同于真实异常退出。
- `TranContext::put()`、`remove()`，以及上述 LSM 写入口：落实已选入本轮的 [A5](AUDIT.md#a5)。批量参数在开始写入前一次检查完；底层继续允许合法墓碑和内部事务标记。进入 A5 实施前先交付 issue 草稿。
- README：入口可实际使用后同步示例说明和参数约定，不混入内部状态流水账。

### 仅联动复核

- `LSM::~LSM()`、`clear()`、`flush()`、`flush_all()`，以及 `LSMEngine::flush()`。
- 事务状态文件读写、状态登记、检查点传递及事务 ID 初始化路径。

复核不等于全部重写。若现有路径阻碍本轮恢复目标，先说明实际触发场景、所需最小修改及验收，再补入当前步骤。

## 本轮衔接规则

1. **先收集，再判断，再重放。** `WAL::recover` 返回检查点之后所有事务的记录，包括 ROLLBACK 和未结束事务；管理器需要这些信息登记失效版本、延续 ID。返回 map 不代表其中每个事务都允许重放。
2. **检查点是连续安全前缀。** 不能拿最大的零散已刷盘 ID 直接过滤全部较小事务；独立的已刷盘 ID 仍由上层进一步判断。
3. **回滚沿用版本过滤。** RU 数据可能已经在 SST 中，仅不重放该事务不足以让它失效；恢复时须接通现有回滚集合，不重新引入旧值回写。
4. **恢复期间保留旧 WAL。** 重放到内存不等于已经持久化。初始化新文件不能删除恢复仍需的记录，也不能直接把检查点设置成 next ID 减一。推进清理检查点前，必须满足数据与恢复所需事务状态的保存条件。
5. **启动时执行恢复。** 此时尚未启动新的 WAL 清理线程，也不接收并发用户操作；不为静态 recover 增加无效的对象锁。

## 阶段①方案与验收场景

`WAL::recover()` 的第一轮草案：

- 目录不存在返回空 map；空路径与现有 WAL 构造函数一致，表示当前目录。
- 只识别普通文件 `wal.<十进制数字>`，使用 uint64_t 序号，先解析再排序，避免每次比较重复解析。
- 每个文件一次读取，按 Record 的小端长度字段定位完整记录的末尾，再交给现有严格 `Record::decode` 解码。
- 崩溃可能留下不足 2 字节的长度头，或声明长度大于文件剩余字节的末条记录：报告警告，只在读取缓冲中排除这段尾部。旧日志保留后，多次重启可能再次遇到同一尾部，因此不限制为最新文件。
- 长度小于公共头、完整记录的非法类型／负载以及文件读取失败仍抛异常；不使用 catch-all 把损坏文件当作空日志。当前格式没有校验和，尾部容错不代表能识别任意字节损坏。
- 只保留 ID 大于检查点的记录，移动进相应 vector，保留完整提交／回滚记录和操作先后顺序；不更改文件内容，不在本函数登记事务状态。

本轮验收覆盖 `test/test_wal.cpp` 中的：目录／空文件、数字序号与跨文件分组、检查点边界、三类事务记录保留、尾部截断、完整记录损坏上报，以及重复收集结果和文件内容保持不变。原恢复测试的断言须核对，不能因返回空结果也会通过就计为有效验收。

阶段②～⑤的测试随对应实现补充，阶段⑥恢复此前按章节排除的相关用例。阶段①已复测通过，当前进入阶段②；保留以下首次失败记录作为历史证据。

## 2026-11-09 阶段①首次检查

用户已实现 recover。发现 `src/wal/wal.cpp:209` 将长度高字节写成 `static_cast<size_t>(valid_bytes + 1) << 8`，应读取 `data[valid_bytes + 1]`；当前位置被误当作字节值，会把正常记录误认作不完整尾部，或者导致严格解码失败。业务代码由用户修正，助手未代改；这是当前函数实现的笔误，不新增独立审计问题或修复阶段。

助手新增 7 项 Recover 用例，并修正旧 RecoverTest：原检查点大于全部日志 ID，且只遍历实际结果，空结果也会通过；现在使用真实 WAL 写入／轮换并比较完整预期 map。8 项覆盖真实日志、缺失目录与空文件、数字顺序／64 位 ID／无关文件、检查点边界、空路径、尾部截断及重复恢复、完整坏记录、序号溢出与非法目录。

`xmake build test_wal` 成功；隔离运行 `WALTest.Recover*`：**2/8 通过，6 项失败**。日志完整读取、排序分组、检查点、空路径、截断尾部和非法完整记录用例失败；不存在目录／空文件与序号／目录错误用例通过。构建与运行期间源码／测试哈希一致。未运行 LSM 恢复或其他套件，未对修正后的代码作通过声明。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-recover-review-jlfrdalz`。阶段①未验收，进度仍为 0/6；下一步先修正这一行并复测。

## 2026-11-09 阶段①复测通过

用户已将长度高字节修正为 `data[valid_bytes + 1]`。`xmake build test_wal` 成功；隔离运行相同的 `WALTest.Recover*`，**8/8 通过**，原断言未削弱。覆盖完整日志／真实轮换、分组顺序、连续检查点、尾记录截断、重复恢复及文件不被改写、完整格式错误上报等阶段①范围。构建与运行期间源码／测试哈希一致；没有扩展重跑其他套件，也未修改业务源码。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-recover-retest-duckq3l_`。阶段①验收完成。

## 阶段②职责调整与验收

2026-11-09 用户指出 Lab §2.2 的职责是调用 WAL::recover 并向上层返回结果。重新核对 Lab 与作者参考后，撤回此前将事务分类、状态恢复、ID 延续和集合副本交换全部放在本函数的草案。那些工作仍属于本轮完整恢复目标，但不因此都属于 check_recover；必要功能与额外防护也不能混称 Lab 要求。

用户已开始手写上一版，助手不覆盖业务文件；在聊天中给出完整短函数供用户替换。当前定位 `src/lsm/transation.cpp:1107`，后续复查以实际行号为准。按 Lab 推导的短函数：

1. 启动时管理器构造已恢复状态文件，取得 `get_checkpoint_tranc_id()` 返回的连续检查点。
2. 调用静态 `WAL::recover(data_dir_, checkpoint)`；此时尚未创建新 WAL，也未对外接收事务。
3. 保留必要日志信息，原样返回候选 map；不筛掉未结束／取消事务，不写入管理器集合，不更新 next ID，不删除日志。

此调用发生在启动、无并发状态修改的阶段，不为这个包装函数新增锁或容器副本。已有构造／状态读取保证 flushed 集合包含检查点。构建或测试失败不能被捕获后伪装为空结果；文件读取异常自然向上传递。

本函数验收：实际连续检查点传递正确（含 flushed={2,4} 的缺口）；原样返回提交／取消／未结束候选；调用前后管理器状态和 next ID 不变；WAL 错误正常传递。事务分类、回滚过滤和 ID 恢复的测试随阶段④独立状态恢复步骤验收。此前仅修正方案与文档；本轮已按精简职责完成以下验收。


## 2026-11-09 阶段②验收通过

用户已完成短函数：读取连续检查点、调用静态 recover、记录候选组数并原样返回；正常启动无并发状态修改，不增加不必要的对象锁／状态变更。前一版承担状态恢复的大函数已撤回，不能再按旧草案编写本函数测试。

助手在 `test/test_wal.cpp` 新增 `TranManagerRecoveryTest` 的 3 个用例：

- `CheckRecoverUsesSavedCheckpointAndPreservesCandidates`：从真实状态文件恢复 flushed={2,4}，返回 3、4、回滚 5 和未结束 6 的候选记录；重复调用不改变 next ID、flushed／ready／aborted 或日志文件。
- `CheckRecoverWithoutWalLeavesProgressUnchanged`：无 WAL 时返回空，不创建日志或消耗 ID。
- `CheckRecoverPropagatesFailureWithoutChangingState`：完整记录损坏时报告异常，文件及管理器状态保持原样。

`xmake build test_wal` 成功；隔离运行 `TranManagerRecoveryTest.CheckRecover*`，**3/3 通过**。运行期间源码／测试哈希一致；本轮没有重跑已经通过的 8 项静态 recover 用例或尚未接通的 LSM 恢复。助手未修改业务代码。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-check-recover-review-rhgmbnyf`。

## 阶段③方案与验收

位置：`src/lsm/transation.cpp:652`，保持现有声明。按 Lab §2.3 负责启动阶段创建 WAL 对象，不在此函数执行日志重放、事务状态恢复或重算 next ID。

调用前提：首次启动初始化一次；上层尚未接收用户事务，也没有正在使用的旧 WAL 对象。阶段④的 LSM 启动流程负责按顺序执行：读取候选日志 → 恢复事务状态／ID → 重放有效数据 → 成功调用 write_tranc_id_file 保存对应状态 → init_new_wal。保存失败不能继续启动 WAL 清理线程。这一先后关系必须在阶段④联调验证，不能因本函数通过就宣称已接通。

实现草案：

1. 使用 get_checkpoint_tranc_id() 取得连续检查点，不使用 get_max_flushed_tranc_id()。
2. 沿用已有配置含义：128 条记录为缓冲阈值、1 秒清理间隔、4096 字节文件轮换阈值；用局部常量表达参数，无需配置重构。
3. 调用现有 WAL 构造函数并保存 shared_ptr。构造函数已经会选择大于旧文件的序号、保留原内容，并在准备完成后启动清理线程；此处不重复扫描或删除文件。
4. 保持原 flushed／ready／aborted 集合和 next ID；不能将重放进 MemTable 当作已经写入 SST，也不能用 next ID 减一重置 flushed 集合。
5. 初始化失败正常上报；新对象成功构造后才赋给成员，沿用 shared_ptr 赋值语义。

与作者区别：作者 init_new_wal 会删除所有旧 WAL，并重置 flushed 集合。我们的重放可能仍只在内存中，因此保留恢复仍需的日志和真实刷盘进度；已被安全检查点覆盖的历史文件继续由已验收的 cleaner 按规则清理。

用户实现后验证：新目录可以写入；含旧 WAL 时新建更大序号、未被检查点覆盖的日志不丢失；已保存 flushed={2,4} 时 WAL 使用 2；初始化不改变状态集合／ID；文件初始化失败报告错误并保留旧日志。测试夹具须先保存与非零检查点对应的状态，遵守上述启动前提。此前为草案；本轮阶段③实现与验证结果见下文。


## 2026-11-09 阶段③验收通过

用户实现正确：使用连续检查点，沿用 128 条／1 秒／4096B 参数，通过 WAL 构造选择新序号；没有删除旧日志、重置 flushed 集合或修改 next ID。开头日志仍为 `Cleaning up old WAL files`，已建议替换成 `Initializing new WAL`；这是非阻塞的提示文字，助手未代改。

新增三项 `TranManagerRecoveryTest.InitWal*`：

- 保留 wal.2／wal.10，创建 wal.11 并验证实际新写入，事务进度不变。
- 保存 flushed={2,4} 及回滚状态后启动真实 cleaner；等待它删除被检查点 2 覆盖的旧文件，同时保留事务 3／4／5 的日志；完整状态编码与初始化前一致。
- 用目录占用 wal.3 制造真实文件创建失败；异常上报，旧 wal.2 与 ID／检查点保留，不能假装已经初始化。

`xmake build test_wal` 成功；隔离执行 `TranManagerRecoveryTest.InitWal*:TranManagerWALTest.*`，**7/7 通过**（新增 3、相关回归 4）。日志中的 Is a directory 是故障测试主动制造的预期错误。运行期间源码／测试哈希一致；未运行尚未接通的 LSM 启动恢复。助手未修改业务代码。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-init-wal-review-qpr1f428`。

## 阶段④辅助函数方案与验收

本函数是阶段④启动整合的依赖，用于恢复管理器状态；`check_recover` 保持已验收的日志读取／返回职责。调用链为 LSM 构造：绑定组件 → check_recover → restore_recovery_state → 跳过已结算或已取消事务并重放有效数据 → 保存事务状态 → init_new_wal。

精确位置：在 `include/lsm/transaction.h:122` 的 check_recover 声明后新增公开组件接口；定义放在 `src/lsm/transation.cpp:1146` 的 `// Lab 5.4` 注释之前。当前文件已有 `<limits>`，不需新增头文件。范围为这一处声明、一个函数体及后续 LSM 构造调用，不暴露管理器私有容器。

本轮实现草案：

1. 输入为 check_recover 返回的候选 map，按 ID 排序，每组至少有一条完整记录；启动期间尚未对外服务。空 map 直接返回，保留状态文件恢复的已有进度。
2. 从 WAL 最大 ID 计算下一可用 ID，保留已有 next ID 的较大值；最大 ID 无法加一时先报错。正常状态文件中的 next ID 已覆盖其保存的各集合，不重复扫描这些集合。取消或未结束 ID 同样不能再次使用。
3. 先推进 ID 分配器，避免后续登记异常后保存了状态却仍重复分配旧 ID。此操作不意味着事务提交或数据刷盘；恢复失败必须让启动失败，保留 WAL 供后续恢复。
4. 已在 flushed 集合中的候选 ID 保持原状态，不重新加入待处理表。其余事务：已有回滚决定保持回滚；末条为 COMMIT 者登记 OP_COMMITTED；ROLLBACK 或未结束者登记 ABORTED。
5. 复用 add_ready_to_flush_tranc_id 完成每笔事务的登记和回滚集合维护。该接口及 is_aborted 内部已有锁，外层不重复获取同一把 mutex。无需复制整份集合或自行重写登记逻辑。
6. 不修改候选记录、不推进检查点、不写 SST／状态文件或新 WAL。后续 LSM 根据 flushed 和 is_aborted 判断哪些记录实际重放。

验收范围：提交／回滚／未结束状态登记，保留已有回滚名单和零散已结算项，ID 不回退且越过取消／未结束 ID，空输入、重复调用、ID 溢出；后续结合 SST 验证取消／未完成版本已落盘时仍不可读。本轮辅助函数已按以下范围验收；阶段④整体尚未完成。


## 2026-11-10 恢复状态辅助函数检查

用户已完成声明和定义，符合当前启动时调用的设计：空输入保持状态；最大 WAL ID 无法加一时先抛异常；next ID 只前进；保留零散已结算项；复用既有登记接口将回滚／未结束事务加入回滚名单。没有向存储层写数据或在此函数保存状态文件。助手未修改业务代码。

新增五个 `TranManagerRecoveryTest.Restore*`：

- 分类提交、回滚、未结束事务；ID 越过所有候选；已提交事务等待真实刷盘通知，不能直接推进检查点。
- 保留状态文件已有回滚名单、flushed={2,4} 与较大的 next ID；重复恢复不改变结果，已结算项不重新变成 ready。
- 空候选不重置原进度或名单。
- uint64_t 最大事务 ID 在任何新登记前报错，已有状态保持。
- 直接准备已刷盘的值／墓碑，恢复取消／未结束状态后，SST 查询返回旧有效版本；新增的取消 key 不可见，内存表仍为空，证明通过过滤生效。此为组件衔接测试，不代表完整 LSM 重启入口已实现。

构建 `test_wal` 成功，隔离运行 `TranManagerRecoveryTest.Restore*`，**5/5 通过**。运行期间源码／测试哈希一致；未扩大重跑无关套件。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-restore-state-review-dmrpu6lo`。本轮使用正常生成的候选记录，不能据此覆盖 [A6](AUDIT.md#a6) 中结束尝试异常后继续追加的路径；该疑点没有被本次辅助函数实现修复。

阶段④的 `LSM::LSM()`：按此前约定绑定组件、读取候选、调用恢复状态接口、跳过已结算／已取消事务并重放、保存状态，最后初始化新 WAL。2026-11-10 已由用户实现并完成下述范围的验收。


## 阶段④ LSM 构造方案与验收

位置：`src/lsm/engine.cpp:932`，替换构造函数完整定义，保留头文件现有声明。所需头文件与前置接口均已存在。本轮仅交付此函数，不顺带修改普通读写 API、commit(test_fail)、析构或 clear。

核对依据：Lab 5.5 §2.4 和作者 master/src/lsm/engine.cpp:772 的构造流程；本地 Engine::put/remove 会自动检查容量并可能刷盘，MemTable::flush_last 将数据和完成标记写入 SST，成功后报告标记 ID。因此不能把恢复当作只修改内存而忽略刷盘登记。

实现顺序：

1. 初始化列表创建 Engine 和管理器；函数体中通过两个 setter 绑定，供事务创建、读取过滤及刷盘通知使用。
2. 调用 check_recover 读取候选，再调用已验收的 restore_recovery_state 恢复 ID 与事务状态；先建立完整回滚名单，再开始应用数据。
3. 按事务 ID 遍历候选。跳过 ID <= 当前检查点、在零散 flushed 集合中的 ID、is_aborted 为 true 的 ID。重放会调用同步刷盘并可能压缩集合，例如 {2,4,5} 在事务 3 完成后变成 {5}，4 已不再单独保存，因此不能只查 contains(4)。不新增容器副本，使用现有 getter 读取当前进度。
4. 对剩余事务按记录顺序调用底层 Engine::put/remove，沿用原事务 ID。CREATE／COMMIT 等状态记录不作为业务 key-value 写入。不能调用会分配新 ID 的 LSM 普通入口。
5. 当前事务的数据操作全部成功后，调用 Engine::put("", "", id) 补上内部完成标记，沿用正常提交的刷盘登记机制。标记必须放在该事务全部数据之后；不在恢复代码里直接 add_flushed_tranc_id。
6. 重放结束后保存完整事务状态，再调用 init_new_wal 启动新日志和清理线程。没有强制把全部重放内容刷入 SST；尚未刷盘的事务仍由保留的旧 WAL 保障下一次恢复。
7. 失败直接向上传递，构造失败时不返回可用的 LSM 对象，不捕获错误后继续初始化。

验收场景：新库可以建立事务并访问正确组件；提交事务的 PUT/DELETE 恢复、回滚／未结束版本（包括已经在 SST 中的记录）不可见；已刷盘缺口和恢复期间检查点推进；完成标记后续刷盘登记；保存成功后才启动 WAL；正常关闭与重复启动。commit(true) 的模拟故障入口和普通写入 WAL 接入仍按阶段⑤处理，不能拿尚未实现的路径当作本函数已完成能力。

以下为本轮实际构建／测试结果；这些结果不构成任意文件写失败点或断电恢复的保证。


## 2026-11-10 LSM 启动验收结果

构造函数按既定顺序实现正确。新增 test_lsm.cpp 的 4 项 StartupRecovery 用例，验证正式事务入口绑定、提交日志、PUT/DELETE 及多次重启、已有 SST 的失效版本过滤、{2,4,5} 在恢复中压缩后不重复写 4/5。test_wal.cpp 新增 3 项 LSMStartupWALTest，验证坏 WAL 停止启动、子进程 _Exit 绕过析构后恢复已提交／取消／未结束数据，以及状态保存失败时保留 WAL 且不创建新 WAL。另运行原 LSMTest.TranContextTest。

test_wal 与 test_lsm 均构建成功；隔离运行 **3/3 + 5/5 = 8/8 通过**，其中新增 7 项，原有回归 1 项。源码／测试在构建运行期间哈希一致。两个子进程测试使用 threadsafe death-test 模式，父进程资源限制不受影响；未运行 commit(true) 或依赖普通 API 预写日志的后续阶段用例。助手未修改业务代码。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-lsm-startup-review-t5pta068`。

边界：故障用例确认旧 WAL 保留，但记录到 tranc_id 从 40B 变成 16B，故障后的再次启动会受损坏状态阻碍；此项并未被测试断言为正确行为，也未被算作已修复。[A7](AUDIT.md#a7) 保存事实与后续安全替换方向。[A8](AUDIT.md#a8) 为残缺 SST 的独立未验证疑点。这些全局问题没有自动扩展成本轮构造函数重写；不得用“阶段④已验收所列范围”宣称全部 I/O 故障下恢复已完成。

## 阶段⑤普通 get 的方案与验收

位置：`src/lsm/engine.cpp:1016`，保留公开签名。按阶段⑤计划先处理 get，再处理 get_batch，逐函数推进。

输入 key，输出 optional<string>；由普通查询用户调用。普通查询不创建显式事务、不分配新的事务 ID。调用底层 engine->get(key, 0, false)：0 表示不设版本上限，false 沿用直接操作允许看到其他事务未提交修改的约定；回滚过滤和真实版本选择仍由已有 Engine 读取路径完成。

未找到返回 nullopt，找到则将结果中的 value 移动到返回值；不在门面层重复比较版本或过滤墓碑。现有普通 get 每次分配 ID，却不产生完成登记，会制造检查点无法跨过的编号缺口；取消这次分配属于已选定的公开接口衔接。

验收：普通 get 不消耗事务 ID；命中／不存在结果正确；仍能观察 RU 的未提交修改并在其取消后排除；不改显式事务 get 的隔离语义。该函数已按以下范围由用户修正并验收；未增加新的公开版本上限重载。


## 2026-11-11 普通 get 检查与复测

用户已改为 engine->get(key, 0, false)，取消事务 ID 分配。首次检查发现判空条件写成 if(result.has_value())，会在命中时返回空、缺失时解引用空 optional；已精确指出 engine.cpp:1026。用户在首轮构建／测试期间补上了 !，助手未修改业务代码。

助手新增 `LSMTest.DirectGetReturnsValuesWithoutAllocatingTransactionIds` 与 `DirectGetPreservesVisibilityAndDeletionRules`。前者验证命中／缺失及两笔真实事务之间查询不消耗 ID；后者验证 RU 未提交值／墓碑可见、刷入 SST 后回滚恢复旧值、取消新增不可见，以及暂存／已提交删除的行为。两个用例先断言合法命中；首轮旧二进制均在此断言失败，未继续执行已知可能解引用空 optional 的分支。

首轮记录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-get-review-mulx8nez`，构建成功，0/2 通过；源码哈希在期间变化，结果不能代表当前修正版本。读取最新源码确认补 ! 后重新构建，隔离运行同一过滤器 `LSMTest.DirectGet*`，**2/2 通过**；第二轮构建／运行期间源码和测试哈希一致。复测证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-get-retest-xr5h1u7k`。没有扩大重跑其他套件，业务源码由用户手写。

## 阶段⑤普通 get_batch 的方案与验收

位置：`src/lsm/engine.cpp:1035`，保留现有签名与结果格式。删除 getNextTransactionId 调用，直接使用 engine->get_batch(keys, 0)。底层只有 keys 和版本上限两个参数，不新增 require_committed 参数；现有路径已经允许未提交版本，并过滤回滚、处理最终墓碑。

按返回顺序逐项转换为 pair<key, optional<value>>，保留重复 key 和不存在项；空输入交给 Engine 得到空结果。草案可预留结果容量并移动临时批查结果的字符串以减少分配／复制，这些属于小优化，不是判定原循环有错的依据。无需修改任何迭代器、底层查询接口或头文件。

用户实现后验证：空／非空批查均不消耗事务 ID；输入顺序、重复项、不存在项保留；允许 RU 修改并正确排除取消版本和处理墓碑。不将普通批查描述为整批一致快照。本轮已经按下述范围验收，未修改任何底层查询接口或迭代器。


## 2026-11-11 普通批查验收

用户实现正确：不分配新 ID，以版本上限 0 调用 Engine；按输入顺序转换并保留缺失项／重复 key；只移动临时查询结果的字符串。新增 3 项 LSMTest.DirectGetBatch*：空／非空查询不消耗 ID、顺序／重复／缺失及重复查询值保持；RU 值／墓碑进入 SST 后取消的过滤；已提交删除在内存／SST 中均返回不存在。

`xmake build test_lsm` 成功，隔离运行新增批查测试 **3/3 通过**，构建／运行期间源码和测试哈希一致；没有重复运行已通过的单条 get 或其他套件。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-batch-review-dg6rdngt`。助手未修改业务代码。README 已同步普通查询不再分配 ID 的规则，并移除入口初始化尚未完成的过期说明；保留整体 WAL 恢复保证尚未完成的限制，没有额外声称 README 示例已重新编译运行。

## 已选问题：A5 公开写入口的空参数约束

沿用 [A5](AUDIT.md#a5) 已确认方向，进入实施前在聊天中提供可复制 issue；不是新增加一轮全库修复。issue 引用已通过 GitHub 读取核实的提交 ae156f7269ba30bd3d940f3ab5daa28b328a3053，不把本地未提交注释造成的行号变化套到远程代码上。

2026-11-11 顺序调整：保留用户已经完成的 TranContext::put 校验，验收后回到 LSM::put → put_batch → remove → remove_batch 的 WAL 和结束登记主线，再完成 commit(test_fail) 及必要状态／关闭联动。A5 不再单独打断当前主线；当前 LSM::put 草案直接带上该函数已约定的两项空参数检查，其他入口及批量预检查统一在本阶段末补齐。每轮仍只交付一个函数。

当前函数：`src/lsm/transation.cpp:114` 的 TranContext::put。保留现有结束状态检查、RU 先写 WAL 再写 Engine、其他级别暂存，以及 SERIALIZABLE 限制标记。只在结束状态检查之后、追加 Record 之前增加空 key／空 value 检查，抛 invalid_argument；空 key 保留给内部标记，空 value 为墓碑，用户删除应调用 remove。

验证要求：所有支持的当前隔离分支均拒绝非法输入；operations、temp_map、WAL 和存储数据不因非法调用而改变；合法写入与 remove 删除语义保持；已结束上下文仍按原有 logic_error 优先拒绝。既有两项显式事务测试使用 put(key, "") 表达删除，下一轮随新约定迁移为 remove(key)，保留原本的隔离／暂存／记录检查。底层 MemTable、SST 等合法空标记测试不改，不把空参数检查下放到这些层。

普通写入仍需补齐 WAL、完成标记和结束登记。此前提出复用 new_tranc(READ_OP_COMMITTED)→commit 的草案，因会引入现有 RC 的冲突拒绝策略，已在用户质疑后撤回；不把这项行为变化当作 WAL 接入的必需前提。下一版按下方约束重新推导。

首轮交付 A5 issue 与 TranContext::put 的校验草案；用户现已完成该函数的前置检查。验收结果记在下方，A5 的其他入口仍待阶段末补齐；不把整个问题标为已解决，也不将 A6 自动混入此次修改。

## 2026-11-11 TranContext::put 参数校验验收

用户在结束状态检查之后、追加 Record 之前加入空 key／空 value 检查，位置正确；非法输入不清理已有合法操作，也不改变原有 RU 立即写入和其他级别暂存规则。助手没有修改业务代码。

助手新增 2 项测试：四种当前隔离分支均拒绝空参数，operations、temp_map、真实 WAL 字节及引擎数据不变，随后仍可合法写入并提交；已提交／已取消上下文优先抛事务结束错误，显式区分 invalid_argument 与其父类 logic_error。迁移两项既有用例，从 put(key, "") 改为 remove(key)，保留原来的读缓存、墓碑、提交与结束标记断言；没有修改底层合法空值测试。

`xmake build test_lsm` 成功，隔离运行 `LSMTest.TranContextPut*:LSMTest.TranContextGetOwnWritesOverrideReadCache:TranCommitTest.AppliesWritesAndEndsOnlyOnce`，**7/7 通过**（新增 2、原有 5）。源码和测试哈希在执行期间一致；没有扩大运行其他套件。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-tran-put-validation-9gm4zsyi`。A5 仅此函数通过，其他公开入口仍待本阶段末完成，A6 未纳入本次改动。

## 当前草案：LSM::put 独立接入 WAL 与完成登记

位置：`src/lsm/engine.cpp:1067`，公开签名仍为 `void put(const std::string &, const std::string &)`。当前业务源码仍是分配 ID 后直接写 Engine，上一版隐式 RC 事务草案没有落入业务代码。

2026-11-11 用户质疑在普通 put 中调用 new_tranc。重新核对：内部包装事务并非原则错误，也不等于 RU；但本地 RC commit 还携带版本冲突检查，会让普通 put 新增同 key 并发冲突失败。复用整段提交流程不只是补日志，用户尚未接受这种语义变化，因此撤回该具体草案。

继续目标不变：普通写入必须有可恢复的 WAL 和完成登记，用户只调用 put，不承担显式事务句柄或手动 commit。下一版优先保持普通写入独立，复用管理器的 write_to_wal、结束登记等底层能力，不将完整 TranContext 及某个隔离级别作为必需依赖。WAL 中可以沿用同一 ID 的 CREATE／PUT／COMMIT 分组，这不要求一定创建 TranContext 对象。

2026-11-11 当前草案已核对调用顺序：普通一次写入作为一个自动结束的日志单元，分配独立新 ID；不创建 TranContext，也不使用 ID 0。与显式 RU 不同，普通写入已经知道本次操作和结束决定，因此一次发送 CREATE／PUT／COMMIT，再应用数据，不需要先发送 PUT、等待用户决定后再发送 COMMIT。

具体顺序：空参数检查 → 预留三条 Record 容量 → 分配 ID → 构建三条记录 → write_to_wal 强制写出整组 → Engine::put 写业务数据 → add_ready_to_flush_tranc_id(COMMITTED) → Engine::put 写完成标记。Record 构建失败且尚未调用 WAL 时，可登记 ABORTED 结算已分配的 ID，再上报；进入 WAL 调用之后，失败可能已有 COMMIT，不再使用这个取消分支。

刷盘衔接已检查：第一次 Engine::put 可能自动刷出数据，但没有本次完成标记，不会误报本次全部刷盘；登记 ready 后才写标记，标记被刷出时状态已经存在。数据先于标记，沿用 MemTable 最老表优先刷盘的顺序。LSM 不额外套锁，调用已有自行加锁接口；没有引入 RC 的版本冲突拒绝策略。WAL 确认 COMMIT 后，应用数据、状态登记或标记写入失败直接向上传播，不改成回滚。

本轮只在聊天交付完整 LSM::put 草案，由用户手写。无需头文件或其他业务函数联动；没有运行草案或声称验收通过。A5 的本函数空参数检查随此草案处理，剩余接口检查仍留本阶段末。

用户实现后验收：成功写入有对应 CREATE／PUT／COMMIT、正确数据和完成登记；用户无需 commit；进程绕过析构退出后可恢复；参数／编码错误不修改数据，已分配的失败 ID 能结算；完成标记即使触发自动刷盘也正确登记；日志或数据应用失败向调用方报告，不静默成功，不把已确认 COMMIT 改成回滚。
