# KopiDB 当前任务：崩溃恢复

最后更新：2026-11-12。用户已确认本轮目标与六阶段计划。

章节：目录 **5.2.3「崩溃恢复」**，文章标题 **Lab 5.5 崩溃恢复**。

**进度：66.7%（4/6 阶段按所列范围验收通过）**。A9 的 stop_wal 已验收；本轮 reset_after_clear 构建成功、新增 **4/4 通过**。当前进入 LSMEngine::clear 的错误传播修改。完整 clear 调用链仍待接通，A9 尚未整体修复；A5 仍剩 TranContext::remove。阶段内部分完成不折算百分比。

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
| ⑤ 必要接口衔接 | get → get_batch → 普通写入口、`commit(test_fail)`、A5 参数校验；必要的状态／关闭联动 | 普通写入同样受 WAL 保护；纯读取不制造事务 ID 缺口；故障点能留下可恢复的提交；非法参数在产生修改前拒绝 | 进行中：get、get_batch 及 TranContext::put 校验通过；LSM::put 相关 7/7、put_batch 新增 6/6、remove 相关 7/7 通过；remove_batch 新增 6/6、commit(test_fail) 相关 18/18 通过；当前 A9：stop_wal 3/3、reset_after_clear 4/4 通过，当前 Engine::clear（clear 原始基线 0/1） |
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

## 已验收：LSM::put 独立接入 WAL 与完成登记

位置：`src/lsm/engine.cpp:1067`，公开签名仍为 `void put(const std::string &, const std::string &)`。用户现已完成独立普通写入实现；此前隐式 RC 事务草案没有落入业务代码。

2026-11-11 用户质疑在普通 put 中调用 new_tranc。重新核对：内部包装事务并非原则错误，也不等于 RU；但本地 RC commit 还携带版本冲突检查，会让普通 put 新增同 key 并发冲突失败。复用整段提交流程不只是补日志，用户尚未接受这种语义变化，因此撤回该具体草案。

继续目标不变：普通写入必须有可恢复的 WAL 和完成登记，用户只调用 put，不承担显式事务句柄或手动 commit。下一版优先保持普通写入独立，复用管理器的 write_to_wal、结束登记等底层能力，不将完整 TranContext 及某个隔离级别作为必需依赖。WAL 中可以沿用同一 ID 的 CREATE／PUT／COMMIT 分组，这不要求一定创建 TranContext 对象。

2026-11-11 当前草案已核对调用顺序：普通一次写入作为一个自动结束的日志单元，分配独立新 ID；不创建 TranContext，也不使用 ID 0。与显式 RU 不同，普通写入已经知道本次操作和结束决定，因此一次发送 CREATE／PUT／COMMIT，再应用数据，不需要先发送 PUT、等待用户决定后再发送 COMMIT。

具体顺序：空参数检查 → 预留三条 Record 容量 → 分配 ID → 构建三条记录 → write_to_wal 强制写出整组 → Engine::put 写业务数据 → add_ready_to_flush_tranc_id(COMMITTED) → Engine::put 写完成标记。Record 构建失败且尚未调用 WAL 时，可登记 ABORTED 结算已分配的 ID，再上报；进入 WAL 调用之后，失败可能已有 COMMIT，不再使用这个取消分支。

刷盘衔接已检查：第一次 Engine::put 可能自动刷出数据，但没有本次完成标记，不会误报本次全部刷盘；登记 ready 后才写标记，标记被刷出时状态已经存在。数据先于标记，沿用 MemTable 最老表优先刷盘的顺序。LSM 不额外套锁，调用已有自行加锁接口；没有引入 RC 的版本冲突拒绝策略。WAL 确认 COMMIT 后，应用数据、状态登记或标记写入失败直接向上传播，不改成回滚。

本轮用户已按独立写入方案完成 LSM::put，无需头文件或其他业务函数联动。A5 的本函数空参数检查同时实现；实际验收结果见下文，剩余入口继续按计划逐函数处理。

用户实现后验收：成功写入有对应 CREATE／PUT／COMMIT、正确数据和完成登记；用户无需 commit；进程绕过析构退出后可恢复；参数／编码错误不修改数据，已分配的失败 ID 能结算；完成标记即使触发自动刷盘也正确登记；日志或数据应用失败向调用方报告，不静默成功，不把已确认 COMMIT 改成回滚。

## 2026-11-11 普通 put 验收结果

用户实现与既定顺序一致；只有构建 Record 之前的失败登记取消，进入 WAL 后的错误不改写结束决定。助手未修改业务代码。

新增 test_lsm.cpp 的 4 项 LSMTest.DirectPut*：真实 CREATE／PUT／COMMIT 和独立 ID、阈值为 0 时业务数据／完成标记自动刷盘后检查点推进；空参数不消耗 ID、不写日志或删除旧值；超长 Record 取消已分配 ID，后续检查点可以跨过它；普通覆盖与显式事务共用版本序列、旧事务提交冲突及历史版本读取。另回归 LSMTest.BasicOperations。

新增 test_wal.cpp 的 2 项 LSMStartupWALTest.DirectPut*：占用下一 WAL 文件制造真实轮换失败，旧值与旧日志保持、新 key 不写入；子进程 _Exit 绕过析构后，普通写入及同 key 更新可恢复，重启后继续分配新 ID，并再次正常重启验证。子进程用 threadsafe death-test 模式。

两个目标构建成功，隔离运行上述范围，最终 **5/5 + 2/2 = 7/7 通过**，每轮最终构建／运行期间相关源码与测试哈希一致。首轮 test_lsm 在新增超长测试处阻塞：助手写的测试持有 Level_Iterator 的 SST 读锁，又同线程调用 flush_all 等写锁；终止该次运行后，仅修正测试迭代器作用域，并让历史版本检查逐个销毁迭代器，原有断言保留。最终五项全部通过，不把首轮未跑完的项目算作通过。

证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-put-review-ssy4g0kf`，含 WAL 结果、首轮中断说明及最终 LSM 结果。未扩大运行全量 LSM 或后续 commit(test_fail)；没有注入内存分配失败或宣称断电持久性。A5 此时仅 TranContext::put 与 LSM::put 校验通过，其他入口仍待完成。

## 已验收：LSM::put_batch

位置：`src/lsm/engine.cpp:1131`，保留原公开签名。在单条普通写入验收后，按同一独立写入方案处理整批：不创建 TranContext、不引入 RC 冲突策略，整批共用一个新 ID。空批次直接返回；先检查全部 key/value，预留 CREATE + N 条 PUT + COMMIT 的位置，再分配 ID 和构建记录。任何 Record 构建失败时尚未调用 WAL、也没写数据，可登记该 ID 取消；整组日志确认写出后，再调用 Engine::put_batch、登记提交完成、写内部完成标记。

调用链已核对：MemTable::put_batch 在同一把活跃表写锁内按输入顺序处理，整批结束后检查冻结；同 key 同版本更新值，因此重复 key 由最后一项生效，WAL 保留原输入顺序即可。LSM 不自行套锁，也不逐条调用普通 LSM::put，否则会拆成多个 ID 和多组提交。COMMIT 确认后应用或刷盘失败继续上报，不反向登记取消；不宣称任意异常下整批自动回滚。

用户实现后验收：空批次不分配 ID／写日志；后部空参数不会导致前部写入；整批日志共用 ID、记录顺序正确、重复 key 最后值生效；后部 Record 超长不写任何数据或日志，已分配的 ID 正确结算；完成登记及自动刷盘正确；绕过析构退出后整批能够恢复。本函数已由用户实现，实际验收见下文。

## 2026-11-12 普通 put_batch 验收结果

用户实现符合已确认方案：空批次返回、全量空参数预检查、整批同一 ID、按输入顺序构建全部记录、WAL 确认后再应用数据、先登记 ready 再写单个完成标记。仅构建 Record 失败时登记取消。业务代码未由助手修改。

助手新增 test_lsm.cpp 的 4 项 LSMTest.DirectPutBatch*：空批次不消耗 ID 或写日志；后部空参数不让前部新增／覆盖生效，也不消耗 ID；同一 ID、真实 WAL 顺序、重复 key 最后值及零阈值自动刷盘后检查点推进；后部 Record 超长整批不写日志／数据，失败 ID 取消后检查点可继续推进。迭代器使用局部作用域，先释放 SST 读锁再结束 DB。

test_wal.cpp 新增 2 项 LSMStartupWALTest.DirectPutBatch*：真实 WAL 轮换失败时整批没有任何条目应用，后续批次也不能绕过故障；子进程 _Exit 绕过析构后恢复整批与重复 key 的最后值，重启后批次 ID 正确延续，并再次正常重启验证。

两个目标最终构建成功，隔离运行 **4/4 + 2/2 = 6/6 通过**，最终运行期间源码／测试哈希一致。首轮 test_wal 编译失败来自助手新增测试在 ASSERT_EXIT 宏内直接声明带逗号的模板／初始化列表；将输入批次声明移到宏外后重新构建通过，未改业务实现。没有扩大重跑其他套件或验证内存分配失败、断电持久性。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-put-batch-review-e0iphix9`。

## 已验收：LSM::remove

位置：`src/lsm/engine.cpp:1195`（本轮核对）。沿用普通 put 的独立写入流程：空 key 在分配 ID 前拒绝；预留三条 Record 后分配新 ID；构建 CREATE／DELETE／COMMIT，构建失败且未调用 WAL 时结算 ABORTED；完整日志确认写出后调用 Engine::remove、登记 COMMITTED、最后写完成标记。不创建 TranContext，不引入新的提交冲突规则或外层锁。

删除本质是写一个新版本的墓碑，不能调用会拒绝空 value 的公开 LSM::put(key, "")；底层 Engine::remove 继续允许墓碑。删除不存在 key 也允许，无需先 get。WAL 开始写入之后的异常继续上报，不进入构建失败的取消分支。现有头文件和依赖足够，助手只交付聊天草案，由用户手写。

用户实现后验收：已有 key 删除及历史版本读取；不存在 key 删除；真实 CREATE／DELETE／COMMIT 日志与独立 ID；空 key 不分配 ID／写日志；编码长度失败结算 ID；自动刷盘登记；WAL 失败不删除原值；绕过析构的异常退出后恢复删除。本函数已由用户实现，实际验收见下文。

## 2026-11-12 普通 remove 验收结果

用户实现符合独立普通删除方案：空 key 先拒绝、三条 CREATE／DELETE／COMMIT 共用新 ID、只有 Record 构建失败登记取消、WAL 确认后写墓碑、先登记 ready 再写完成标记。助手未修改业务代码。

新增 test_lsm.cpp 的 4 项 LSMTest.DirectRemove*，覆盖真实 DELETE 日志、删除已有 key 后的历史版本读取、缺失 key 重复删除、零阈值自动刷盘与检查点、空 key 不消耗 ID／修改日志、长 key 编码失败结算 ID。另回归 BasicOperations。test_wal.cpp 新增 2 项 LSMStartupWALTest.DirectRemove*，覆盖真实日志轮换失败时保留旧值，以及旧值已在 SST、删除仍在内存时 _Exit 绕过析构，恢复后旧值不复活，并验证版本延续及重新写入。

两个目标构建成功，隔离运行 **5/5 + 2/2 = 7/7 通过**；源码和测试在各自构建运行期间哈希一致，没有扩大重跑其他套件。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-remove-review-us7jp4yl`。当前只验收本函数与关联场景，不代表普通接口整合及整个阶段⑤已经完成。

## 已验收：LSM::remove_batch

位置：`src/lsm/engine.cpp:1248`。空批次直接返回；分配 ID 前检查全部 key 非空；预留 N+2 条 Record 后，为整批分配一个 ID，构建 CREATE／全部 DELETE／COMMIT。构建失败且未调用 WAL 时登记取消；一次确认日志写出后调用 Engine::remove_batch，再登记 COMMITTED，最后写一个完成标记。不逐项调用普通 LSM::remove，不额外创建上下文或套锁。

底层 MemTable::remove_batch 在同一活跃表写锁内按输入顺序写墓碑，整批结束后检查冻结；Engine 自己检查总容量并可能刷盘。重复 key 同版本只更新同一个墓碑，允许保留原输入；不存在 key 也允许删除，无需先查询。完成标记在全部墓碑与 ready 登记之后，WAL 尝试后的异常不改成取消。

用户实现后验收：空批次与后部空 key 不消耗 ID／写日志或提前删除前部 key；同一 ID 的 DELETE 顺序、重复 key 与缺失 key；后部长 key 构建失败结算 ID且不删除前部；自动刷盘登记；WAL 失败不删除数据；绕过析构退出后整批删除恢复。用户已完成实现，本轮实际验收见下文。

## 2026-11-12 普通 remove_batch 验收结果

用户实现符合整批一个 ID、一组完整 DELETE 日志和一个完成标记的方案。空批次返回、全量参数校验、构建失败结算、先 WAL 后墓碑、先 ready 后完成标记的顺序正确。业务代码未由助手修改。

新增 test_lsm.cpp 的 4 项 LSMTest.DirectRemoveBatch*：空批次不分配 ID／写日志；后部空 key 不提前删除前部数据；同 ID 的 DELETE 顺序包含重复和缺失 key，历史版本与零阈值自动刷盘检查点正确；后部长 key 编码失败整批不删除，取消 ID 后检查点继续推进。新增 test_wal.cpp 的 2 项 LSMStartupWALTest.DirectRemoveBatch*：真实 WAL 轮换失败保留整批旧值；旧值已在 SST、整批删除仍在内存时 _Exit，重启后旧值不复活，后续 ID 和再次重启正确。

两个目标构建成功，隔离运行 **4/4 + 2/2 = 6/6 通过**，各轮构建与运行期间相关源码／测试哈希一致；没有扩大重跑其他套件。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-direct-remove-batch-review-inf09t0f`。普通读写入口的日志衔接部分完成，不等于阶段⑤或整个 Lab 已完成。

## 已验收：TranContext::commit(bool test_fail)

按既定顺序继续阶段⑤恢复测试入口；A5 剩余 TranContext::remove 空 key 检查留在阶段末。本轮 commit 位于 `src/lsm/transation.cpp:301`，用户已删除提前拒绝 test_fail 的旧分支。已重读 Lab 5.5 的日志先行／恢复流程及缓存的作者 master commit，同时核对本地 reference-impl：作者对暂存型事务在 WAL 写出后跳过数据与完成标记，但仍登记提交、返回 true；RU 分支忽略此开关，正常提交。

本地采用同一测试语义：仅 test_fail=true 且非 RU 时，故意制造“COMMIT 日志已确认，但暂存数据与完成标记未应用”的状态。开关不会终止进程，不是真实崩溃；测试随后关闭重启，验证数据来自 WAL 重放。正常冲突检查和 WAL 错误处理仍执行，冲突返回 false、WAL 失败抛异常；RU 的数据已经提前写入，沿用参考忽略该开关并正常完成。

只做必要局部修改，不重写已验收的正常提交流程：删除开头的 test_fail 拒绝；在现有 try 内以 skip_apply_for_test = test_fail && !write_immediately 控制“应用暂存数据＋写完成标记”；isCommited、统一的 add_ready(COMMITTED)、容量维护与解锁仍走原路径，不通过提前返回漏掉登记。完成标记必须与数据一起跳过，否则析构刷盘可能误判该事务已落盘，重启跳过所需 WAL。登记 ready 不等于已刷盘，没有标记就不能推进本事务的刷盘检查点。

同步 transation.cpp:460–462 和 include/lsm/transaction.h:54 的说明：普通提交完整应用后返回成功；恢复测试分支返回 true 表示提交决定已记录，不能当作当前实例的数据已经应用。该局部修改已由用户手写完成，实际验收见下文。

2026-11-12 按用户要求，README 的 C++ 示例与示例后说明已补充 test_fail 的用户用法：正常业务默认 false，true 为预留恢复测试参数。当前源码仍提前拒绝 true，因此 README 如实注明未结束事务调用会抛 logic_error。开关实现并验收后，须同步这段说明，改为实际测试行为及 RU 忽略该开关的规则，不保留过期的“尚未启用”文字。

用户实现后：迁移 TranCommitTest.InvalidInputsDoNotApplyWritesOrEndTransaction 中旧的 EXPECT_THROW(commit(true))，保留缺组件检查；新增验证日志、数据未应用、标记缺失与检查点不误推进、已结束状态、RU 正常结束、冲突／WAL 错误不能绕过。恢复 LSMTest.Recover 以及 WALTest.PartialFlushRecoveryTest、ConcurrentTransactionsTest 等适用用例；真实 _Exit 测试仍独立保留，不能用正常析构重启代替真实进程退出验证。

## 2026-11-12 commit(test_fail) 验收结果

用户已正确删除旧拒绝，新增 test_fail && !RU 的 guard，同时跳过暂存数据与完成标记，保持共同的提交登记、容量维护和解锁；RU 正常完成。头文件说明已同步，助手未改业务实现。

迁移旧 InvalidInputs 测试为 MissingComponents，去掉“测试开关尚不支持”的临时期望，保留并补强缺组件、状态和记录不变检查。新增 test_lsm 的 4 项，验证 RC/RR/SER 仅写 WAL、退出 active 且不能重复结束；较新事务已刷盘不能越过测试提交的检查点缺口、重启恢复 PUT/DELETE；RU 正常完成；true 不能绕过冲突。test_wal 新增 2 项，验证真实 WAL 失败仍上报，以及 commit(true) 后 _Exit 的真实进程退出恢复。

构建 test_lsm、test_wal 成功。隔离运行 LSM 7/7、WAL 11/11，共 **18/18 通过**（新增 6、迁移旧用例 1、其他既有用例 11），包括 LSMTest.Recover、WALTest.PartialFlushRecoveryTest、ConcurrentTransactionsTest 及正常提交回归。源码／测试在各自运行期间哈希一致。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-commit-test-fail-review-ejjvvygd`。README 已移除 test_fail 尚未启用的过期说明，明确暂存型测试提交返回 true 不表示数据已应用，RU 正常提交。

## 下一项：A9 clear 与 WAL／事务状态的生命周期联动

来自本阶段明确列出的 clear／状态／关闭联动复核，不扩大为全库审计。问题事实见 [A9](AUDIT.md#a9)。新增回归 LSMStartupWALTest.ClearThenDirectWriteSurvivesProcessExit 在单线程、无事务上下文或迭代器下复现：clear 后成功写入并能当场查询，_Exit 后重启丢失新值，重启前 WAL 文件数为 0；构建成功，基线 **0/1 通过**。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-clear-wal-baseline-g92e2ndn`。此失败独立于前述 commit 的 18 项通过，不能宣称当前全部恢复场景已通过。

先在聊天交付 issue，再逐函数实施。修复方案保留现有 Engine 和 TranManager 实例，不重建组件、无需重新绑定。先由 TranManager::stop_wal 显式释放其私有 WAL，再清空存储、重置并保存状态、初始化新 WAL。已核实生产代码中 WAL 只有管理器这一处 shared_ptr 强持有，清理线程保存原始 this，因此这里的 wal.reset 会在返回前执行 WAL 析构并等待线程结束；不能把它与依赖 tran_manager_.reset 恰好销毁整个管理器混为一谈。

LSMEngine::clear 删除文件失败当前只打印日志，实施时需使失败向上传递，不能在未清空成功时继续重置状态并返回成功。LSM::~LSM 的正常 flush_all→保存状态顺序正确，但当前任一步抛异常会逃出析构；清空后的保存／初始化失败时，销毁对象可能再次触发保存错误，因此需要局部捕获并记录析构错误，显式 clear/flush 仍向调用者报告失败。flush/flush_all 无需为本次正确性修复强行更新 WAL 检查点，现有旧安全检查点只会多保留日志，不能启用“单次最大 ID 即安全检查点”的旧注释。

本次先覆盖无并发读写、无活跃事务和有效迭代器的维护调用；并发 clear 和跨文件原子 clear 不自动纳入。具体接口与调用前提在交付函数前落实。验收：clear 后旧值消失、新写有可读的 WAL；新写成功后异常退出可恢复；重复 clear／正常重启及事务 ID、刷盘进度正确；停止旧清理线程后才删除旧文件；删除／初始化失败正常上报而非继续假装成功。A7/A8 仍按原状态独立保留，不在这里扩展为任意 I/O 故障原子性保证。A9 的 stop_wal 和 reset_after_clear 已由用户实现并验收，其他函数及完整 clear 联动仍未完成。

## A9 逐函数实施顺序

本轮按用户“如何修复”的授权推进，仍由用户手写业务代码，助手逐函数验收。新增两个维护接口，配合三个既有函数的局部修改，不重写 WAL、迭代器或普通读写路径：

1. **TranManager::stop_wal()**（已验收 3/3）：没有活跃事务时释放 WAL；返回前旧清理线程和句柄已关闭。有活跃事务先报错，保持原 WAL 可用。
2. **TranManager::reset_after_clear()**（已验收 4/4）：仅在 WAL 已停止、存储成功清空后，清 ready/aborted，flushed 重建为 `{next_id - 1}`，next ID 保持单调递增。旧数据已按 clear 请求被明确丢弃，旧编号不能留下永远无法补齐的刷盘缺口。先准备新的集合，再替换状态；分配失败保证管理器状态未被部分改动，不宣称整个 clear 未发生修改。
3. **LSMEngine::clear()**：删除失败向上传递，使上层停止后续重置，不把删除失败当成功。
4. **LSM::clear()**：依次 stop_wal → Engine::clear → reset_after_clear → write_tranc_id_file → init_new_wal。
5. **LSM::~LSM()**：保持原关闭顺序，仅补析构错误捕获与日志，避免维护失败后的析构异常导致 terminate。

维护前提：调用方保证整段 clear 没有并发普通读写／事务操作，且已经销毁数据库迭代器（遍历到末尾不等于释放其 SST 读锁）；stop_wal 对活跃事务作显式检查，但 active 表为空不等于全局没有操作。当前不新增全局失败状态或并发 clear 调度器。维护失败后先处理错误、重试 clear 或销毁对象，不承诺可以继续正常开启事务；WAL 停止后，普通写入已有 write_to_wal 判空会明确失败。

**本轮只交付第 1 个函数。** 在 include/lsm/transaction.h:90 的 init_new_wal 声明后新增 stop_wal 声明；定义放在 src/lsm/transation.cpp:699 的 set_engine 之前。函数用 mutex_ 检查 activeTrans_，非空抛 logic_error，随后 wal.reset；WAL 析构／清理线程不回调管理器，不存在持管理器锁等待清理线程再等管理器锁的循环。wal 已为空时 reset 本身无操作，不分配新 ID、不保存或重置状态。

stop_wal 的后续验收：无 WAL／重复停止安全；活跃事务时拒绝且原事务仍可继续写入和提交；停止后 write_to_wal 返回 false，再初始化可写新文件且旧文件不被覆盖。线程 join 和关闭句柄通过当前唯一所有权与 WAL 析构调用链核查，不能仅凭 macOS 能删除文件就声称句柄已关。该函数现已由用户实现，本轮验收如下；A9 原始基线仍为 0/1，尚未接通 clear，不将单个维护接口通过写成整体修复完成。

## 2026-11-12 stop_wal 验收通过

声明与定义一致，活跃事务检查发生在 wal.reset 之前；重复停止或没有 WAL 时 reset 安全。当前 WAL 只有管理器一个强持有者，WAL 析构已实现通知、join 和 close，清理线程不反向访问管理器锁。业务代码由用户实现，助手未代改。

新增 test_wal.cpp 的 3 项 TranManagerWALTest.StopWal*：未初始化／重复停止不改变状态或消耗 ID；活跃 RU 事务拒绝停止后仍能继续写入并提交，持有已结束的上下文指针不影响停止；停止后拒绝写日志，再保存状态／初始化后新文件可写、旧文件保留，回滚名单、完整状态文件字节、ID 和检查点保持正确。没有通过 sleep 或删除文件猜测线程与句柄是否关闭。

构建 test_wal 成功，隔离运行 **3/3 通过**，构建运行期间相关源码、头文件和测试哈希一致。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-stop-wal-review-jhief_dw`。未重跑其他套件或尚未接通的 clear 基线。

## 已验收：TranManager::reset_after_clear

声明放在 include/lsm/transaction.h:94 的 stop_wal 后，定义放在 src/lsm/transation.cpp:723 的 set_engine 前。调用者必须保证 WAL 已停止、存储已经成功清空、无活跃事务／并发操作／存活的数据库迭代器。该函数只调整管理器内存状态，不删除数据、不保存文件、不重新初始化 WAL。

先持管理器 mutex 检查 wal 为空、activeTrans_ 为空，再读取 nextTransactionId_，拒绝 next_id=0，避免减一回绕。构造临时 set `{next_id-1}` 后，再清 readyToFlushTrancIds_ 和 aborted_tranc_ids_，与 flushedTrancIds_ 交换。保持 next ID 不变，active 表按前置条件已经为空，不强制清空它；不调用会重复获取 mutex 的公开登记／查询函数。

例：next_id=6，旧数据已被 clear 删除，那么旧 ID 1～5 不再需要恢复，新写入仍用 6，检查点设为 5。这里表示 clear 主动丢弃旧数据，不是声称旧事务全部刷进 SST；若将检查点设回 0，却保留 next_id=6，会留下无法再收到刷盘标记的 1～5 缺口。先构造新 set，使分配失败时原管理器集合不被部分清空；不宣称此时存储层仍保留旧数据。

当前状态文件格式可直接保存 next=N、flushed={N-1}、ready={}、aborted={}，不改格式。现有读取器没有额外验证 next 大于所有集合 ID，本方案自身满足该关系，不因此扩展读取器修改。

用户实现后验收：未停止 WAL／存在活跃事务时拒绝且状态不变；新库与重复重置；通过真实 clear 移除旧数据后清空 ready/aborted、保留 next ID；保存重开仍保持新进度，后续真实写入刷盘可继续推进。该函数已由用户实现，实际验收见下文。

## 2026-11-12 reset_after_clear 验收通过

用户实现正确：WAL／活跃事务／next_id 前置检查在修改之前；临时 set 先准备，再清 ready/aborted 并交换 flushed；next ID 保留，函数不做文件 I/O。助手未修改业务代码。

新增 test_wal.cpp 的 4 项 TranManagerWALTest.ResetAfterClear*：WAL 未停止时拒绝，状态、数据与日志不变；无 WAL 但有活跃事务时拒绝，活跃登记与暂存修改保持；新库／重复重置不自动保存或创建 WAL；真实 abort1→commit2+flush→commit3未刷盘→RU abort4 后，stop→Engine::clear→reset→save，保存为 40 字节，重开后旧数据不存在、ID 从 5 继续、新事务刷盘将检查点推进到 5。

构建 test_wal 成功，隔离运行 **4/4 通过**，构建／运行期间相关源码、头文件和测试哈希一致。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-reset-after-clear-review-cji6c5ba`。未注入内存分配故障或重跑未接通的 LSM::clear 基线；本轮只证明管理器重置接口和正常清空后的衔接，不把它算作 A9 整体修复。

## 下一函数：LSMEngine::clear

当前位于 src/lsm/engine.cpp:382。本轮只修正文件删除失败的传播：原 filesystem_error 分支打印日志后继续返回，必须在记录错误后 throw，阻止上层把未清空成功的存储当作成功，继续 reset_after_clear／保存新状态／创建新 WAL。保持既有内存表、SST 目录、普通文件删除和 VLog 重建流程，不重置 SST 编号或改造缓存。

调用前仍要求独占维护、无活跃事务和存活迭代器，并已停止 WAL。该步骤可能在失败前删除部分数据，不承诺原子 clear；失败交给调用方处理后重试，不能吞掉异常。VLog 的打开失败已有 FileObj::open 向上传递；目录循环已删除普通 VLog 文件，之后既有 del_vlog 的重复删除不作为新问题，不扩展 Lab 6 的实现。

用户实现后验收：正常清空内存／SST 与旧文件，保留既有 VLog 重开行为；真实目录／删除错误向上抛出，不能伪装清空成功。完整 LSM::clear 的停止、状态重置与 WAL 重开待下一函数接通；当前只交付本函数聊天草案。
